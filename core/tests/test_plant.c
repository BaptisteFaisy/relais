/* Relais — rack model, reference trajectories and fallback law (spec 5.1, 5.6).
 *
 *   CMD-05  the rack model and the trajectories are bounded, continuous and
 *           deterministic, and the disturbance is a pure function of time and
 *           seed, so the twin without fault and the faulted run agree;
 *   CMD-08  the braking profile;
 *   ACT-04  leaving the fallback towards a valid order is rate limited.
 *
 * The shared fixtures live in fixtures_control.c.
 */
#include <math.h>
#include <string.h>

#include "fixtures_control.h"
#include "test_util.h"
/* ------------------------------------------------- CMD-05 trajectories */

/* A reference never jumps: the largest allowed step between two ticks. */
#define TRAJ_MAX_STEP 0.05

static void test_cmd05_trajectories_bounded_and_continuous(void) {
  t_section("CMD-05 every trajectory is bounded and continuous");
  const relais_traj_kind kinds[] = {RELAIS_TRAJ_SLALOM, RELAIS_TRAJ_LANE_CHANGE,
                                    RELAIS_TRAJ_STEP, RELAIS_TRAJ_CONST,
                                    RELAIS_TRAJ_BRAKE};
  relais_traj t;
  relais_traj_default(&t);
  T_EQ_U(t.kind, RELAIS_TRAJ_SLALOM);
  T_CHECK(t.amp > 0.0f);
  /* Slalom: a sine of the given amplitude, 0.5 Hz by default. */
  T_NEAR(t.freq, 0.5, 1e-9);
  T_NEAR(relais_traj_setpoint(&t, 0.0), 0.0, 1e-6);
  T_NEAR(relais_traj_setpoint(&t, 0.5), t.amp, 1e-5);
  T_NEAR(relais_traj_setpoint(&t, 1.0), 0.0, 1e-5);
  T_NEAR(relais_traj_setpoint(&t, 1.5), -t.amp, 1e-5);
  for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
    relais_traj_default(&t);
    t.kind = kinds[k];
    double previous = relais_traj_setpoint(&t, 0.0);
    long out_of_bound = 0, jumps = 0, non_finite = 0;
    for (int i = 1; i <= 4000; i++) {
      double now = relais_traj_setpoint(&t, (double)i * 0.005);
      if (fabs(now) > fabs((double)t.amp) + fabs((double)t.offset) + 1e-6) out_of_bound++;
      if (fabs(now - previous) > TRAJ_MAX_STEP) jumps++;
      if (!isfinite(now)) non_finite++;
      previous = now;
    }
    T_EQ_U((uint64_t)out_of_bound, 0u);
    T_EQ_U((uint64_t)non_finite, 0u);
    /* Only the step is discontinuous by design (CMD-05 lists the others). */
    if (kinds[k] != RELAIS_TRAJ_STEP) T_EQ_U((uint64_t)jumps, 0u);
  }
}

/* One call, one asserted value: keeps the point-by-point table short. */
static void expect_at(relais_traj_kind kind, float t_start, double seconds, double want) {
  relais_traj t;
  relais_traj_default(&t);
  t.kind = kind;
  t.t_start = t_start;
  T_NEAR(relais_traj_setpoint(&t, seconds), want, 1e-6);
}

