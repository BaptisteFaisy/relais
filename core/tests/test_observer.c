/* Relais — the state observer (OBS-02, REP-04, mission M30).
 *
 *   tracking   with a clean measurement the observer converges on the rack;
 *   filtering  with a noisy measurement the estimate is closer to the truth
 *              than the measurement is, and the gain is published;
 *   blindness  with no measurement at all the model carries the estimate, and
 *              the drift is measured BOTH with a perfect model and with a
 *              model that does not match the rack (the realistic case);
 *   CMD-03     two observers fed the same inputs give the same bits;
 *   REP-04     the handover bump of a second that rebuilds from the observer
 *              against the published rebuild from the raw sample.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fixtures_control.h"
#include "relais/observer.h"
#include "relais/plant_ext.h"
#include "test_util.h"

/* A deterministic normal-ish draw, mean 0, standard deviation sigma. Twelve
 * uniforms is the classic approximation and keeps the test reproducible. */
static float noise(t_rng *r, float sigma) {
  double sum = 0.0;
  for (int i = 0; i < 12; i++) sum += (double)t_rng_below(r, 1000000u) / 1000000.0;
  return (float)((sum - 6.0) * (double)sigma);
}

/* The rack under test: the published model, or the faithful one of mission
 * M30. One wrapper, so every observer test can choose its truth without
 * duplicating the loop. */
typedef struct {
  relais_plant plain;
  relais_plant_ext ext;
  bool faithful;
} obs_rack;

static void obs_rack_init(obs_rack *w, bool faithful) {
  memset(w, 0, sizeof *w);
  w->faithful = faithful;
  relais_plant_init(&w->plain, 0.0f, 0u);
  if (faithful) {
    relais_phys_params phys;
    relais_phys_params_demo(&phys);
    relais_plant_ext_init(&w->ext, 0.0f, &phys);
  }
}

static void obs_rack_step(obs_rack *w, float value, float dt) {
  if (w->faithful) relais_plant_ext_step(&w->ext, value, 0.0f, dt);
  else relais_plant_step(&w->plain, value, 0.0f, dt);
}

static float obs_rack_pos(const obs_rack *w) { return w->faithful ? w->ext.pos : w->plain.pos; }
static float obs_rack_vel(const obs_rack *w) { return w->faithful ? w->ext.vel : w->plain.vel; }

typedef struct {
  double raw_se;    /* sum of the squared errors of the measurement */
  double est_se;    /* sum of the squared errors of the estimate */
  double blind_err; /* worst position error reached while blind */
  double worst_err; /* worst position error over the whole run */
  long count;
} obs_stats;

/* One closed loop over the rack; the observer is fed the command that was
 * applied and a measurement of the position, `sigma` being the noise of that
 * measurement and [blind_from, blind_to) the ticks where it is absent. */
static void obs_loop(const relais_params *p, const relais_traj *tr, float sigma, long ticks,
                     long blind_from, long blind_to, uint64_t seed, bool faithful,
                     obs_stats *st) {
  const float dt = (float)relais_params_period_s(p);
  obs_rack rack;
  obs_rack_init(&rack, faithful);
  relais_ctl_state ctl;
  relais_ctl_init(&ctl, p);
  relais_obs_params op;
  relais_obs_params_default(&op);
  relais_observer obs;
  relais_observer_init(&obs, &op, 0.0f, 0.0f);
  t_rng rng;
  t_rng_seed(&rng, seed);
  memset(st, 0, sizeof *st);
  float previous = 0.0f;
  for (long i = 0; i < ticks; i++) {
    double t = (double)i * (double)dt;
    relais_meas m;
    m.pos = obs_rack_pos(&rack);
    m.vel = obs_rack_vel(&rack);
    m.applied = previous;
    m.setpoint = relais_traj_setpoint(tr, t + (double)dt);
    previous = relais_ctl_step(&ctl, &m, p);
    obs_rack_step(&rack, previous, dt);
    bool blind = i >= blind_from && i < blind_to;
    float meas = obs_rack_pos(&rack) + (sigma > 0.0f ? noise(&rng, sigma) : 0.0f);
    relais_observer_step(&obs, previous, meas, !blind, dt);
    double est_err = (double)obs.pos - (double)obs_rack_pos(&rack);
    if (fabs(est_err) > st->worst_err) st->worst_err = fabs(est_err);
    if (!blind) {
      double raw_err = (double)meas - (double)obs_rack_pos(&rack);
      st->raw_se += raw_err * raw_err;
      st->est_se += est_err * est_err;
      st->count++;
    } else if (fabs(est_err) > st->blind_err) {
      st->blind_err = fabs(est_err);
    }
  }
}

