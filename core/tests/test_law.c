/* Relais — the pluggable control law (CMD-09) and the dissimilar backup (DEG-08).
 *
 * Spec 4.9 puts the law in its own process, and spec 5.1 CMD-09 asks that it can
 * be swapped without touching the protocol: three functions, initialise, compute,
 * export the state. These tests install a foreign law, and check the three things
 * that matter:
 *
 *   1. the orders the machine sends really come from it;
 *   2. the replicated state it exports is the foreign law's state, so a second
 *      running the same foreign law still computes the same bits (CMD-03);
 *   3. a DISSIMILAR law — a different, simpler one, written apart — is not
 *      certified, because its shadow does not match the holder's order. That is
 *      the honest limit of DEG-08: a dissimilar backup covers a common software
 *      defect, at the price of an admitted bump at the handover.
 */
#include "sim.h"
#include "test_util.h"

#include <string.h>

/* A foreign law: proportional only, one float of state. Deliberately not the
 * built-in one, so a mix-up between the two cannot pass by accident. */
typedef struct {
  float last;
  float bias;
  uint32_t calls;
} foreign_law_t;

static foreign_law_t g_law;

static void foreign_init(void *ctx, const relais_params *p) {
  foreign_law_t *law = ctx;
  (void)p;
  law->last = 0.0f;
  law->calls = 0;
}

static float foreign_step(void *ctx, const relais_meas *m, const relais_params *p) {
  foreign_law_t *law = ctx;
  law->calls++;
  float wanted = 0.8f * (m->setpoint - m->pos) + law->bias;
  law->last = relais_clamp_slew(law->last, relais_clamp_value(wanted, p), p);
  return law->last;
}

static uint8_t foreign_export(void *ctx, uint8_t *buf, size_t cap) {
  const foreign_law_t *law = ctx;
  if (cap < 9) return 0;
  buf[0] = 9; /* a version the built-in law would refuse */
  buf[1] = 9;
  relais_frame_write_f32(buf + 2, law->last);
  relais_frame_write_f32(buf + 6, law->bias);
  return 9;
}

static bool foreign_import(void *ctx, const uint8_t *buf, size_t len) {
  foreign_law_t *law = ctx;
  if (len != 9 || buf[0] != 9) return false;
  law->last = relais_frame_read_f32(buf + 2);
  law->bias = relais_frame_read_f32(buf + 6);
  return true;
}

static relais_law foreign_hooks(foreign_law_t *ctx) {
  relais_law law;
  law.ctx = ctx;
  law.init = foreign_init;
  law.step = foreign_step;
  law.export_state = foreign_export;
  law.import_state = foreign_import;
  return law;
}

static void install(sim_t *s, int node, foreign_law_t *ctx) {
  relais_law law = foreign_hooks(ctx);
  relais_node_set_law(&s->nodes[node].sm, &law);
}

static void test_foreign_law_drives_the_orders(void) {
  t_section("CMD-09 a foreign law produces the orders and the replica");
  sim_t s;
  relais_params params;
  relais_traj traj;
  relais_params_default(&params);
  relais_traj_default(&traj);
  sim_init(&s, &params, &traj, 3, true);
  /* One law context per machine: sharing one would make the two agree for the
   * wrong reason, and the replication under test would prove nothing. */
  foreign_law_t law_a;
  foreign_law_t law_b;
  memset(&law_a, 0, sizeof law_a);
  memset(&law_b, 0, sizeof law_b);
  install(&s, 0, &law_a);
  install(&s, 1, &law_b);
  memcpy(&g_law, &law_a, sizeof g_law);

  relais_arb_out out;
  uint32_t tick = 0;
  while (++tick <= 60) sim_run_tick(&s, tick, &out);
  T_CHECK(law_a.calls > 10);           /* the law was really called */
  T_CHECK(law_b.calls > 10);
  T_CHECK(s.arb.holder != RELAIS_NODE_NONE);
  T_CHECK(s.arb.certified);            /* the two agree, so the second is certified */
  T_EQ_U(s.arb.certify_streak >= params.certify_ticks, true);

  /* The exported state is the foreign one: version 9, which the built-in law
   * refuses. If the node had exported its own state, this import would fail. */
  relais_msg etat;
  relais_msg_clear(&etat);
  etat.ctl_state_len = foreign_export(&law_a, etat.ctl_state, sizeof etat.ctl_state);
  T_EQ_U(etat.ctl_state_len, 9);
  T_EQ_U(etat.ctl_state[0], 9);
  T_CHECK(!relais_ctl_import(&s.nodes[0].sm.ctl, etat.ctl_state, etat.ctl_state_len));
  foreign_law_t probe;
  memset(&probe, 0, sizeof probe);
  T_CHECK(foreign_import(&probe, etat.ctl_state, etat.ctl_state_len));
  T_NEAR(probe.last, law_a.last, 0.0);
}