static void test_cmd05_trajectory_kinds_exact_points(void) {
  t_section("CMD-05 each kind at known points");
  const float amp = 0.6f; /* relais_traj_default() */
  /* Lane change: from -amp to +amp over two seconds after t_start. */
  expect_at(RELAIS_TRAJ_LANE_CHANGE, 1.0f, 0.0, -amp);
  expect_at(RELAIS_TRAJ_LANE_CHANGE, 1.0f, 1.0, -amp);
  expect_at(RELAIS_TRAJ_LANE_CHANGE, 1.0f, 2.0, 0.0);
  expect_at(RELAIS_TRAJ_LANE_CHANGE, 1.0f, 3.0, amp);
  expect_at(RELAIS_TRAJ_LANE_CHANGE, 1.0f, 9.0, amp);
  /* Step: -amp before t_start, +amp from t_start on. */
  expect_at(RELAIS_TRAJ_STEP, 1.0f, 0.0, -amp);
  expect_at(RELAIS_TRAJ_STEP, 1.0f, 0.999, -amp);
  expect_at(RELAIS_TRAJ_STEP, 1.0f, 1.0, amp);
  expect_at(RELAIS_TRAJ_STEP, 1.0f, 20.0, amp);
  /* Constant: the offset, always. */
  expect_at(RELAIS_TRAJ_CONST, 1.0f, 0.0, 0.0);
  expect_at(RELAIS_TRAJ_CONST, 1.0f, 42.0, 0.0);
  /* Brake (CMD-08): a one-second ramp, then a hold. */
  expect_at(RELAIS_TRAJ_BRAKE, 1.0f, 0.0, 0.0);
  expect_at(RELAIS_TRAJ_BRAKE, 1.0f, 1.0, 0.0);
  expect_at(RELAIS_TRAJ_BRAKE, 1.0f, 1.5, -amp * 0.5);
  expect_at(RELAIS_TRAJ_BRAKE, 1.0f, 2.0, -amp);
  expect_at(RELAIS_TRAJ_BRAKE, 1.0f, 5.0, -amp);
  /* A NULL trajectory is the neutral reference, not a crash. */
  T_NEAR(relais_traj_setpoint(NULL, 3.0), 0.0, 0.0);
}

static void test_cmd05_disturbance_is_pure(void) {
  t_section("CMD-05 the disturbance is a pure function of time and seed");
  relais_traj t;
  relais_traj_default(&t);
  T_CHECK(t.dist_amp > 0.0f);
  /* Same seed, same time: the same value, bit for bit. */
  T_CHECK(relais_traj_disturbance(&t, 1.2345) == relais_traj_disturbance(&t, 1.2345));
  T_CHECK(relais_traj_disturbance(&t, 17.0) == relais_traj_disturbance(&t, 17.0));
  /* A different seed changes the value. */
  const float with_seed = relais_traj_disturbance(&t, 1.2345);
  relais_traj other = t;
  other.seed = t.seed + 1u;
  T_CHECK(relais_traj_disturbance(&other, 1.2345) != with_seed);
  /* The amplitude bounds it, over a long sweep. */
  long out_of_bound = 0;
  for (int i = 0; i < 5000; i++) {
    float d = relais_traj_disturbance(&t, (double)i * 0.005);
    if (fabsf(d) > t.dist_amp + 1e-6f || !isfinite(d)) out_of_bound++;
  }
  T_EQ_U((uint64_t)out_of_bound, 0u);
  /* No disturbance asked for: exactly zero, whatever the time. */
  relais_traj quiet = t;
  quiet.dist_amp = 0.0f;
  T_NEAR(relais_traj_disturbance(&quiet, 3.0), 0.0, 0.0);
  T_NEAR(relais_traj_disturbance(NULL, 3.0), 0.0, 0.0);
}

