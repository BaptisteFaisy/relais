/* Relais — unit tests of the comparison harness and of the two baselines.
 *
 * What is asserted here is the behaviour the comparison measures, so that the
 * numbers published in docs/COMPARISON-MEASURED.md cannot drift silently:
 * the handover delay of each implementation, the ticks without a fresh
 * command, and the double command of the heartbeat family when the dead
 * machine comes back.
 */
#include "harness.h"

#include "test_util.h"

#include <string.h>

static bl_config base(const bl_impl *impl, uint32_t timeout) {
  bl_config cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.impl = impl;
  cfg.ticks = 1200u;
  cfg.seed = 7u;
  cfg.timeout = timeout;
  cfg.window = 200u;
  return cfg;
}

static void test_fault_parse(void) {
  t_section("the fault syntax of the harness");
  bl_config cfg = base(bl_impl_relais(), 20);
  T_CHECK(bl_fault_parse(&cfg, "800:death:0"));
  T_EQ_U(cfg.fault_count, 1u);
  T_EQ_U(cfg.faults[0].at, 800u);
  T_EQ_U(cfg.faults[0].node, 0u);
  T_EQ_U(cfg.faults[0].kind, BL_F_DEATH);
  T_CHECK(bl_fault_parse(&cfg, "900:freeze:1:40"));
  T_EQ_U(cfg.faults[1].at, 900u);
  T_EQ_U(cfg.faults[1].node, 1u);
  T_EQ_U(cfg.faults[1].arg, 40u);
  T_CHECK(!bl_fault_parse(&cfg, "900:unknown:0"));
  T_CHECK(!bl_fault_parse(&cfg, "900:death:7"));
}

static void test_relais_handover_is_two_ticks(void) {
  t_section("Relais: the arbiter withdraws the mandate on its own evidence");
  bl_config cfg = base(bl_impl_relais(), 20);
  T_CHECK(bl_fault_parse(&cfg, "200:death:0"));
  bl_metrics m;
  bl_run(&cfg, &m);
  T_CHECK(m.handover_done);
  T_EQ_U(m.handover_ticks, 2u); /* p + 1 ticks, p = 1 */
  T_CHECK(m.fresh_done);
  T_EQ_U(m.fresh_ticks, 1u); /* the shadow of the certified second */
  T_EQ_U(m.stale_ticks, 0u); /* not one tick without a fresh command */
  T_EQ_U(m.double_cmds, 0u);
}

static void test_heartbeat_waits_for_its_timeout(void) {
  t_section("heartbeat: the takeover waits for the silence timeout");
  bl_config cfg = base(bl_impl_heartbeat(), 20);
  T_CHECK(bl_fault_parse(&cfg, "200:death:0"));
  bl_metrics m;
  bl_run(&cfg, &m);
  T_CHECK(m.handover_done);
  T_EQ_U(m.handover_ticks, 20u);
  T_EQ_U(m.stale_ticks, 19u); /* the timeout of ticks without a fresh order */
}

static void test_lease_waits_for_its_lease(void) {
  t_section("lease: the takeover waits for the expiry of the lease");
  bl_config cfg = base(bl_impl_lease(), 20);
  T_CHECK(bl_fault_parse(&cfg, "200:death:0"));
  bl_metrics m;
  bl_run(&cfg, &m);
  T_CHECK(m.handover_done);
  T_EQ_U(m.handover_ticks, 21u);
  T_EQ_U(m.stale_ticks, 21u);
  T_EQ_U(m.double_cmds, 0u); /* the term makes the old leader harmless */
}

static void test_the_heartbeat_splits_the_brain_on_return(void) {
  t_section("heartbeat: the machine that comes back commands again");
  bl_config hb = base(bl_impl_heartbeat(), 20);
  T_CHECK(bl_fault_parse(&hb, "200:freeze:0:40"));
  bl_metrics mh;
  bl_run(&hb, &mh);
  T_CHECK(mh.double_cmds > 0u); /* two commanders at the same tick */
  bl_config ls = base(bl_impl_lease(), 20);
  T_CHECK(bl_fault_parse(&ls, "200:freeze:0:40"));
  bl_metrics ml;
  bl_run(&ls, &ml);
  T_EQ_U(ml.double_cmds, 0u); /* the term rejects the returning leader */
  bl_config rl = base(bl_impl_relais(), 20);
  T_CHECK(bl_fault_parse(&rl, "200:freeze:0:40"));
  bl_metrics mr;
  bl_run(&rl, &mr);
  T_EQ_U(mr.double_cmds, 0u); /* the returning machine is disarmed */
}

