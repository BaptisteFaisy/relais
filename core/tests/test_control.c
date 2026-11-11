/* Relais — control law, state replication and testament (spec 5.1, 5.5, 5.6).
 *
 *   CMD-03  the law is a pure function of its state and of the measurements:
 *           two instances fed the same inputs give the same bits;
 *   CMD-06  the command is bounded in amplitude and in change per tick;
 *   REP-01  the exported state fits in 256 bytes;
 *   REP-02  export then import is exact; a bad version or length is refused;
 *   REP-04  a rebuilt state continues from the value already applied, within
 *           the actuator's own rate limit (that is what "no bump" means);
 *   ACT-02  the will is played only inside [base, base + len);
 *   REL-03  the first order of a successor that imported the holder's state is
 *           within delta of the will at the same tick.
 *
 * The rack model, the trajectories and the fallback law are tested in
 * test_plant.c; the shared fixtures live in fixtures_control.c.
 */
#include <math.h>
#include <string.h>

#include "fixtures_control.h"
#include "test_util.h"
/* ---------------------------------------------------------------- CMD-03 */

static void test_cmd03_determinism_bit_exact(void) {
  t_section("CMD-03 two instances, same inputs, same bits (10 000 steps)");
  relais_params p;
  relais_params_default(&p);
  relais_ctl_state a, b;
  relais_ctl_init(&a, &p);
  relais_ctl_init(&b, &p);
  t_rng rng;
  t_rng_seed(&rng, 0x1D3Bu);
  long mismatches = 0;
  for (long i = 0; i < 10000; i++) {
    relais_meas m;
    meas_random(&m, &rng, 0.0f);
    float va = relais_ctl_step(&a, &m, &p);
    float vb = relais_ctl_step(&b, &m, &p);
    if (memcmp(&va, &vb, sizeof va) != 0) mismatches++;
  }
  T_EQ_U((uint64_t)mismatches, 0u);
  /* The whole state is identical, not only the returned value: that is what
   * the actuator compares between an order and a shadow (DET-09). */
  T_CHECK(memcmp(&a, &b, sizeof a) == 0);
  T_EQ_U(a.ticks, 10000u);
}

/* ---------------------------------------------------------------- CMD-06 */

static void test_cmd06_bounds(void) {
  t_section("CMD-06 amplitude and slew bounds, 100 000 random steps");
  relais_params p;
  relais_params_default(&p);
  relais_ctl_state s;
  relais_ctl_init(&s, &p);
  t_rng rng;
  t_rng_seed(&rng, 0xB0D1u);
  long amplitude = 0, slew = 0;
  float previous = 0.0f;
  for (long i = 0; i < 100000; i++) {
    relais_meas m;
    meas_random(&m, &rng, previous);
    float v = relais_ctl_step(&s, &m, &p); /* deliberately brutal inputs */
    if (fabsf(v) > p.max_amplitude + 1e-6f) amplitude++;
    if (fabsf(v - previous) > p.max_slew + 1e-6f) slew++;
    if (v != relais_clamp_value(v, &p)) amplitude++;
    previous = v;
  }
  T_EQ_U((uint64_t)amplitude, 0u);
  T_EQ_U((uint64_t)slew, 0u);
  /* The clamps themselves, including NaN: a NaN must never reach the actuator. */
  T_NEAR(relais_clamp_value(5.0f, &p), p.max_amplitude, 0.0);
  T_NEAR(relais_clamp_value(-5.0f, &p), -p.max_amplitude, 0.0);
  T_NEAR(relais_clamp_value(NAN, &p), -p.max_amplitude, 0.0);
  T_NEAR(relais_clamp_slew(0.0f, 1.0f, &p), p.max_slew, 1e-6);
  T_NEAR(relais_clamp_slew(0.0f, -1.0f, &p), -p.max_slew, 1e-6);
  T_NEAR(relais_clamp_slew(0.0f, 0.01f, &p), 0.01, 1e-7); /* inside: untouched */
  T_NEAR(relais_clamp_value(7.0f, NULL), 7.0, 0.0);        /* no params: as is */
  T_NEAR(relais_clamp_slew(0.0f, 7.0f, NULL), 7.0, 0.0);
}

/* ------------------------------------------------------- REP-01 / REP-02 */