static void test_cmd05_traj_parse(void) {
  t_section("CMD-05 trajectory parsing: documented keys, unknown refused");
  relais_traj t;
  char why[128];
  relais_traj_default(&t);
  T_CHECK(relais_traj_parse(&t, "kind=slalom amp=0.6 freq=0.5 offset=0.1 t_start=2 "
                                "dist_amp=0.05 dist_freq=2 seed=11", why, sizeof why));
  T_EQ_U(t.kind, RELAIS_TRAJ_SLALOM);
  T_NEAR(t.amp, 0.6, 1e-6);
  T_NEAR(t.freq, 0.5, 1e-6);
  T_NEAR(t.offset, 0.1, 1e-6);
  T_NEAR(t.t_start, 2.0, 1e-6);
  T_NEAR(t.dist_amp, 0.05, 1e-6);
  T_NEAR(t.dist_freq, 2.0, 1e-6);
  T_EQ_U(t.seed, 11u);
  /* Every documented kind is recognised. */
  T_CHECK(relais_traj_parse(&t, "kind=lane_change", why, sizeof why));
  T_EQ_U(t.kind, RELAIS_TRAJ_LANE_CHANGE);
  T_CHECK(relais_traj_parse(&t, "kind=step", why, sizeof why));
  T_EQ_U(t.kind, RELAIS_TRAJ_STEP);
  T_CHECK(relais_traj_parse(&t, "kind=const", why, sizeof why));
  T_EQ_U(t.kind, RELAIS_TRAJ_CONST);
  T_CHECK(relais_traj_parse(&t, "kind=brake", why, sizeof why));
  T_EQ_U(t.kind, RELAIS_TRAJ_BRAKE);
  /* Refusals: an unknown key, an unknown kind, a NULL trajectory. */
  T_CHECK(!relais_traj_parse(&t, "kind=slalom warp=3", why, sizeof why));
  T_CHECK(strstr(why, "unknown key") != NULL);
  T_CHECK(!relais_traj_parse(&t, "kind=spiral", why, sizeof why));
  T_CHECK(!relais_traj_parse(NULL, "kind=step", why, sizeof why));
  /* An empty body keeps the defaults. */
  relais_traj fresh;
  relais_traj_default(&fresh);
  T_CHECK(relais_traj_parse(&fresh, "", why, sizeof why));
  T_EQ_U(fresh.kind, RELAIS_TRAJ_SLALOM);
}

/* --------------------------------------------- CMD-05 rack model, tracking */

static void test_cmd05_plant_stability(void) {
  t_section("CMD-05 rack model: 100 000 steps, no NaN, no divergence");
  relais_params p;
  relais_params_default(&p);
  const float dt = (float)relais_params_period_s(&p);
  relais_plant pl;
  relais_plant_init(&pl, 0.0f, 3u);
  long non_finite = 0;
  float max_pos = 0.0f, max_vel = 0.0f;
  for (long i = 0; i < 100000; i++) {
    /* A bounded command and a bounded road disturbance, as in the campaigns. */
    float u = 0.9f * sinf(0.01f * (float)i);
    float d = 0.05f * sinf(0.3f * (float)i);
    relais_plant_step(&pl, u, d, dt);
    if (!isfinite(pl.pos) || !isfinite(pl.vel)) non_finite++;
    if (fabsf(pl.pos) > max_pos) max_pos = fabsf(pl.pos);
    if (fabsf(pl.vel) > max_vel) max_vel = fabsf(pl.vel);
  }
  T_EQ_U((uint64_t)non_finite, 0u);
  T_CHECK(max_vel <= 4.0f * 1.05f);  /* gain 4, 5 % of margin */
  T_CHECK(max_pos <= 4.0f * 20.0f);  /* bounded by the dynamics, not by luck */

  /* A step converges to the steady state: velocity = 4 * command. */
  relais_plant_init(&pl, 0.0f, 0u);
  for (int i = 0; i < 4000; i++) relais_plant_step(&pl, 0.5f, 0.0f, dt);
  T_NEAR(pl.vel, 2.0, 1e-3);
  /* A zero dt is a no-op; a NaN command never pollutes the state. */
  relais_plant before = pl;
  relais_plant_step(&pl, 0.5f, 0.0f, 0.0f);
  T_CHECK(memcmp(&pl, &before, sizeof pl) == 0);
  relais_plant_step(&pl, NAN, 0.0f, dt);
  T_CHECK(isfinite(pl.pos) && isfinite(pl.vel));
  relais_plant_step(&pl, 0.5f, NAN, dt);
  T_CHECK(isfinite(pl.pos) && isfinite(pl.vel));
  relais_plant_step(NULL, 0.5f, 0.0f, dt); /* must not crash */
}

