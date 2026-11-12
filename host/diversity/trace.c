/* Relais — bit exact decision trace of one simulator run (mission M37).
 *
 * The published journal prints floats with %.6g, which hides a difference below
 * one part in a million. To compare two builds of the core tick by tick we need
 * the bits, so this driver writes every decision of every tick as raw IEEE-754
 * patterns plus the discrete fields (mandate, holder, source, lamp, cause,
 * certification, events). Two builds that agree on this trace agree on every
 * decision they published, not on a rounded view of it.
 *
 * It is also where an alternative law is installed on one machine (CMD-09): the
 * design diversity of docs/CCF.md is measured on the very same corpus.
 *
 * Usage: relais-trace [--ticks N] [--seed S] [--params SPEC] [--trajectory SPEC]
 *                     [--fault at:kind:node[:arg[:value]]]...
 *                     [--level socle|complet] [--loss P] [--delay N]
 *                     [--law-a none|first-order|ramp] [--law-b ...] [--out FILE]
 * Exit code: 0 when no invariant is violated, 1 when one is, 2 on bad usage.
 *
 * The parameters, the trajectory and the faults are parsed by the core parsers
 * (relais_params_parse, relais_traj_parse, sim_parse_fault): no second
 * implementation of a format lives here.
 */
#include "sim.h"

#include "law_alt.h"

#include <stdlib.h>
#include <string.h>

#define TRACE_LINE 2048
#define TRACE_FAULT_SPECS 64

typedef struct {
  uint32_t ticks;
  uint64_t seed;
  bool complete;
  double loss;
  uint32_t delay;
  uint32_t restart_actuator_at;
  law_alt_kind law[2]; /* node A, node B */
  const char *out;
  char params[128];
  char trajectory[192];
} trace_opts;

/* One law state per machine per world: sharing a state would make the two
 * machines agree for the wrong reason (as in core/tests/test_law.c). */
static law_alt_state g_law[2][2]; /* [world][node] */

typedef struct {
  char *at;
  size_t left;
  bool first;
} line_buf;

static void lb_str(line_buf *b, const char *s) {
  if (b->left == 0) return;
  int n = snprintf(b->at, b->left, "%s%s", b->first ? "" : "\t", s);
  if (n < 0 || (size_t)n >= b->left) { b->left = 0; return; }
  b->at += (size_t)n;
  b->left -= (size_t)n;
  b->first = false;
}

static void lb_u64(line_buf *b, uint64_t v) {
  char tmp[24];
  snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
  lb_str(b, tmp);
}

static void lb_u32(line_buf *b, uint32_t v) { lb_u64(b, v); }

static void lb_bool(line_buf *b, bool v) { lb_str(b, v ? "1" : "0"); }

/* Raw bits, not a decimal rounding: the comparison is exact and it does not
 * depend on the printf of the libc of the variant under test. */
static void lb_bits(line_buf *b, float v) {
  uint32_t raw = 0;
  char tmp[12];
  memcpy(&raw, &v, sizeof raw);
  snprintf(tmp, sizeof tmp, "%08x", (unsigned)raw);
  lb_str(b, tmp);
}

static const char *columns(void) {
  return "tick\tmandate\tholder\tsource\tvalue\tpos\tvel\tsetpoint\tmisses"
         "\tlamp\tcause\tcert\tcert_node\tstop\torder\tshadow\tdissim\twiden"
         "\tdisagree\tstreak\tshadow_miss\tfallback\tin_fb\tevents";
}

static void write_row(FILE *f, uint32_t tick, const relais_arb_out *out,
                      const relais_arb *arb, const relais_plant *pl) {
  char buf[TRACE_LINE];
  line_buf b = {buf, sizeof buf, true};
  lb_u32(&b, tick);
  lb_u32(&b, out->mandate);
  lb_str(&b, relais_node_name(out->holder));
  lb_str(&b, relais_source_name(out->source));
  lb_bits(&b, out->value);
  lb_bits(&b, pl->pos);
  lb_bits(&b, pl->vel);
  lb_bits(&b, out->statut.meas.setpoint);
  lb_u32(&b, out->misses);
  lb_str(&b, relais_lamp_name(out->lamp));
  lb_str(&b, relais_cause_name(out->cause));
  lb_bool(&b, out->certified);
  lb_str(&b, relais_node_name(out->certified_node));
  lb_bool(&b, out->stop_requested);
  lb_bits(&b, arb->last_order);
  lb_bits(&b, arb->last_shadow);
  lb_bool(&b, arb->certified_dissimilar);
  lb_bool(&b, arb->widen_seen);
  lb_bool(&b, arb->disagreement);
  lb_u32(&b, arb->certify_streak);
  lb_u32(&b, arb->shadow_misses);
  lb_bool(&b, out->source == RELAIS_SRC_FALLBACK || out->source == RELAIS_SRC_TESTAMENT);
  lb_bool(&b, arb->in_fallback);
  char evs[64];
  size_t used = 0;
  for (uint8_t i = 0; i < out->event_count && used + 8 < sizeof evs; i++)
    used += (size_t)snprintf(evs + used, sizeof evs - used, "%s%u",
                             i > 0 ? "," : "", (unsigned)out->events[i].kind);
  lb_str(&b, used > 0 ? evs : "-");
  fprintf(f, "%s\n", buf);
}

