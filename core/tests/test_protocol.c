/* Relais — protocol level tests (level 0/1 of spec 7.1).
 *
 * These drive the very same world as the seeded simulator (host/simulate):
 * one actuator, two computers, the links, the plant. What is asserted here is
 * the promise of the project, tick by tick:
 *   REL-01  at level S, at most p+2 periods between two fresh orders;
 *   REL-05  at level C, no tick at all without a fresh order;
 *   PERF-05 the mandate changes exactly p+1 ticks after the last order;
 *   spec 4.6 the returning machine is disarmed and stands down;
 *   ACT-03  the safe state is reached when both machines are gone.
 */
#include "sim.h"
#include "test_util.h"

#include <string.h>

static void world(sim_t *s, bool complete, uint64_t seed) {
  relais_params params;
  relais_traj traj;
  relais_params_default(&params);
  relais_traj_default(&traj);
  sim_init(s, &params, &traj, seed, complete);
}

/* Run until the mandate is armed and the second is certified (level C). */
static uint32_t arm(sim_t *s, uint32_t max_ticks) {
  relais_arb_out out;
  uint32_t tick = 0;
  while (++tick <= max_ticks) {
    sim_run_tick(s, tick, &out);
    if (out.holder != RELAIS_NODE_NONE && (!s->complete || out.certified)) return tick;
  }
  return 0;
}

/* Kill the machine that actually holds the mandate: which one wins the first
 * demande depends on the order the frames are delivered, and the test must not
 * assume it. */
static int holder_index(const sim_t *s) { return s->arb.holder == RELAIS_NODE_B ? 1 : 0; }
static int second_index(const sim_t *s) { return holder_index(s) == 0 ? 1 : 0; }

static void test_socle_handover_is_bounded(void) {
  t_section("REL-01 / spec 4.5: handover at level S, at most p+2 ticks");
  sim_t s;
  world(&s, false, 11);
  uint32_t start = arm(&s, 50);
  T_CHECK(start > 0);
  relais_arb_out out;
  uint32_t tick = start;
  /* A dies right after its order for this tick: the worst case of spec 4.5. */
  sim_add_fault(&s, tick + 1, "death", holder_index(&s), 0, 0.0f);
  relais_tick last_fresh = 0;
  uint32_t worst_gap = 0;
  uint32_t gap = 0;
  relais_mandate mandate_before = 0;
  uint32_t handovers = 0;
  for (uint32_t i = 0; i < 20; i++) {
    sim_apply_faults(&s, ++tick);
    sim_run_tick(&s, tick, &out);
    bool fresh = out.source == RELAIS_SRC_HOLDER || out.source == RELAIS_SRC_SHADOW;
    if (fresh) {
      if (gap > worst_gap) worst_gap = gap;
      gap = 0;
      last_fresh = out.statut.tick;
    } else {
      gap++;
    }
    if (mandate_before != 0 && out.mandate != mandate_before) handovers++;
    mandate_before = out.mandate;
  }
  (void)last_fresh;
  T_MSG(worst_gap <= (uint32_t)s.params.patience + 2u,
        "REL-01: more than p+2 ticks between fresh orders");
  T_EQ_U(handovers, 1);
  T_EQ_U(s.arb.mandate, 2); /* MAN-03: exactly one step per change of holder */
}

