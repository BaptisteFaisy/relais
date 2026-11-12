/* Relais — fault-injection campaign driver (M18).
 *
 * Runs N seeded simulated worlds, injects exactly one fine hardware fault
 * (a bit/byte/word flip in a node state machine, a control state, or an
 * in-flight frame) into the world at a chosen tick, and compares it against a
 * twin that ran identically without the fault. One JSON line per run gives the
 * target, the zone, the grain, whether the fault was detected or masked, which
 * counters moved, and whether any safety invariant tripped.
 *
 * campaign.py aggregates these lines into the published balance. The driver
 * never touches the simulator's own fault model (sim_faults.c): it mutates the
 * world state directly between ticks, which is the whole point of a "fine
 * hardware fault" that the bench's coarse fault types cannot express.
 *
 * Requirement ids: ROB-01, INJ-06, SEC-02, SEC-04, DET-04, DET-09, REP-01.
 */
#include "sim.h"

#include "bitflip.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TICKS   40u
#define DEFAULT_WARMUP  12u

typedef enum { TGT_NODE = 0, TGT_CTL, TGT_FRAME, TGT_COUNT } target_kind;

static const char *target_name(target_kind t) {
  switch (t) {
    case TGT_NODE:  return "node";
    case TGT_CTL:   return "ctl";
    case TGT_FRAME: return "frame";
    default:        return "?";
  }
}

static const char *zone_name(fi_zone z) {
  switch (z) {
    case FI_ZONE_ANY:      return "any";
    case FI_ZONE_MANDATE:  return "mandate";
    case FI_ZONE_TICK:     return "tick";
    case FI_ZONE_CTLSTATE: return "ctlstate";
    case FI_ZONE_VALUE:    return "value";
    default:               return "?";
  }
}

static const char *mode_name(fi_mode m) {
  switch (m) {
    case FI_MODE_BIT:  return "bit";
    case FI_MODE_BYTE: return "byte";
    case FI_MODE_WORD: return "word";
    default:           return "?";
  }
}

/* One injection plan, drawn deterministically from the master RNG. */
typedef struct {
  target_kind target;
  fi_zone zone;
  fi_mode mode;
  int node_idx;          /* 0 or 1 */
  uint32_t inject_at;    /* tick at which the flip lands */
} plan_t;

static void draw_plan(plan_t *pl, fi_rng *rng, uint32_t warmup, uint32_t ticks) {
  pl->target = (target_kind)fi_rng_below(rng, (uint32_t)TGT_COUNT);
  /* FI_ZONE_ANY on a node state machine can land on the external-law function
   * pointers; a flip there is a real crash, not a protocol event, so the node
   * target draws only from the four named data zones. */
  uint32_t zone_span = (pl->target == TGT_NODE) ? 4u : 5u;
  uint32_t zone_base = (pl->target == TGT_NODE) ? 1u : 0u;
  pl->zone = (fi_zone)(zone_base + fi_rng_below(rng, zone_span));
  pl->mode = (fi_mode)fi_rng_below(rng, 3u);
  pl->node_idx = (int)fi_rng_below(rng, 2u);
  /* Inject somewhere in the middle third of the run. */
  uint32_t lo = warmup + 1u;
  uint32_t hi = ticks > warmup + 6u ? ticks - 4u : lo;
  pl->inject_at = hi > lo ? lo + fi_rng_below(rng, hi - lo + 1u) : lo;
}

/* Apply the flip to the world only (the twin stays clean). Returns true when
 * an injection actually happened (a frame target may find no pending frame). */
static bool apply_plan(sim_t *w, const plan_t *pl, fi_rng *rng, fi_report *rep) {
  if (pl->target == TGT_NODE)
    return fi_flip_node(&w->nodes[pl->node_idx].sm, pl->zone, pl->mode,
                        FI_RANDOM, rng, rep) > 0;
  if (pl->target == TGT_CTL)
    return fi_flip_ctl(&w->nodes[pl->node_idx].sm.ctl, pl->zone, pl->mode,
                       FI_RANDOM, rng, rep) > 0;

  /* Frame target: corrupt one pending frame in flight, as if a bit flipped on
   * the wire and the receiver's parser stored the wrong field. */
  if (w->pending_count == 0) return false;
  uint32_t idx = fi_rng_below(rng, w->pending_count);
  relais_msg *m = &w->to_act[idx].msg;
  relais_key k;
  relais_key_clear(&k);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(m, &k, buf, sizeof buf);
  if (len == 0) return false;
  if (fi_flip_frame(buf, len, pl->zone, pl->mode, FI_RANDOM, rng, rep) == 0)
    return false;
  relais_msg corrupted;
  if (!relais_frame_decode_unchecked(buf, len, &corrupted)) return false;
  *m = corrupted;
  return true;
}