static void test_the_relais_handover_survives_a_cut_peer_link(void) {
  t_section("a cut link between the computers does not stop Relais");
  bl_config cfg = base(bl_impl_relais(), 20);
  T_CHECK(bl_fault_parse(&cfg, "200:cut-peer:0"));
  T_CHECK(bl_fault_parse(&cfg, "400:death:0"));
  cfg.measure_at = 400u;
  bl_metrics m;
  bl_run(&cfg, &m);
  T_CHECK(m.handover_done);
  T_EQ_U(m.handover_ticks, 2u);
  T_EQ_U(m.stale_ticks, 0u);
}

static void test_the_baselines_do_not_hand_over_on_a_cut_link(void) {
  t_section("without the death, no implementation changes of commander");
  bl_config hb = base(bl_impl_heartbeat(), 20);
  T_CHECK(bl_fault_parse(&hb, "200:cut-peer:0"));
  bl_metrics mh;
  bl_run(&hb, &mh);
  T_CHECK(!mh.handover_done);
  T_CHECK(mh.double_cmds > 0u); /* the standby takes over on a suspicion */
  bl_config ls = base(bl_impl_lease(), 20);
  T_CHECK(bl_fault_parse(&ls, "200:cut-peer:0"));
  bl_metrics ml;
  bl_run(&ls, &ml);
  T_CHECK(!ml.handover_done);
  T_EQ_U(ml.double_cmds, 0u); /* the lease is renewed, nobody takes over */
}

static void test_the_planned_handover(void) {
  t_section("a planned step-down: the cession and the lease release are fast");
  bl_config rl = base(bl_impl_relais(), 20);
  T_CHECK(bl_fault_parse(&rl, "200:handover:0:40"));
  bl_metrics mr;
  bl_run(&rl, &mr);
  T_CHECK(mr.handover_done);
  T_EQ_U(mr.handover_ticks, 1u);
  bl_config ls = base(bl_impl_lease(), 20);
  T_CHECK(bl_fault_parse(&ls, "200:handover:0:40"));
  bl_metrics ml;
  bl_run(&ls, &ml);
  T_CHECK(ml.handover_done);
  T_EQ_U(ml.handover_ticks, 1u); /* the lease is released, the peer takes it */
  bl_config hb = base(bl_impl_heartbeat(), 20);
  T_CHECK(bl_fault_parse(&hb, "200:handover:0:40"));
  bl_metrics mh;
  bl_run(&hb, &mh);
  T_EQ_U(mh.handover_ticks, 20u); /* the watchdog of the actuator waits */
}

static void test_nominal_is_quiet(void) {
  t_section("without any fault, nobody changes of commander");
  const bl_impl *impls[3] = {bl_impl_relais(), bl_impl_heartbeat(), bl_impl_lease()};
  for (int i = 0; i < 3; i++) {
    bl_config cfg = base(impls[i], 20);
    bl_metrics m;
    bl_run(&cfg, &m);
    T_CHECK(!m.handover_done);
    T_EQ_U(m.double_cmds, 0u);
  }
}

static void test_the_bench_is_reproducible(void) {
  t_section("the same seed gives the same numbers");
  bl_config a = base(bl_impl_relais(), 20);
  bl_config b = base(bl_impl_relais(), 20);
  T_CHECK(bl_fault_parse(&a, "200:death:0"));
  T_CHECK(bl_fault_parse(&b, "200:death:0"));
  bl_metrics ma;
  bl_metrics mb;
  bl_run(&a, &ma);
  bl_run(&b, &mb);
  T_EQ_U(ma.handover_ticks, mb.handover_ticks);
  T_EQ_U(ma.stale_ticks, mb.stale_ticks);
  T_NEAR(ma.twin_gap_max, mb.twin_gap_max, 1e-9);
}

int main(void) {
  test_fault_parse();
  test_relais_handover_is_two_ticks();
  test_heartbeat_waits_for_its_timeout();
  test_lease_waits_for_its_lease();
  test_the_heartbeat_splits_the_brain_on_return();
  test_the_relais_handover_survives_a_cut_peer_link();
  test_the_baselines_do_not_hand_over_on_a_cut_link();
  test_the_planned_handover();
  test_nominal_is_quiet();
  test_the_bench_is_reproducible();
  return t_summary();
}
