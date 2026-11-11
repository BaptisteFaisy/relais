/* Relais — the rolling update (REL-10, spec 12 item 6).
 *
 * Two machines, one actionneur, and two software updates in a row. The claim
 * is not "the system survives an update" but the stronger one: while the two
 * machines are updated in turn, NOT ONE tick is served by anything else than a
 * fresh order -- the order of the holder, or the shadow of the certified
 * second. The command never falls back to the emergency law, the witness never
 * turns red, and the mandate number only ever grows, one step per change of
 * holder (MAN-03).
 *
 * The sequence (the same one as scenarios/rolling-update.json, with the
 * instants chosen here from the live state of the world so the test does not
 * depend on which machine won the first grant):
 *
 *   1. B, the second, is updated: it goes out of service for the installation,
 *      reboots through relais_node_init, and comes back with an empty state.
 *      The actuator must certify it again before it can be trusted (DEG-03,
 *      DEG-07).
 *   2. A hands the mandate over to B (REL-09): B becomes the holder.
 *   3. A, now the second, is updated: same story.
 *   4. B hands the mandate back to A: A holds again, B is the certified
 *      second -- the nominal redundant state, with a mandate number that only
 *      ever grew.
 *
 * What is checked here:
 *   (a) from the arming of the mandate to the end, every tick is served by a
 *       fresh order (holder) or by the shadow of the certified second;
 *   (b) the mandate grows by exactly one step per change of holder (MAN-03) and
 *       ends up back with A;
 *   (c) every machine that was updated is certified again BEFORE it takes the
 *       mandate back (DEG-07);
 *   (d) the invariant checker of spec 7.2 sees no violation;
 *   (e) the witness never turns red and the emergency law never takes over.
 */
#include "sim.h"
#include "test_util.h"

#include <string.h>

#define OTA_MAX_TICKS 600u
#define OTA_INSTALL 20u   /* ticks out of service: the installation window */
#define OTA_MARGIN 10u    /* ticks between a milestone and the next action */

typedef struct {
  uint32_t tick;
  relais_node holder;
  relais_mandate mandate;
  relais_source source;
  relais_lamp lamp;
  bool certified;
  relais_node certified_node;
} ota_step_t;

typedef struct {
  ota_step_t step[OTA_MAX_TICKS];
  uint32_t count;
  uint32_t first_fresh; /* tick of the first fresh order, 0 = none yet */
  uint32_t stale;       /* ticks served by something else than a fresh order */
  uint32_t red;         /* ticks with a red witness, once the mandate is armed */
} ota_log_t;

typedef struct {
  uint32_t arm;          /* the mandate is armed and the second certified */
  uint32_t update_b;     /* B goes out for its update */
  uint32_t b_back;       /* B is certified again after its update */
  uint32_t handover_ab;  /* A cedes the mandate to B */
  uint32_t b_holds;      /* B is the holder */
  uint32_t a_certified;  /* A is certified again, as the second */
  uint32_t update_a;     /* A goes out for its update */
  uint32_t a_back;       /* A is certified again after its update */
  uint32_t handover_ba;  /* B cedes the mandate back to A */
  uint32_t a_holds;      /* A is the holder again */
  uint32_t end;          /* last tick of the run */
} ota_marks_t;

static ota_step_t *last_step(ota_log_t *log) { return &log->step[log->count - 1u]; }