static void test_observer_tracks_the_rack(void) {
  t_section("M30 the observer converges on the rack with a clean measurement");
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  obs_stats st;
  obs_loop(&p, &tr, 0.0f, 4000, -1, -1, 1u, false, &st);
  double rmse = sqrt(st.est_se / (double)st.count);
  T_CHECK(rmse < 1e-4);
  printf("  observer, exact model, no noise: position RMSE = %.3e\n", rmse);
  /* With the faithful rack as the truth, the observer keeps a model mismatch
   * error, and it stays small: that is the honest accuracy of the estimate. */
  obs_stats mismatch;
  obs_loop(&p, &tr, 0.0f, 4000, -1, -1, 1u, true, &mismatch);
  double rmse_mismatch = sqrt(mismatch.est_se / (double)mismatch.count);
  T_CHECK(rmse_mismatch < 0.05);
  printf("  observer, faithful rack as truth: position RMSE = %.6f, worst = %.6f\n",
         rmse_mismatch, mismatch.worst_err);
}

static void test_observer_filters_the_noise(void) {
  t_section("M30 the observer filters the measurement noise");
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  const float sigmas[] = {0.001f, 0.005f, 0.02f};
  for (size_t k = 0; k < sizeof sigmas / sizeof sigmas[0]; k++) {
    obs_stats st;
    obs_loop(&p, &tr, sigmas[k], 4000, -1, -1, 11u + (uint64_t)k, false, &st);
    double raw = sqrt(st.raw_se / (double)st.count);
    double est = sqrt(st.est_se / (double)st.count);
    T_CHECK(est < raw); /* the estimate beats the measurement */
    printf("  sigma=%.3f: measurement RMSE=%.5f estimate RMSE=%.5f gain=%.2f\n",
           (double)sigmas[k], raw, est, raw / est);
  }
}

static void test_observer_bridges_a_blind_window(void) {
  t_section("M30 the observer carries the estimate through a sensor outage");
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  /* With the exact model the prediction is exact: the outage costs nothing. */
  obs_stats st;
  obs_loop(&p, &tr, 0.0f, 4000, 1000, 1020, 5u, false, &st);
  T_CHECK(st.blind_err < 1e-4);
  /* With the faithful rack as truth, the model mismatch shows up as a drift
   * during the outage, and the observer recovers once the sensor is back. */
  obs_stats mismatch;
  obs_loop(&p, &tr, 0.0f, 4000, 1000, 1020, 5u, true, &mismatch);
  T_CHECK(isfinite(mismatch.blind_err));
  T_CHECK(mismatch.blind_err < 0.1);
  /* A long outage still leaves a finite, bounded estimate. */
  obs_stats long_blind;
  obs_loop(&p, &tr, 0.0f, 4000, 500, 4000, 6u, true, &long_blind);
  T_CHECK(isfinite(long_blind.blind_err));
  T_CHECK(long_blind.blind_err < 5.0);
  printf("  blind 20 ticks: exact model %.3e, faithful rack %.6f\n", st.blind_err,
         mismatch.blind_err);
  printf("  blind 3500 ticks on the faithful rack: worst error %.6f\n", long_blind.blind_err);
}

static void test_observer_is_deterministic(void) {
  t_section("M30 CMD-03 two observers, same inputs, same bits");
  relais_obs_params op;
  relais_obs_params_default(&op);
  relais_observer a, b;
  relais_observer_init(&a, &op, 0.0f, 0.0f);
  relais_observer_init(&b, &op, 0.0f, 0.0f);
  t_rng rng;
  t_rng_seed(&rng, 0x0B5Eu);
  long mismatches = 0;
  for (long i = 0; i < 10000; i++) {
    float applied = (float)((int)t_rng_below(&rng, 2001u) - 1000) / 1000.0f;
    float meas = (float)((int)t_rng_below(&rng, 2001u) - 1000) / 1000.0f;
    bool valid = t_rng_below(&rng, 10u) != 0;
    relais_observer_step(&a, applied, meas, valid, 0.005f);
    relais_observer_step(&b, applied, meas, valid, 0.005f);
    if (memcmp(&a.pos, &b.pos, sizeof(float)) != 0 ||
        memcmp(&a.vel, &b.vel, sizeof(float)) != 0)
      mismatches++;
  }
  T_EQ_U((uint64_t)mismatches, 0u);
  T_EQ_U(a.steps, b.steps);
  T_EQ_U(a.blind, b.blind);
}

static void test_observer_survives_garbage(void) {
  t_section("M30 the observer never hands a non finite estimate to the law");
  relais_obs_params op;
  relais_obs_params_default(&op);
  relais_observer o;
  relais_observer_init(&o, &op, 0.0f, 0.0f);
  const float bad[] = {NAN, INFINITY, -INFINITY};
  for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
    relais_observer_step(&o, bad[k], bad[k], true, 0.005f);
    T_CHECK(isfinite(o.pos) && isfinite(o.vel));
  }
  relais_observer_step(&o, 1.0f, 1.0f, true, 0.0f);     /* dt <= 0: ignored */
  relais_observer_step(NULL, 1.0f, 1.0f, true, 0.005f); /* no crash */
  T_CHECK(isfinite(o.pos) && isfinite(o.vel));
  char buf[128];
  T_CHECK(relais_observer_status_text(&o, buf, sizeof buf) > 0);
  T_CHECK(strstr(buf, "observer") != NULL);
}

/* The handover of REP-04, with and without the observer: the second has lost
 * the replica and rebuilds its state from what it can measure. The bump is the
 * distance between the first order of the successor and the value the holder
 * had announced in its will (REL-03). */