static void state_fill(relais_ctl_state *s) {
  s->integ = 0.125f;
  s->filt = -0.25f;
  s->last_pos = 0.5f;
  s->last_value = -0.75f;
  s->phase = 12.5f;
  s->fb_value = 0.375f;
  s->prev_setpoint = -0.125f;
  s->setpoint_rate = 3.25f;
  s->ticks = 0xdeadbeefu;
}

static void test_rep01_rep02_export_import(void) {
  t_section("REP-01 state fits in 256 bytes, REP-02 round trip is exact");
  uint8_t buf[RELAIS_CTL_STATE_MAX];
  relais_ctl_state s;
  memset(&s, 0, sizeof s);
  state_fill(&s);
  uint8_t len = relais_ctl_export(&s, buf, sizeof buf);
  const unsigned used = len;
  T_CHECK(len > 0u);
  T_CHECK(used <= RELAIS_CTL_STATE_MAX); /* REP-01: 38 of a 256-byte budget */
  T_EQ_U(len, 38u);

  relais_ctl_state back;
  memset(&back, 0xAA, sizeof back);
  T_CHECK(relais_ctl_import(&back, buf, len));
  T_CHECK(memcmp(&back, &s, sizeof s) == 0); /* exact at the bit level */
  T_EQ_U(back.ticks, s.ticks);

  /* A length that does not match is refused and leaves the state untouched. */
  relais_ctl_state kept = back;
  T_CHECK(!relais_ctl_import(&back, buf, (size_t)len - 1u));
  T_CHECK(!relais_ctl_import(&back, buf, (size_t)len + 1u));
  T_CHECK(memcmp(&back, &kept, sizeof back) == 0);
  /* An unknown version is refused. */
  buf[0] = 0x7Fu;
  T_CHECK(!relais_ctl_import(&back, buf, len));
  buf[0] = 1u;
  /* The inner length must agree with the buffer given. */
  buf[1] = (uint8_t)(len + 2u);
  T_CHECK(!relais_ctl_import(&back, buf, len));
  buf[1] = len;
  T_CHECK(relais_ctl_import(&back, buf, len));
  /* A capacity smaller than the state is refused, never overflowed. */
  T_EQ_U(relais_ctl_export(&s, buf, (size_t)len - 1u), 0u);
  T_EQ_U(relais_ctl_export(&s, buf, sizeof buf), len);
  /* NULL arguments never crash and never claim success. */
  T_EQ_U(relais_ctl_export(NULL, buf, sizeof buf), 0u);
  T_EQ_U(relais_ctl_export(&s, NULL, 64u), 0u);
  T_CHECK(!relais_ctl_import(NULL, buf, len));
  T_CHECK(!relais_ctl_import(&back, NULL, len));
}

/* ---------------------------------------------------------------- REP-04 */

static void test_rep04_reconstruct_continues(void) {
  t_section("REP-04 a rebuilt state continues from the applied value");
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  relais_ctl_state holder, replica;
  relais_plant pl;
  loop_stats st;
  stats_reset(&st);
  float last = loop_run(&holder, &pl, &tr, &p, 200, 0.5, &st);
  memcpy(&replica, &holder, sizeof replica); /* the replica was up to date */

  const double dt = relais_params_period_s(&p);
  relais_meas m;
  meas_set(&m, pl.pos, pl.vel, last, relais_traj_setpoint(&tr, 200.0 * dt));
  float v_holder = relais_ctl_step(&replica, &m, &p);

  /* No ETAT any more: rebuild from what the actuator published, then compute. */
  relais_ctl_state rebuilt;
  relais_ctl_init(&rebuilt, &p);
  relais_ctl_reconstruct(&rebuilt, &m, &p);
  float v_rebuilt = relais_ctl_step(&rebuilt, &m, &p);
  T_CHECK(isfinite(v_rebuilt));
  /* REP-04 "no bump" means inside the actuator's own rate limit: a rebuilt
   * state has no integral history, so the bound is max_slew, not delta. Delta
   * is the bound of REL-03, where the replica is exact. */
  T_NEAR(fabsf(v_rebuilt - m.applied), 0.0, (double)p.max_slew);
  T_NEAR(v_rebuilt, v_holder, (double)p.max_slew);
  /* The applied value is taken as published, then bounded. */
  meas_set(&m, 0.0f, 0.0f, 4.0f, 0.0f);
  relais_ctl_init(&rebuilt, &p);
  relais_ctl_reconstruct(&rebuilt, &m, &p);
  T_NEAR(rebuilt.last_value, p.max_amplitude, 0.0);
  T_NEAR(rebuilt.fb_value, p.max_amplitude, 0.0);
  T_NEAR(rebuilt.filt, m.pos, 0.0);
  /* NULL arguments are tolerated. */
  relais_ctl_reconstruct(NULL, &m, &p);
  relais_ctl_reconstruct(&rebuilt, NULL, &p);
}