/* One tick of the world, recorded and fed to the invariant checker (spec 7.2). */
static void ota_tick(sim_t *s, uint32_t tick, ota_log_t *log, relais_inv *inv) {
  sim_apply_faults(s, tick);
  relais_arb_out out;
  sim_run_tick(s, tick, &out);
  if (log->count >= OTA_MAX_TICKS) return;
  ota_step_t *st = &log->step[log->count++];
  st->tick = tick;
  st->holder = out.holder;
  st->mandate = out.mandate;
  st->source = out.source;
  st->lamp = out.lamp;
  st->certified = out.certified;
  st->certified_node = out.certified_node;
  bool fresh = out.source == RELAIS_SRC_HOLDER || out.source == RELAIS_SRC_SHADOW;
  if (fresh && log->first_fresh == 0) log->first_fresh = tick;
  /* Both claims are measured from the arming of the mandate: before it, the
   * actuator is alone and the emergency law is exactly what must run. */
  if (log->first_fresh != 0 && !fresh) log->stale++;
  if (log->first_fresh != 0 && out.lamp == RELAIS_LAMP_RED) log->red++;
  for (uint8_t e = 0; e < out.event_count; e++) {
    relais_jr_event ev;
    memset(&ev, 0, sizeof ev);
    ev.tick = tick;
    ev.kind = out.events[e].kind;
    ev.node = out.events[e].node;
    ev.mandate = out.events[e].mandate;
    ev.detail = out.events[e].detail;
    relais_inv_on_event(inv, &ev);
  }
  relais_jr_tick rec;
  memset(&rec, 0, sizeof rec);
  rec.tick = tick;
  rec.mandate = out.mandate;
  rec.holder = out.holder;
  rec.source = out.source;
  rec.value = out.value;
  rec.misses = out.misses;
  rec.lamp = out.lamp;
  rec.cause = out.cause;
  rec.certified = out.certified;
  rec.certified_node = out.certified_node;
  relais_inv_on_tick(inv, &rec, NULL);
  relais_inv_end_tick(inv, tick);
}

/* Run until the predicate is true; returns the tick, or 0 when the limit is
 * reached. The predicate reads the last recorded step. */
static uint32_t ota_until(sim_t *s, ota_log_t *log, relais_inv *inv, uint32_t *tick,
                          uint32_t limit, bool (*want)(const ota_step_t *)) {
  while (*tick < limit) {
    (*tick)++;
    ota_tick(s, *tick, log, inv);
    if (want(last_step(log))) return *tick;
  }
  return 0;
}

static bool want_armed(const ota_step_t *st) {
  return st->holder != RELAIS_NODE_NONE && st->certified;
}
static bool want_certified_b(const ota_step_t *st) {
  return st->certified && st->certified_node == RELAIS_NODE_B;
}
static bool want_not_certified_b(const ota_step_t *st) {
  return st->certified_node != RELAIS_NODE_B;
}
static bool want_certified_a(const ota_step_t *st) {
  return st->certified && st->certified_node == RELAIS_NODE_A;
}
static bool want_not_certified_a(const ota_step_t *st) {
  return st->certified_node != RELAIS_NODE_A;
}
static bool want_holder_b(const ota_step_t *st) { return st->holder == RELAIS_NODE_B; }
static bool want_holder_a(const ota_step_t *st) { return st->holder == RELAIS_NODE_A; }
static bool want_never(const ota_step_t *st) {
  (void)st;
  return false;
}