typedef struct {
  relais_params p;
  relais_plant pl;
  relais_ctl_state holder;
  relais_observer obs;
  relais_meas last;   /* what the second saw on its own sensor */
  relais_meas exact;  /* what the actuator really published */
  float previous;     /* last applied command */
} rep04_run;

/* Drive the holder on the exact measurement and the second's observer on its
 * own noisy channel, up to the instant of the handover. */
static void rep04_drive(rep04_run *r, float sigma, uint64_t seed, long ticks) {
  relais_traj tr;
  relais_traj_default(&tr);
  relais_params_default(&r->p);
  relais_plant_init(&r->pl, 0.0f, 0u);
  relais_ctl_init(&r->holder, &r->p);
  relais_obs_params op;
  relais_obs_params_default(&op);
  relais_observer_init(&r->obs, &op, 0.0f, 0.0f);
  t_rng rng;
  t_rng_seed(&rng, seed);
  const float dt = (float)relais_params_period_s(&r->p);
  r->previous = 0.0f;
  memset(&r->last, 0, sizeof r->last);
  for (long i = 0; i < ticks; i++) {
    double t = (double)i * (double)dt;
    relais_meas m;
    m.pos = r->pl.pos;
    m.vel = r->pl.vel;
    m.applied = r->previous;
    m.setpoint = relais_traj_setpoint(&tr, t + (double)dt);
    r->previous = relais_ctl_step(&r->holder, &m, &r->p);
    relais_plant_step(&r->pl, r->previous, 0.0f, dt);
    /* What the second sees: its own sensor, noisy, and the applied value. */
    r->last.pos = r->pl.pos + noise(&rng, sigma);
    r->last.vel = r->pl.vel + noise(&rng, sigma * 4.0f);
    r->last.applied = r->previous;
    r->last.setpoint = relais_traj_setpoint(&tr, t + 2.0 * (double)dt);
    relais_observer_step(&r->obs, r->previous, r->last.pos, true, dt);
  }
  r->exact.pos = r->pl.pos;
  r->exact.vel = r->pl.vel;
  r->exact.applied = r->previous;
  r->exact.setpoint = r->last.setpoint;
}

/* The first order of the successor, rebuilt either from the raw sample (the
 * published REP-04) or from the observer estimate. */
static float rep04_first_order(rep04_run *r, bool use_observer) {
  relais_ctl_state state;
  relais_meas seen = r->last;
  relais_ctl_init(&state, &r->p);
  if (use_observer) {
    relais_ctl_reconstruct_observer(&state, &r->obs, &seen, &r->p);
    relais_observer_meas(&r->obs, &seen, &seen);
  } else {
    relais_ctl_reconstruct(&state, &seen, &r->p);
  }
  return relais_ctl_step(&state, &seen, &r->p);
}

static void rep04_bump(float sigma, uint64_t seed, double *raw_bump, double *obs_bump) {
  rep04_run run;
  rep04_drive(&run, sigma, seed, 2000);
  float will[RELAIS_TESTAMENT_MAX];
  uint8_t will_len = 0;
  relais_testament_fill(&run.holder, &run.exact, &run.p, will, &will_len);
  T_CHECK(will_len > 0);
  *raw_bump = fabs((double)rep04_first_order(&run, false) - (double)will[0]);
  *obs_bump = fabs((double)rep04_first_order(&run, true) - (double)will[0]);
}

static void test_rep04_with_and_without_the_observer(void) {
  t_section("M30 REP-04 the observer against the raw rebuild at the handover");
  const float sigmas[] = {0.001f, 0.005f, 0.02f};
  for (size_t k = 0; k < sizeof sigmas / sizeof sigmas[0]; k++) {
    double raw_sum = 0.0, obs_sum = 0.0, raw_max = 0.0, obs_max = 0.0;
    const int runs = 24;
    for (int r = 0; r < runs; r++) {
      double raw_bump = 0.0, obs_bump = 0.0;
      rep04_bump(sigmas[k], 100u + (uint64_t)r, &raw_bump, &obs_bump);
      raw_sum += raw_bump;
      obs_sum += obs_bump;
      if (raw_bump > raw_max) raw_max = raw_bump;
      if (obs_bump > obs_max) obs_max = obs_bump;
    }
    double raw_mean = raw_sum / (double)runs;
    double obs_mean = obs_sum / (double)runs;
    T_CHECK(obs_mean < raw_mean); /* the observer reduces the handover bump */
    printf("  sigma=%.3f: raw bump mean=%.6f max=%.6f | observer mean=%.6f max=%.6f "
           "| reduction=%.1f%%\n",
           (double)sigmas[k], raw_mean, raw_max, obs_mean, obs_max,
           100.0 * (raw_mean - obs_mean) / (raw_mean > 0.0 ? raw_mean : 1e-12));
  }
}

int main(void) {
  test_observer_tracks_the_rack();
  test_observer_filters_the_noise();
  test_observer_bridges_a_blind_window();
  test_observer_is_deterministic();
  test_observer_survives_garbage();
  test_rep04_with_and_without_the_observer();
  return t_summary();
}