static void test_cmd05_tracking_quality(void) {
  t_section("CMD-05 closed loop: 10 % of the amplitude on a 0.5 Hz slalom");
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  relais_ctl_state s;
  relais_plant pl;
  loop_stats st;
  stats_reset(&st);
  loop_run(&s, &pl, &tr, &p, 2000, 2.0, &st); /* error measured after 2 s */
  T_CHECK(st.finite);
  T_CHECK(st.max_err < 0.10f * tr.amp);
  T_CHECK(st.max_value <= p.max_amplitude + 1e-6f);
  T_CHECK(st.max_slew <= p.max_slew + 1e-6f);
  T_CHECK(st.max_pos > 0.9f * tr.amp); /* the rack does follow the slalom */

  /* A step converges: the static error ends below 1 % of the amplitude. */
  relais_traj step_traj;
  relais_traj_default(&step_traj);
  step_traj.kind = RELAIS_TRAJ_STEP;
  step_traj.amp = 0.4f;
  step_traj.t_start = 0.2f;
  loop_stats st2;
  stats_reset(&st2);
  loop_run(&s, &pl, &step_traj, &p, 1500, 2.0, &st2);
  T_CHECK(st2.finite);
  T_CHECK(st2.max_err < 0.01f * step_traj.amp);
  /* The braking profile is followed without leaving the bounds either. */
  relais_traj brake;
  relais_traj_default(&brake);
  brake.kind = RELAIS_TRAJ_BRAKE;
  brake.t_start = 0.5f;
  loop_stats st3;
  stats_reset(&st3);
  loop_run(&s, &pl, &brake, &p, 1000, 2.0, &st3);
  T_CHECK(st3.finite);
  T_CHECK(st3.max_err < 0.10f * brake.amp);
}

/* ---------------------------------------------------------------- ACT-04 */

static void test_act04_fallback_converges_and_hands_over(void) {
  t_section("ACT-04 fallback converges, stays bounded, hands over within slew");
  relais_params p;
  relais_params_default(&p);
  const float dt = (float)relais_params_period_s(&p);
  relais_plant pl;
  relais_plant_init(&pl, 0.3f, 0u);
  relais_meas m;
  meas_set(&m, pl.pos, 0.0f, 0.0f, 0.0f);
  float fb = 0.0f;
  relais_fallback_init(&fb, &m);
  long violations = 0, non_finite = 0;
  float previous = fb;
  for (int i = 0; i < 2000; i++) {
    float v = relais_fallback_step(&fb, &m, &p);
    relais_plant_step(&pl, v, 0.0f, dt);
    meas_set(&m, pl.pos, pl.vel, v, 0.0f);
    if (fabsf(v - previous) > p.max_slew + 1e-6f) violations++;
    if (fabsf(v) > p.max_amplitude + 1e-6f) violations++;
    if (!isfinite(v)) non_finite++;
    previous = v;
  }
  T_EQ_U((uint64_t)violations, 0u);
  T_EQ_U((uint64_t)non_finite, 0u);
  T_NEAR(pl.vel, 0.0, 1e-3); /* the loop damps the rack: it holds */
  T_CHECK(fabsf(fb) <= p.max_amplitude);

  /* ACT-04: the first step back towards a valid order is rate limited too, so
   * leaving the fallback cannot create a bump. */
  const float wanted = relais_clamp_value(0.9f, &p);
  const float handover = relais_clamp_slew(previous, wanted, &p);
  T_NEAR(fabsf(handover - previous), 0.0, (double)p.max_slew);
  float v = handover;
  int ticks = 0;
  while (fabsf(v - wanted) > 1e-5f && ticks < 1000) {
    v = relais_clamp_slew(v, wanted, &p);
    ticks++;
  }
  T_CHECK(ticks > 0);
  T_CHECK(ticks <= (int)(2.0f * p.max_amplitude / p.max_slew) + 1);
  /* A NULL state is a defined answer; a NULL measurement is tolerated. */
  T_NEAR(relais_fallback_step(NULL, &m, &p), 0.0, 0.0);
  float alone = 0.25f;
  T_CHECK(isfinite(relais_fallback_step(&alone, NULL, &p)));
  relais_fallback_init(NULL, &m); /* must not crash */
}

int main(void) {
  test_cmd05_trajectories_bounded_and_continuous();
  test_cmd05_trajectory_kinds_exact_points();
  test_cmd05_disturbance_is_pure();
  test_cmd05_traj_parse();
  test_cmd05_plant_stability();
  test_cmd05_tracking_quality();
  test_act04_fallback_converges_and_hands_over();
  return t_summary();
}