static void usage(void) {
  fprintf(stderr,
          "usage: relais-trace [--ticks N] [--seed S] [--params SPEC] [--trajectory SPEC]\n"
          "       [--fault at:kind:node[:arg[:value]]]... [--level socle|complet]\n"
          "       [--loss P] [--delay N] [--restart-actuator TICK]\n"
          "       [--law-a K] [--law-b K] [--out FILE]\n"
          "       K = none | first-order | ramp\n");
}

static bool arg_needs_value(int i, int argc) { return i + 1 < argc; }

static int parse_args(int argc, char **argv, trace_opts *o,
                      char faults[][96], size_t *fault_count) {
  *fault_count = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--ticks") == 0 && arg_needs_value(i, argc))
      o->ticks = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--seed") == 0 && arg_needs_value(i, argc))
      o->seed = strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--params") == 0 && arg_needs_value(i, argc))
      snprintf(o->params, sizeof o->params, "%s", argv[++i]);
    else if (strcmp(a, "--trajectory") == 0 && arg_needs_value(i, argc))
      snprintf(o->trajectory, sizeof o->trajectory, "%s", argv[++i]);
    else if (strcmp(a, "--level") == 0 && arg_needs_value(i, argc))
      o->complete = strcmp(argv[++i], "complet") == 0;
    else if (strcmp(a, "--loss") == 0 && arg_needs_value(i, argc))
      o->loss = strtod(argv[++i], NULL);
    else if (strcmp(a, "--delay") == 0 && arg_needs_value(i, argc))
      o->delay = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--restart-actuator") == 0 && arg_needs_value(i, argc))
      o->restart_actuator_at = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(a, "--law-a") == 0 && arg_needs_value(i, argc))
      o->law[0] = law_alt_parse(argv[++i]);
    else if (strcmp(a, "--law-b") == 0 && arg_needs_value(i, argc))
      o->law[1] = law_alt_parse(argv[++i]);
    else if (strcmp(a, "--out") == 0 && arg_needs_value(i, argc))
      o->out = argv[++i];
    else if (strcmp(a, "--fault") == 0 && arg_needs_value(i, argc)) {
      if (*fault_count < TRACE_FAULT_SPECS)
        snprintf(faults[(*fault_count)++], 96, "%s", argv[++i]);
      else i++;
    } else { usage(); return 2; }
  }
  return 0;
}

static void install_laws(sim_t *s, const trace_opts *o, int world) {
  for (int node = 0; node < 2; node++) {
    if (o->law[node] == LAW_ALT_NONE) continue;
    g_law[world][node].kind = o->law[node];
    relais_law hooks;
    law_alt_hooks(&g_law[world][node], &hooks);
    relais_node_set_law(&s->nodes[node].sm, &hooks);
  }
}

static void feed_invariants(relais_inv *inv, uint32_t tick, const relais_arb_out *out,
                            const relais_plant *pl, float twin_value) {
  relais_jr_tick record;
  memset(&record, 0, sizeof record);
  record.tick = tick;
  record.mandate = out->mandate;
  record.holder = out->holder;
  record.source = out->source;
  record.value = out->value;
  record.pos = pl->pos;
  record.vel = pl->vel;
  record.setpoint = out->statut.meas.setpoint;
  record.misses = out->misses;
  record.lamp = out->lamp;
  record.cause = out->cause;
  record.certified = out->certified;
  record.certified_node = out->certified_node;
  record.stop_requested = out->stop_requested;
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
  relais_inv_on_tick(inv, &record, &twin_value);
  relais_inv_end_tick(inv, tick);
}