/* The whole rolling update, with the instants taken from the live world. */
static void ota_run(sim_t *s, ota_log_t *log, ota_marks_t *marks, relais_inv *inv) {
  uint32_t tick = 0;
  marks->arm = ota_until(s, log, inv, &tick, 100u, want_armed);
  /* 1. B, the second, is updated. */
  marks->update_b = tick + OTA_MARGIN;
  sim_add_fault(s, marks->update_b, "update", 1, OTA_INSTALL, 0.0f);
  ota_until(s, log, inv, &tick, marks->update_b + 4u * OTA_INSTALL, want_not_certified_b);
  marks->b_back = ota_until(s, log, inv, &tick, marks->update_b + 4u * OTA_INSTALL, want_certified_b);
  /* 2. A cedes the mandate to B (REL-09). */
  marks->handover_ab = tick + OTA_MARGIN;
  sim_add_fault(s, marks->handover_ab, "handover", 0, 0, 0.0f);
  marks->b_holds = ota_until(s, log, inv, &tick, marks->handover_ab + 20u, want_holder_b);
  /* The new holder has no certified second any more: A must be certified again. */
  marks->a_certified = ota_until(s, log, inv, &tick, marks->b_holds + 60u, want_certified_a);
  /* 3. A, now the second, is updated. */
  marks->update_a = tick + OTA_MARGIN;
  sim_add_fault(s, marks->update_a, "update", 0, OTA_INSTALL, 0.0f);
  ota_until(s, log, inv, &tick, marks->update_a + 4u * OTA_INSTALL, want_not_certified_a);
  marks->a_back = ota_until(s, log, inv, &tick, marks->update_a + 4u * OTA_INSTALL, want_certified_a);
  /* 4. B cedes the mandate back to A. */
  marks->handover_ba = tick + OTA_MARGIN;
  sim_add_fault(s, marks->handover_ba, "handover", 1, 0, 0.0f);
  marks->a_holds = ota_until(s, log, inv, &tick, marks->handover_ba + 20u, want_holder_a);
  /* The nominal redundant state: A holds, B is the certified second. */
  ota_until(s, log, inv, &tick, marks->a_holds + 20u, want_never);
  marks->end = tick;
}

/* MAN-03: the mandate grows by exactly one step per change of holder, the
 * initial grant included (0 -> 1), and the two handovers of the rolling
 * update (A -> B, then B -> A). */
static void check_mandate_steps(const ota_log_t *log) {
  uint32_t changes = 0;
  uint32_t bad = 0;
  for (uint32_t i = 1; i < log->count; i++) {
    const ota_step_t *prev = &log->step[i - 1u];
    const ota_step_t *cur = &log->step[i];
    if (cur->mandate < prev->mandate) bad++;
    if (cur->holder != prev->holder && cur->holder != RELAIS_NODE_NONE) {
      changes++;
      if (cur->mandate != prev->mandate + 1u) bad++;
    }
  }
  T_EQ_U(changes, 3u); /* the first grant, A -> B, B -> A */
  T_EQ_U(bad, 0u);
}

/* The mission's sequence, in order: update B, cede A->B, update A, cede B->A,
 * each step waiting for the previous one to be certified (DEG-07). */
static void check_marks_order(const ota_marks_t *m) {
  T_CHECK(m->arm > 0u);
  T_CHECK(m->update_b > m->arm);
  T_CHECK(m->b_back > m->update_b);       /* B came back and was certified */
  T_CHECK(m->handover_ab > m->b_back);
  T_CHECK(m->b_holds > m->handover_ab);   /* B took the mandate */
  T_CHECK(m->a_certified > m->b_holds);   /* A certified again as the second */
  T_CHECK(m->update_a > m->a_certified);
  T_CHECK(m->a_back > m->update_a);       /* A came back and was certified */
  T_CHECK(m->handover_ba > m->a_back);
  T_CHECK(m->a_holds > m->handover_ba);   /* A took the mandate back */
  T_CHECK(m->end > m->a_holds);
}