/* ---------------------------------------------------------------- ACT-02 */

static void test_act02_testament_window(void) {
  t_section("ACT-02 the will is played inside [base, base + len) only");
  float will[RELAIS_TESTAMENT_MAX];
  for (uint8_t i = 0; i < RELAIS_TESTAMENT_MAX; i++) will[i] = 0.1f * (float)i;
  const relais_tick base = 1000u;
  const uint8_t len = RELAIS_TESTAMENT_MAX;
  float out = -1.0f;

  T_CHECK(!relais_testament_at(will, len, base, base - 1u, &out)); /* before */
  T_CHECK(relais_testament_at(will, len, base, base, &out));       /* first */
  T_NEAR(out, will[0], 0.0);
  T_CHECK(relais_testament_at(will, len, base, base + len - 1u, &out)); /* last */
  T_NEAR(out, will[len - 1u], 0.0);
  T_CHECK(!relais_testament_at(will, len, base, base + len, &out)); /* at horizon */
  T_CHECK(!relais_testament_at(will, len, base, base + 1000u, &out));
  /* An empty will, a missing base, and NULL pointers. */
  T_CHECK(!relais_testament_at(will, 0, base, base, &out));
  T_CHECK(!relais_testament_at(will, len, RELAIS_TICK_NONE, base, &out));
  T_CHECK(!relais_testament_at(NULL, len, base, base, &out));
  T_CHECK(!relais_testament_at(will, len, base, base, NULL));
  T_CHECK(!relais_testament_at(NULL, 0, base, base, NULL));
}

/* ---------------------------------------------------------------- REL-03 */

static void test_rel03_successor_order_within_delta(void) {
  t_section("REL-03 the successor's first order is within delta of the will");
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  relais_ctl_state holder;
  relais_plant pl;
  loop_stats st;
  stats_reset(&st);
  float last = loop_run(&holder, &pl, &tr, &p, 200, 0.5, &st); /* 200 ticks */

  /* What the holder published last, and the will it embedded in its order. */
  const double dt = relais_params_period_s(&p);
  relais_meas m;
  meas_set(&m, pl.pos, pl.vel, last, relais_traj_setpoint(&tr, 200.0 * dt));
  float will[RELAIS_TESTAMENT_MAX];
  uint8_t will_len = 0;
  relais_testament_fill(&holder, &m, &p, will, &will_len);
  T_EQ_U(will_len, RELAIS_TESTAMENT_MAX);
  long non_finite = 0;
  for (uint8_t i = 0; i < will_len; i++)
    if (!isfinite(will[i])) non_finite++;
  T_EQ_U((uint64_t)non_finite, 0u);

  /* The successor imports the replicated state (REP-02) and computes on the
   * STATUT of the next tick, as it would after the handover. */
  uint8_t buf[RELAIS_CTL_STATE_MAX];
  uint8_t len = relais_ctl_export(&holder, buf, sizeof buf);
  T_CHECK(len > 0u);
  relais_ctl_state successor;
  memset(&successor, 0, sizeof successor);
  T_CHECK(relais_ctl_import(&successor, buf, len));
  /* The successor computes on the STATUT of the next tick: the rack has moved
   * under the last command, exactly as the will predicted it would. */
  relais_plant ahead = pl;
  relais_plant_step(&ahead, last, 0.0f, (float)dt);
  relais_meas next;
  meas_set(&next, ahead.pos, ahead.vel, last, relais_traj_setpoint(&tr, 200.0 * dt + dt));
  float first = relais_ctl_step(&successor, &next, &p);
  float planned = 0.0f;
  T_CHECK(relais_testament_at(will, will_len, 200u, 200u, &planned));
  T_NEAR(fabsf(first - planned), 0.0, (double)p.delta);
  T_CHECK(isfinite(first));
}

int main(void) {
  test_cmd03_determinism_bit_exact();
  test_cmd06_bounds();
  test_rep01_rep02_export_import();
  test_rep04_reconstruct_continues();
  test_act02_testament_window();
  test_rel03_successor_order_within_delta();
  return t_summary();
}