/* Feed the invariants exactly as the simulator main does, for one tick. */
static void feed_inv(relais_inv *inv, const relais_arb_out *out, uint32_t tick,
                     float twin_value) {
  relais_jr_tick rec;
  memset(&rec, 0, sizeof rec);
  rec.tick = tick;
  rec.mandate = out->mandate;
  rec.holder = out->holder;
  rec.source = out->source;
  rec.value = out->value;
  rec.misses = out->misses;
  rec.lamp = out->lamp;
  rec.cause = out->cause;
  rec.certified = out->certified;
  rec.certified_node = out->certified_node;
  rec.stop_requested = out->stop_requested;
  for (uint8_t e = 0; e < out->event_count; e++) {
    relais_jr_event ev;
    memset(&ev, 0, sizeof ev);
    ev.tick = tick;
    ev.kind = out->events[e].kind;
    ev.node = out->events[e].node;
    ev.mandate = out->events[e].mandate;
    ev.detail = out->events[e].detail;
    relais_inv_on_event(inv, &ev);
  }
  relais_inv_on_tick(inv, &rec, &twin_value);
  relais_inv_end_tick(inv, tick);
}

/* Classify the observable effect of the injection by comparing the world
 * against its twin: the system "detected" the fault when it reacted (a frame
 * refused, a disagreement, a decertification, a handover, the fallback loop or
 * the lamp). A value that drifted without any reaction stays "masked"; the
 * drift itself is published separately as max_deviation. */
static const char *classify(const sim_t *w, const sim_t *t,
                            const uint32_t wev[RELAIS_EV_NODE_BACK + 1],
                            const uint32_t tev[RELAIS_EV_NODE_BACK + 1],
                            bool lamp_diverged) {
  if (relais_arb_rejected_total(&w->arb) != relais_arb_rejected_total(&t->arb))
    return "reject";
  if (wev[RELAIS_EV_DISAGREEMENT] != tev[RELAIS_EV_DISAGREEMENT])
    return "disagreement";
  if (wev[RELAIS_EV_DECERTIFIED] != tev[RELAIS_EV_DECERTIFIED])
    return "decertified";
  if (wev[RELAIS_EV_MANDATE_REVOKED] != tev[RELAIS_EV_MANDATE_REVOKED] ||
      wev[RELAIS_EV_MANDATE_GRANTED] != tev[RELAIS_EV_MANDATE_GRANTED] ||
      wev[RELAIS_EV_MANDATE_AUTO_SWITCH] != tev[RELAIS_EV_MANDATE_AUTO_SWITCH] ||
      wev[RELAIS_EV_CESSION] != tev[RELAIS_EV_CESSION])
    return "handover";
  if (wev[RELAIS_EV_FALLBACK_ENTER] != tev[RELAIS_EV_FALLBACK_ENTER])
    return "fallback";
  if (lamp_diverged || wev[RELAIS_EV_LAMP_CHANGE] != tev[RELAIS_EV_LAMP_CHANGE])
    return "lamp";
  if (wev[RELAIS_EV_REQUEST_DENIED] != tev[RELAIS_EV_REQUEST_DENIED])
    return "request-denied";
  return "masked";
}

/* What one run accumulated, compared against its twin at the end. */
typedef struct {
  bool injected;
  bool lamp_diverged;
  float max_dev;
  uint32_t wev[RELAIS_EV_NODE_BACK + 1];
  uint32_t tev[RELAIS_EV_NODE_BACK + 1];
  relais_inv inv;
  fi_report rep;
} run_state_t;

/* Compare the two worlds tick by tick: the deviation of the applied value, the
 * lamp, and the events each actuator produced. */
static void observe(run_state_t *st, const relais_arb_out *w, const relais_arb_out *t) {
  float dev = fabsf(w->value - t->value);
  if (dev > st->max_dev) st->max_dev = dev;
  if (w->lamp != t->lamp) st->lamp_diverged = true;
  for (uint8_t e = 0; e < w->event_count; e++)
    if ((unsigned)w->events[e].kind <= RELAIS_EV_NODE_BACK) st->wev[w->events[e].kind]++;
  for (uint8_t e = 0; e < t->event_count; e++)
    if ((unsigned)t->events[e].kind <= RELAIS_EV_NODE_BACK) st->tev[t->events[e].kind]++;
}