static void test_rolling_update_has_no_stale_tick(void) {
  t_section("REL-10: two updates in a row, no tick without a fresh order");
  sim_t s;
  relais_params params;
  relais_traj traj;
  relais_params_default(&params);
  relais_traj_default(&traj);
  sim_init(&s, &params, &traj, 4242u, true); /* level C: the shadow is in play */
  ota_log_t log;
  memset(&log, 0, sizeof log);
  ota_marks_t marks;
  memset(&marks, 0, sizeof marks);
  relais_inv inv;
  relais_inv_init(&inv, &params);
  relais_inv_expect_certified(&inv, true);  /* I4: not one tick without a fresh order */
  relais_inv_fault_free(&inv, false);       /* faults are injected on purpose */
  ota_run(&s, &log, &marks, &inv);

  /* The scenario really happened, in the order the mission describes. */
  check_marks_order(&marks);

  /* (a) and (e): every tick is fresh, the witness never turns red. */
  T_EQ_U(log.stale, 0u);
  T_EQ_U(log.red, 0u);
  /* (b): the mandate grew by one step per change of holder, and came back to A. */
  check_mandate_steps(&log);
  T_EQ_U(last_step(&log)->holder, RELAIS_NODE_A);
  T_EQ_U(last_step(&log)->mandate, 3u);
  /* (c): DEG-07, a machine that was updated is certified before it holds again. */
  T_CHECK(marks.b_back < marks.b_holds);
  T_CHECK(marks.a_back < marks.a_holds);
  /* (d): the invariant checker of spec 7.2 saw nothing. */
  char report[256];
  relais_inv_report(&inv, report, sizeof report);
  T_MSG(relais_inv_ok(&inv), "an invariant was violated during the rolling update");
  T_CHECK(inv.violations == 0u);
  printf("   %s\n", report);
  printf("   armed=%u update(B)=%u B certified again=%u cession A->B=%u B holds=%u\n",
         marks.arm, marks.update_b, marks.b_back, marks.handover_ab, marks.b_holds);
  printf("   A certified=%u update(A)=%u A certified again=%u cession B->A=%u A holds=%u "
         "end=%u mandate=%u\n",
         marks.a_certified, marks.update_a, marks.a_back, marks.handover_ba, marks.a_holds,
         marks.end, last_step(&log)->mandate);
}

/* The reason the cession comes first: an update of the HOLDER costs one tick,
 * while a brutal stop of the holder costs p+1 ticks of silence proof. */
static void test_update_of_the_holder_is_instant(void) {
  t_section("REL-06 / REL-10: the cession first makes the handover cost one tick");
  sim_t cession;
  sim_t silence;
  relais_params params;
  relais_traj traj;
  relais_params_default(&params);
  relais_traj_default(&traj);
  sim_init(&cession, &params, &traj, 77u, true);
  sim_init(&silence, &params, &traj, 77u, true);
  ota_log_t log_c;
  ota_log_t log_s;
  memset(&log_c, 0, sizeof log_c);
  memset(&log_s, 0, sizeof log_s);
  /* Two worlds, two checkers: the invariant state is never shared. */
  relais_inv inv_c;
  relais_inv inv_s;
  relais_inv_init(&inv_c, &params);
  relais_inv_init(&inv_s, &params);
  relais_inv_expect_certified(&inv_c, true);
  relais_inv_expect_certified(&inv_s, true);
  uint32_t tick_c = 0;
  uint32_t tick_s = 0;
  uint32_t arm_c = ota_until(&cession, &log_c, &inv_c, &tick_c, 100u, want_armed);
  uint32_t arm_s = ota_until(&silence, &log_s, &inv_s, &tick_s, 100u, want_armed);
  T_CHECK(arm_c > 0u && arm_s > 0u);
  /* The same instant, two ways of losing the holder. */
  uint32_t at = arm_c + OTA_MARGIN;
  sim_add_fault(&cession, at, "update", 0, OTA_INSTALL, 0.0f); /* cedes, then stops */
  sim_add_fault(&silence, at, "death", 0, 0, 0.0f);             /* stops, says nothing */
  uint32_t switch_c = ota_until(&cession, &log_c, &inv_c, &tick_c, at + 40u, want_holder_b);
  uint32_t switch_s = ota_until(&silence, &log_s, &inv_s, &tick_s, at + 40u, want_holder_b);
  T_CHECK(switch_c > 0u && switch_s > 0u);
  T_EQ_U(switch_c - at, 1u);                    /* the cession: one tick */
  T_EQ_U(switch_s - at, (uint32_t)params.patience + 1u); /* the silence proof: p+1 */
  T_CHECK(switch_c < switch_s);                 /* the planned absence is cheaper */
  T_EQ_U(log_c.stale, 0u);
  T_EQ_U(log_s.stale, 0u);
}

int main(void) {
  test_rolling_update_has_no_stale_tick();
  test_update_of_the_holder_is_instant();
  return t_summary();
}