/* Parse the corpus arguments and validate them with the core parsers. */
static int setup(int argc, char **argv, trace_opts *o, relais_params *params,
                 relais_traj *traj, char faults[][96], size_t *fault_count) {
  char why[128] = "";
  memset(o, 0, sizeof *o);
  o->ticks = 1000;
  o->seed = 1;
  if (parse_args(argc, argv, o, faults, fault_count) != 0) return 2;
  relais_params_default(params);
  relais_traj_default(traj);
  if (o->params[0] != '\0' && !relais_params_parse(params, o->params, why, sizeof why)) {
    fprintf(stderr, "invalid parameters: %s\n", why);
    return 2;
  }
  if (o->trajectory[0] != '\0' && !relais_traj_parse(traj, o->trajectory, why, sizeof why)) {
    fprintf(stderr, "invalid trajectory: %s\n", why);
    return 2;
  }
  return 0;
}

/* MAN-11: an actuator restart must not lose the mandate number, so the snapshot
 * is restored into a freshly initialised arbiter, exactly as the simulator does. */
static void restart_actuator(sim_t *world, const relais_params *params) {
  uint8_t snap[64];
  size_t len = relais_arb_snapshot(&world->arb, snap, sizeof snap);
  relais_arb_init(&world->arb, params);
  if (len > 0) relais_arb_restore(&world->arb, snap, len);
}

static void run_ticks(FILE *f, sim_t *world, sim_t *twin, relais_inv *inv,
                      const trace_opts *o) {
  for (uint32_t tick = 1; tick <= o->ticks; tick++) {
    if (sim_apply_faults(world, tick) > 1) relais_inv_double_fault(inv, tick);
    if (o->restart_actuator_at != 0 && tick == o->restart_actuator_at)
      restart_actuator(world, &world->params);
    relais_arb_out out;
    relais_arb_out twin_out;
    sim_run_tick(world, tick, &out);
    sim_run_tick(twin, tick, &twin_out);
    feed_invariants(inv, tick, &out, &world->plant, twin_out.value);
    write_row(f, tick, &out, &world->arb, &world->plant);
  }
}

static void init_inv(relais_inv *inv, const relais_params *params, const trace_opts *o,
                     size_t fault_count) {
  relais_inv_init(inv, params);
  relais_inv_expect_certified(inv, o->complete);
  relais_inv_fault_free(inv, fault_count == 0 && o->delay == 0 &&
                                  o->restart_actuator_at == 0);
  relais_inv_lossy_channel(inv, o->loss > 0.0 || o->delay > 0);
  /* DEG-08: with an alternative law on one machine the continuity bound at the
   * handover is the published dissimilar_delta, not delta. */
  if (o->law[0] != LAW_ALT_NONE || o->law[1] != LAW_ALT_NONE)
    relais_inv_dissimilar(inv, true);
}

int main(int argc, char **argv) {
  trace_opts o;
  relais_params params;
  relais_traj traj;
  char faults[TRACE_FAULT_SPECS][96];
  size_t fault_count = 0;
  if (setup(argc, argv, &o, &params, &traj, faults, &fault_count) != 0) return 2;

  sim_t world;
  sim_t twin;
  sim_init(&world, &params, &traj, o.seed, o.complete);
  sim_init(&twin, &params, &traj, o.seed, o.complete);
  world.loss = o.loss;
  world.max_delay = o.delay;
  install_laws(&world, &o, 0);
  install_laws(&twin, &o, 1);
  for (size_t i = 0; i < fault_count; i++)
    if (!sim_parse_fault(&world, faults[i]))
      fprintf(stderr, "fault ignored (syntax): %s\n", faults[i]);

  FILE *f = o.out != NULL ? fopen(o.out, "w") : stdout;
  if (f == NULL) { fprintf(stderr, "cannot open %s\n", o.out); return 2; }
  fprintf(f, "#columns\t%s\n", columns());
  fprintf(f, "#meta\tlaw_a=%s\tlaw_b=%s\tmutant=%s\tlevel=%s\tticks=%u\tseed=%llu\n",
          law_alt_name(o.law[0]), law_alt_name(o.law[1]), relais_mutant_name(),
          o.complete ? "complet" : "socle", (unsigned)o.ticks,
          (unsigned long long)o.seed);

  relais_inv inv;
  init_inv(&inv, &params, &o, fault_count);

  run_ticks(f, &world, &twin, &inv, &o);
  if (o.out != NULL) fclose(f);

  char report[RELAIS_INV_MSG_MAX * 2];
  relais_inv_report(&inv, report, sizeof report);
  fprintf(stderr, "trace: %u ticks, violations %u, %s\n", (unsigned)o.ticks,
          (unsigned)inv.violations, report);
  return relais_inv_ok(&inv) ? 0 : 1;
}