/* The tick loop: both worlds run identically, the flip lands in the world only
 * at the planned tick. */
static void run_ticks(sim_t *world, sim_t *twin, const plan_t *pl, fi_rng *rng,
                      uint32_t ticks, run_state_t *st) {
  relais_inv_init(&st->inv, &world->params);
  relais_inv_expect_certified(&st->inv, world->complete);
  relais_inv_fault_free(&st->inv, false);

  for (uint32_t tick = 1; tick <= ticks; tick++) {
    if (tick == pl->inject_at && !st->injected) {
      st->injected = apply_plan(world, pl, rng, &st->rep);
      /* A raw bitflip is outside the modeled fault taxonomy: from this tick,
       * continuity (I4/I5) is not claimed, only safety (I1/I2/I3/I6/I8/I9/I10/
       * I11 and DET-03) is. */
      relais_inv_double_fault(&st->inv, tick);
    }
    relais_arb_out wout, tout;
    sim_run_tick(world, tick, &wout);
    sim_run_tick(twin, tick, &tout);
    observe(st, &wout, &tout);
    feed_inv(&st->inv, &wout, tick, tout.value);
  }
}

/* One JSON line per run, for campaign.py to aggregate. */
static void emit(uint32_t run, uint64_t seed, const plan_t *pl, bool complete,
                 const sim_t *world, const sim_t *twin, const run_state_t *st) {
  const char *reason = st->injected
                           ? classify(world, twin, st->wev, st->tev, st->lamp_diverged)
                           : "no-target";
  char first[RELAIS_INV_MSG_MAX];
  relais_inv_report(&st->inv, first, sizeof first);

  printf("{\"run\":%u,\"seed\":%llu,\"target\":\"%s\",\"zone\":\"%s\",\"mode\":\"%s\","
         "\"level\":\"%s\",\"inject_at\":%u,\"injected\":%s,\"detected\":%s,"
         "\"reason\":\"%s\",\"violations\":%u,\"rejected_delta\":%d,"
         "\"handover_delta\":%d,\"max_deviation\":%.6g,\"inv\":\"%s\"}\n",
         run, (unsigned long long)seed, target_name(pl->target), zone_name(pl->zone),
         mode_name(pl->mode), complete ? "complet" : "socle", pl->inject_at,
         st->injected ? "true" : "false",
         (st->injected && strcmp(reason, "masked") != 0) ? "true" : "false",
         reason, st->inv.violations,
         (int)relais_arb_rejected_total(&world->arb) -
             (int)relais_arb_rejected_total(&twin->arb),
         (int)world->arb.handovers - (int)twin->arb.handovers,
         (double)st->max_dev, st->inv.violations > 0 ? first : "");
}

static void run_one(uint32_t run, uint64_t seed, uint32_t ticks, uint32_t warmup,
                    bool complete) {
  fi_rng master;
  fi_rng_seed(&master, seed * 1000003ull + run);
  plan_t pl;
  draw_plan(&pl, &master, warmup, ticks);

  relais_params params;
  relais_params_default(&params);
  relais_traj traj;
  relais_traj_default(&traj);

  sim_t world, twin;
  uint64_t world_seed = seed ^ (run * 0x9e3779b97f4a7c15ull);
  sim_init(&world, &params, &traj, world_seed, complete);
  sim_init(&twin, &params, &traj, world_seed, complete);

  run_state_t st;
  memset(&st, 0, sizeof st);
  run_ticks(&world, &twin, &pl, &master, ticks, &st);
  emit(run, seed, &pl, complete, &world, &twin, &st);
}

int main(int argc, char **argv) {
  uint32_t runs = 1000;
  uint64_t seed = 1;
  uint32_t ticks = DEFAULT_TICKS;
  uint32_t warmup = DEFAULT_WARMUP;
  bool complete = true;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--runs") == 0 && i + 1 < argc) runs = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--seed") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--ticks") == 0 && i + 1 < argc) ticks = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--warmup") == 0 && i + 1 < argc) warmup = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--level") == 0 && i + 1 < argc) complete = strcmp(argv[++i], "complet") == 0;
    else { fprintf(stderr, "usage: relais-faultinject [--runs N] [--seed S] [--ticks T] [--warmup W] [--level socle|complet]\n"); return 2; }
  }
  if (warmup >= ticks) warmup = ticks / 3u;

  for (uint32_t r = 0; r < runs; r++) run_one(r, seed, ticks, warmup, complete);
  return 0;
}