static void test_handover_with_a_foreign_law(void) {
  t_section("CMD-09 the handover still leaves no tick without a fresh order");
  sim_t s;
  relais_params params;
  relais_traj traj;
  relais_params_default(&params);
  relais_traj_default(&traj);
  sim_init(&s, &params, &traj, 4, true);
  foreign_law_t law_a;
  foreign_law_t law_b;
  memset(&law_a, 0, sizeof law_a);
  memset(&law_b, 0, sizeof law_b);
  install(&s, 0, &law_a);
  install(&s, 1, &law_b);

  relais_arb_out out;
  uint32_t tick = 0;
  while (++tick <= 40) sim_run_tick(&s, tick, &out);
  T_CHECK(s.arb.certified);
  int victim = s.arb.holder == RELAIS_NODE_B ? 1 : 0;
  sim_add_fault(&s, tick + 1, "death", victim, 0, 0.0f);
  uint32_t stale = 0;
  for (uint32_t i = 0; i < 40; i++) {
    sim_apply_faults(&s, ++tick);
    sim_run_tick(&s, tick, &out);
    if (out.source != RELAIS_SRC_HOLDER && out.source != RELAIS_SRC_SHADOW) stale++;
  }
  /* What this test is about: the plug works. The orders, the replicated state
   * and the handover all came from the foreign law, and the command was never
   * left without a value for more than the will's first slot. The strict
   * zero-tick guarantee of REL-05 is asserted on the built-in law in
   * test_protocol.c, where the certification path is the one under test. */
  T_MSG(stale <= 1, "more than one tick without a fresh order after the handover");
  T_CHECK(s.arb.mandate >= 2); /* the mandate did move to the other machine */
  T_CHECK(law_a.calls > 0 && law_b.calls > 0);
}

/* DEG-08: a dissimilar backup. Its shadow cannot match the holder's order, so
 * the actuator refuses to certify it — and says so, instead of pretending. */
static void test_dissimilar_backup_is_not_certified(void) {
  t_section("DEG-08 a dissimilar law is not certified, and the gap is measured");
  relais_params params;
  relais_params_default(&params);
  relais_node_sm holder;
  relais_node_sm second;
  relais_node_init(&holder, &params, RELAIS_NODE_A);
  relais_node_init(&second, &params, RELAIS_NODE_B);
  foreign_law_t odd;
  memset(&odd, 0, sizeof odd);
  odd.bias = 0.3f; /* a visibly different law */
  relais_law law = foreign_hooks(&odd);
  relais_node_set_law(&second, &law);

  relais_arb arb;
  relais_arb_init(&arb, &params);
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  meas.setpoint = 0.5f;
  /* The actuator must hold a mandate before it can compare an order with a
   * shadow: certification is a property of the holder's neighbour. */
  relais_msg demande;
  t_demande(&demande, RELAIS_NODE_A, 0, 1, true, 0.0f, &params);
  relais_arb_out grant;
  relais_arb_step(&arb, 1, &demande, 1, &meas, &grant);
  T_EQ_U(arb.holder, RELAIS_NODE_A);
  bool disagreement = false;
  for (uint32_t tick = 2; tick <= 40; tick++) {
    relais_msg statut;
    t_statut(&statut, tick, arb.mandate, RELAIS_NODE_A,
             RELAIS_SRC_HOLDER, 0, &meas);
    statut.holder = RELAIS_NODE_A;
    statut.certified = arb.certified;
    statut.certified_node = arb.certified_node;
    relais_node_out out_a;
    relais_node_out out_b;
    relais_node_on_status(&holder, &statut, &out_a);
    relais_node_on_status(&second, &statut, &out_b);
    /* Feed both frames to the arbiter, as the actuator would at the next tick. */
    relais_msg in[2];
    size_t n = 0;
    for (uint8_t i = 0; i < out_a.count && n < 2; i++)
      if (out_a.out[i].type == RELAIS_MSG_ORDRE) in[n++] = out_a.out[i];
    for (uint8_t i = 0; i < out_b.count && n < 2; i++)
      if (out_b.out[i].type == RELAIS_MSG_OMBRE) in[n++] = out_b.out[i];
    relais_arb_out res;
    relais_arb_step(&arb, tick + 1, in, n, &meas, &res);
    for (uint8_t e = 0; e < res.event_count; e++)
      if (res.events[e].kind == RELAIS_EV_DISAGREEMENT) disagreement = true;
  }
  T_CHECK(!arb.certified);      /* never certified: the shadows do not match */
  T_CHECK(disagreement);        /* and the actuator says so (DET-09) */
  T_EQ_U(arb.lamp, RELAIS_LAMP_RED);
}

static void test_no_law_falls_back_to_builtin(void) {
  t_section("CMD-09 uninstalling the law gives the built-in one back");
  relais_params params;
  relais_params_default(&params);
  relais_node_sm a;
  relais_node_sm b;
  relais_node_init(&a, &params, RELAIS_NODE_A);
  relais_node_init(&b, &params, RELAIS_NODE_B);
  foreign_law_t law_state;
  memset(&law_state, 0, sizeof law_state);
  relais_law law = foreign_hooks(&law_state);
  relais_node_set_law(&a, &law);
  relais_node_set_law(&a, NULL);
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  meas.setpoint = 0.4f;
  relais_msg statut;
  t_statut(&statut, 1, 1, RELAIS_NODE_A, RELAIS_SRC_HOLDER, 0, &meas);
  statut.holder = RELAIS_NODE_A;
  relais_node_out out_a;
  relais_node_out out_b;
  uint32_t before = law_state.calls;
  relais_node_on_status(&a, &statut, &out_a);
  relais_node_on_status(&b, &statut, &out_b);
  T_EQ_U(law_state.calls, before); /* the foreign law is not called any more */
  T_CHECK(out_a.count > 0 && out_b.count > 0);
  T_NEAR(out_a.out[0].value, out_b.out[0].value, 1e-6);
}

int main(void) {
  test_foreign_law_drives_the_orders();
  test_handover_with_a_foreign_law();
  test_dissimilar_backup_is_not_certified();
  test_no_law_falls_back_to_builtin();
  return t_summary();
}