static void test_complet_zero_gap(void) {
  t_section("REL-05 / PERF-05: no tick without a fresh order, switch at p+1");
  sim_t s;
  world(&s, true, 12);
  uint32_t start = arm(&s, 50);
  T_CHECK(start > 0);
  relais_arb_out out;
  uint32_t tick = start;
  relais_node first_holder = s.arb.holder;
  sim_add_fault(&s, tick + 1, "death", holder_index(&s), 0, 0.0f);
  relais_tick last_order_tick = 0;
  relais_tick switch_tick = 0;
  uint32_t stale_ticks = 0;
  for (uint32_t i = 0; i < 30; i++) {
    sim_apply_faults(&s, ++tick);
    sim_run_tick(&s, tick, &out);
    if (switch_tick == 0 && out.source == RELAIS_SRC_HOLDER && out.holder == first_holder)
      last_order_tick = tick;
    if (switch_tick == 0 && out.holder != RELAIS_NODE_NONE && out.holder != first_holder)
      switch_tick = tick;
    if (out.source != RELAIS_SRC_HOLDER && out.source != RELAIS_SRC_SHADOW) stale_ticks++;
  }
  T_EQ_U(stale_ticks, 0);
  T_CHECK(switch_tick > 0);
  T_EQ_U(switch_tick - last_order_tick, s.params.patience + 1);
}

static void test_returning_machine_is_disarmed(void) {
  t_section("spec 4.6: a machine waking from a freeze cannot command");
  sim_t s;
  world(&s, true, 13);
  uint32_t start = arm(&s, 50);
  relais_arb_out out;
  uint32_t tick = start;
  int victim = holder_index(&s);
  sim_add_fault(&s, tick + 1, "freeze", victim, 40, 0.0f);
  for (uint32_t i = 0; i < 60; i++) {
    sim_apply_faults(&s, ++tick);
    sim_run_tick(&s, tick, &out);
  }
  T_NE(s.arb.holder, (relais_node)victim);
  T_EQ_U(s.nodes[victim].sm.role, RELAIS_ROLE_SECOND); /* it stood down by itself */
  /* And if it tried to command anyway, the actuator would refuse: a deposed
   * machine cannot impose its order (spec 4.6, I3). */
  uint32_t before = s.arb.rejected[RELAIS_REJ_NOT_HOLDER];
  sim_add_fault(&s, tick + 1, "forge", victim, 0, -0.9f);
  sim_apply_faults(&s, ++tick);
  sim_run_tick(&s, tick, &out);
  T_CHECK(s.arb.rejected[RELAIS_REJ_NOT_HOLDER] > before);
  T_MSG(out.value != -0.9f, "the forged value of the deposed machine was applied");
}

static void test_both_dead_reach_safe_state(void) {
  t_section("ACT-03 / I8: both machines gone, the safe state is reached in time");
  sim_t s;
  world(&s, true, 14);
  uint32_t start = arm(&s, 50);
  relais_arb_out out;
  uint32_t tick = start;
  sim_add_fault(&s, tick + 1, "death", holder_index(&s), 0, 0.0f);
  sim_add_fault(&s, tick + 6, "death", second_index(&s), 0, 0.0f);
  relais_tick last_fresh = 0;
  relais_tick fallback_at = 0;
  for (uint32_t i = 0; i < 40; i++) {
    sim_apply_faults(&s, ++tick);
    sim_run_tick(&s, tick, &out);
    if (out.source == RELAIS_SRC_HOLDER || out.source == RELAIS_SRC_SHADOW) last_fresh = tick;
    if (out.source == RELAIS_SRC_FALLBACK && fallback_at == 0) fallback_at = tick;
  }
  T_CHECK(fallback_at > 0);
  T_MSG(fallback_at - last_fresh <= (relais_tick)s.params.testament_len + 1,
        "the fallback loop was reached too late");
  T_EQ_U(out.lamp, RELAIS_LAMP_RED);
  T_EQ_U(out.source, RELAIS_SRC_FALLBACK);
}

static void test_no_wrongful_handover(void) {
  t_section("I7 / DET-03: 5000 ticks with no fault, the holder never changes");
  sim_t s;
  world(&s, true, 15);
  relais_arb_out out;
  arm(&s, 50);
  relais_node first = RELAIS_NODE_NONE;
  for (uint32_t tick = 1; tick <= 5000; tick++) {
    sim_run_tick(&s, tick, &out);
    if (out.holder != RELAIS_NODE_NONE && first == RELAIS_NODE_NONE) first = out.holder;
    if (first != RELAIS_NODE_NONE) T_EQ_U(out.holder, first);
  }
  T_EQ_U(s.arb.mandate, 1);
  T_EQ_U(s.arb.handovers, 1); /* the initial grant, and nothing else */
}

static void test_replicated_law_is_bit_identical(void) {
  t_section("CMD-03: same state, same measurements, same bits");
  relais_params params;
  relais_params_default(&params);
  relais_node_sm a;
  relais_node_sm b;
  relais_node_init(&a, &params, RELAIS_NODE_A);
  relais_node_init(&b, &params, RELAIS_NODE_B);
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  relais_msg statut;
  relais_node_out out_a;
  relais_node_out out_b;
  bool identical = true;
  for (uint32_t tick = 1; tick <= 500; tick++) {
    meas.pos = 0.3f + 0.001f * (float)tick;
    meas.vel = 0.02f * (float)tick;
    meas.setpoint = 0.5f;
    t_statut(&statut, tick, 1, RELAIS_NODE_A, RELAIS_SRC_HOLDER, 0, &meas);
    statut.holder = RELAIS_NODE_A;
    /* The state put on the wire is the one the holder had BEFORE computing,
     * which is exactly what makes the replica exact (see node.c). */
    relais_ctl_state before = a.ctl;
    relais_msg etat;
    t_etat(&etat, RELAIS_NODE_A, tick, &before, before.last_value);
    relais_node_on_status(&a, &statut, &out_a);
    statut.holder = RELAIS_NODE_A;
    relais_node_on_etat(&b, &etat, &out_b);
    relais_node_on_status(&b, &statut, &out_b);
    if (out_a.count == 0 || out_b.count == 0) continue;
    if (out_a.out[0].value != out_b.out[0].value) identical = false;
  }
  /* The exported state is small enough to be replicated whole (REP-01). */
  uint8_t blob[RELAIS_CTL_STATE_MAX];
  uint8_t len = relais_ctl_export(&a.ctl, blob, sizeof blob);
  T_CHECK(len > 0);
  T_CHECK(identical);
}

static void test_mandate_survives_actuator_restart(void) {
  t_section("MAN-11: the mandate number survives a restart of the actuator");
  sim_t s;
  world(&s, true, 16);
  relais_arb_out out;
  arm(&s, 50);
  for (uint32_t tick = 1; tick <= 20; tick++) sim_run_tick(&s, tick, &out);
  uint8_t snap[64];
  size_t len = relais_arb_snapshot(&s.arb, snap, sizeof snap);
  T_CHECK(len > 0);
  relais_mandate before = s.arb.mandate;
  relais_arb_init(&s.arb, &s.params);
  T_EQ_U(s.arb.mandate, 0);
  T_CHECK(relais_arb_restore(&s.arb, snap, len));
  T_EQ_U(s.arb.mandate, before);
}

static void test_degraded_mode_is_limited(void) {
  t_section("DEG-05: degraded mode ends in a stop request");
  sim_t s;
  world(&s, true, 17);
  s.params.degraded_ticks = 30;
  relais_arb_init(&s.arb, &s.params);
  relais_arb_out out;
  uint32_t tick = arm(&s, 50);
  sim_add_fault(&s, tick + 1, "death", second_index(&s), 0, 0.0f); /* gone */
  for (uint32_t i = 0; i < 60; i++) {
    sim_apply_faults(&s, ++tick);
    sim_run_tick(&s, tick, &out);
  }
  T_CHECK(s.arb.stop_requested);
  T_EQ_U(out.lamp, RELAIS_LAMP_RED);
}

int main(void) {
  test_socle_handover_is_bounded();
  test_complet_zero_gap();
  test_returning_machine_is_disarmed();
  test_both_dead_reach_safe_state();
  test_no_wrongful_handover();
  test_replicated_law_is_bit_identical();
  test_mandate_survives_actuator_restart();
  test_degraded_mode_is_limited();
  return t_summary();
}
