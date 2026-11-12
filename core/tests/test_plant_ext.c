/* Relais — the faithful rack model (CMD-05, mission M30).
 *
 *   non-regression  a fully zeroed parameter structure is the published model,
 *                   bit for bit, over 100 000 steps;
 *   independence    each phenomenon can be enabled alone;
 *   stability       the faithful model runs 100 000 steps with no NaN, no
 *                   runaway and a bounded mean energy;
 *   behaviour       friction holds the rack, the play delays the motion, the
 *                   current limit caps the acceleration, the stiffness
 *                   centres the rack;
 *   gap             the tracking error of the faithful model against the
 *                   published one, the number the mission asks to publish.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fixtures_control.h"
#include "relais/plant_ext.h"
#include "test_util.h"

/* The faithful variant of the fixture's closed loop: the published one drives
 * relais_plant, which this mission must not change. */
static float loop_run_ext(relais_ctl_state *s, relais_plant_ext *pl, const relais_traj *tr,
                          const relais_params *p, int ticks, double settle_s, loop_stats *st) {
  const float dt = (float)relais_params_period_s(p);
  relais_ctl_init(s, p);
  float previous = 0.0f;
  for (int i = 0; i < ticks; i++) {
    const double t = (double)i * (double)dt;
    relais_meas m;
    m.pos = pl->pos;
    m.vel = pl->vel;
    m.applied = previous;
    m.setpoint = relais_traj_setpoint(tr, t + (double)dt);
    float v = relais_ctl_step(s, &m, p);
    relais_plant_ext_step(pl, v, 0.0f, dt);
    if (!isfinite(v) || !isfinite(pl->pos) || !isfinite(pl->vel)) st->finite = false;
    if (fabsf(v) > st->max_value) st->max_value = fabsf(v);
    if (fabsf(v - previous) > st->max_slew) st->max_slew = fabsf(v - previous);
    if (fabsf(pl->pos) > st->max_pos) st->max_pos = fabsf(pl->pos);
    if (t > settle_s && fabsf(m.setpoint - m.pos) > st->max_err)
      st->max_err = fabsf(m.setpoint - m.pos);
    previous = v;
  }
  return previous;
}

/* A command sequence that exercises the whole range, deterministic. */
static float command_at(int i) {
  double t = (double)i * 0.005;
  if (i % 997 == 0) return i % 1994 == 0 ? 1.0f : -1.0f; /* full scale steps */
  return 0.7f * sinf(6.2831853f * 0.5f * (float)t) + 0.2f * sinf(6.2831853f * 3.0f * (float)t);
}

/* ------------------------------------------- non-regression, bit for bit */

static void test_zeroed_model_is_the_published_one(void) {
  t_section("M30 the zeroed faithful model is the published one, bit for bit");
  relais_phys_params zero;
  relais_phys_params_default(&zero);
  T_CHECK(!relais_phys_params_enabled(&zero));
  relais_plant plain;
  relais_plant_ext faithful;
  relais_plant_init(&plain, 0.0f, 7u);
  relais_plant_ext_init(&faithful, 0.0f, &zero);
  long mismatches = 0;
  for (int i = 0; i < 100000; i++) {
    float value = command_at(i);
    float disturbance = 0.05f * sinf(6.2831853f * 2.0f * (float)i * 0.005f);
    relais_plant_step(&plain, value, disturbance, 0.005f);
    relais_plant_ext_step(&faithful, value, disturbance, 0.005f);
    if (memcmp(&plain.pos, &faithful.pos, sizeof(float)) != 0 ||
        memcmp(&plain.vel, &faithful.vel, sizeof(float)) != 0)
      mismatches++;
  }
  T_EQ_U((uint64_t)mismatches, 0u);
}

/* ------------------------------------------------------- independence */

static void test_each_phenomenon_is_independent(void) {
  t_section("M30 each phenomenon can be enabled alone");
  relais_phys_params base;
  relais_phys_params_default(&base);
  T_CHECK(!relais_phys_params_enabled(&base));
  /* One parameter at a time: the model must change, and only that one. */
  const char *names[] = {"friction", "play", "current", "stiffness"};
  for (int k = 0; k < 4; k++) {
    relais_phys_params p;
    relais_phys_params_default(&p);
    if (k == 0) { p.friction_static = 20.0f; p.friction_kinetic = 12.0f; p.friction_band = 0.02f; }
    if (k == 1) p.backlash = 0.02f;
    if (k == 2) p.current_limit = 50.0f;
    if (k == 3) p.stiffness = 50.0f;
    T_MSG(relais_phys_params_enabled(&p), names[k]);
    relais_plant_ext a, b;
    relais_plant_ext_init(&a, 0.0f, &p);
    relais_plant_ext_init(&b, 0.0f, &base);
    long differs = 0;
    for (int i = 0; i < 2000; i++) {
      relais_plant_ext_step(&a, command_at(i), 0.0f, 0.005f);
      relais_plant_ext_step(&b, command_at(i), 0.0f, 0.005f);
      if (memcmp(&a.pos, &b.pos, sizeof(float)) != 0) differs++;
      T_CHECK(isfinite(a.pos) && isfinite(a.vel));
    }
    T_MSG(differs > 0, names[k]);
  }
}

/* ---------------------------------------------------------- stability */

static void test_faithful_model_is_stable(void) {
  t_section("M30 the faithful model runs 100 000 steps, bounded, no NaN");
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  relais_phys_params phys;
  relais_phys_params_demo(&phys);
  T_CHECK(relais_phys_params_enabled(&phys));
  relais_plant_ext pl;
  relais_plant_ext_init(&pl, 0.0f, &phys);
  relais_ctl_state ctl;
  loop_stats st;
  stats_reset(&st);
  loop_run_ext(&ctl, &pl, &tr, &p, 100000, 1.0, &st);
  T_CHECK(st.finite);
  T_CHECK(st.max_pos < 2.0f);          /* the rack stays inside twice the travel */
  T_CHECK(fabsf(pl.vel) <= 40.0f);     /* the velocity bound of the model */
  double mean_energy = (double)pl.energy / 100000.0;
  T_CHECK(mean_energy < 1.0);          /* bounded mean kinetic energy */
  T_CHECK(pl.steps == 100000u);
}

/* ----------------------------------------------------------- friction */

static void test_friction_holds_the_rack(void) {
  t_section("M30 dry friction holds the rack against a weak drive");
  relais_phys_params phys;
  relais_phys_params_default(&phys);
  phys.friction_static = 20.0f;
  phys.friction_kinetic = 12.0f;
  phys.friction_band = 0.02f;
  relais_plant_ext pl;
  relais_plant_ext_init(&pl, 0.0f, &phys);
  /* A command too weak to break the rack away: it must not move. */
  for (int i = 0; i < 200; i++) relais_plant_ext_step(&pl, 0.001f, 0.0f, 0.005f);
  T_CHECK(fabsf(pl.pos) < 1e-6f);
  T_CHECK(fabsf(pl.vel) < 1e-6f);
  /* A strong command breaks the breakaway force: the rack moves. */
  for (int i = 0; i < 400; i++) relais_plant_ext_step(&pl, 1.0f, 0.0f, 0.005f);
  T_CHECK(pl.pos > 0.01f);
  /* The steady velocity is lower than without friction. */
  relais_plant_ext free_plant;
  relais_plant_ext_init(&free_plant, 0.0f, NULL);
  for (int i = 0; i < 400; i++) relais_plant_ext_step(&free_plant, 1.0f, 0.0f, 0.005f);
  T_CHECK(pl.vel < free_plant.vel);
}

/* --------------------------------------------------------------- play */

static void test_play_delays_the_motion(void) {
  t_section("M30 the play delays the load, bounds the lash and remembers the side");
  relais_phys_params phys;
  relais_phys_params_default(&phys);
  phys.backlash = 0.02f;
  relais_plant_ext pl;
  relais_plant_ext_init(&pl, 0.0f, &phys);
  /* A slow command: the motor takes the play up before the load moves. */
  int still = 0;
  for (int i = 0; i < 40; i++) {
    relais_plant_ext_step(&pl, 0.05f, 0.0f, 0.005f);
    if (fabsf(pl.pos) < 1e-9f) still++;
  }
  T_CHECK(still >= 3);           /* the dead zone is visible */
  T_CHECK(pl.contact == 1.0f);   /* engaged on the forward side */
  /* The relative travel never leaves the play: rigid tooth pair. */
  double max_gap = 0.0;
  for (int i = 0; i < 4000; i++) {
    relais_plant_ext_step(&pl, 1.0f, 0.0f, 0.005f);
    double gap = fabs((double)(pl.m_pos - pl.pos));
    if (gap > max_gap) max_gap = gap;
  }
  /* The tooth pair is rigid: the relative travel never leaves half the play.
   * The tolerance covers the float rounding of positions of order 80. */
  T_CHECK(max_gap <= (double)phys.backlash * 0.5 + 1e-4);
  T_CHECK(fabsf(pl.vel - pl.m_vel) < 1e-4f); /* the motion is transmitted */
  /* Reverse: the load keeps moving forward while the motor crosses the play,
   * which is the contact-side memory of the model. */
  float before = pl.pos;
  int coast = 0;
  for (int i = 0; i < 40; i++) {
    relais_plant_ext_step(&pl, -1.0f, 0.0f, 0.005f);
    if (pl.pos > before) coast++;
  }
  T_CHECK(coast >= 2);
  T_CHECK(pl.contact == -1.0f);  /* engaged on the other side afterwards */
  for (int i = 0; i < 4000; i++) relais_plant_ext_step(&pl, -1.0f, 0.0f, 0.005f);
  T_CHECK(pl.m_pos - pl.pos <= 0.0f);
  T_CHECK(pl.m_pos - pl.pos >= -phys.backlash * 0.5f - 1e-4f);
}

/* ---------------------------------------------------- current limit */

static void test_current_limit_caps_the_acceleration(void) {
  t_section("M30 the motor current limit caps the velocity increment");
  relais_phys_params phys;
  relais_phys_params_default(&phys);
  phys.current_limit = 50.0f; /* position/s^2, well below the nominal drive */
  relais_plant_ext pl;
  relais_plant_ext_init(&pl, 0.0f, &phys);
  float previous = 0.0f;
  long over = 0;
  for (int i = 0; i < 2000; i++) {
    relais_plant_ext_step(&pl, 1.0f, 0.0f, 0.005f);
    float increment = fabsf(pl.vel - previous);
    if (increment > phys.current_limit * 0.005f + 1e-6f) over++;
    previous = pl.vel;
  }
  T_EQ_U((uint64_t)over, 0u);
  /* Without the limit the same command reaches the steady velocity faster. */
  relais_plant_ext free_plant;
  relais_plant_ext_init(&free_plant, 0.0f, NULL);
  for (int i = 0; i < 20; i++) relais_plant_ext_step(&free_plant, 1.0f, 0.0f, 0.005f);
  relais_plant_ext limited;
  relais_plant_ext_init(&limited, 0.0f, &phys);
  for (int i = 0; i < 20; i++) relais_plant_ext_step(&limited, 1.0f, 0.0f, 0.005f);
  T_CHECK(limited.vel < free_plant.vel);
}

/* ---------------------------------------------------------- stiffness */

static void test_stiffness_centres_the_rack(void) {
  t_section("M30 the column stiffness pulls the rack back to the centre");
  relais_phys_params phys;
  relais_phys_params_default(&phys);
  phys.stiffness = 50.0f;
  relais_plant_ext pl;
  relais_plant_ext_init(&pl, 0.3f, &phys);
  /* The force is restoring: the rack moves towards the centre. */
  relais_plant_ext_step(&pl, 0.0f, 0.0f, 0.005f);
  T_CHECK(pl.vel < 0.0f);
  /* Without damping it oscillates around the centre and stays bounded by the
   * initial excursion (energy conservation of the undamped oscillator). */
  float extreme = 0.0f;
  double sum = 0.0;
  for (int i = 0; i < 4000; i++) {
    relais_plant_ext_step(&pl, 0.0f, 0.0f, 0.005f);
    if (fabsf(pl.pos) > extreme) extreme = fabsf(pl.pos);
    sum += (double)pl.pos;
  }
  T_CHECK(extreme <= 0.3f + 1e-3f);
  T_CHECK(fabs(sum / 4000.0) < 0.05); /* the mean position is the centre */
  /* With a viscous friction the oscillation is damped and the rack settles. */
  relais_phys_params damped;
  relais_phys_params_default(&damped);
  damped.stiffness = 50.0f;
  damped.friction_viscous = 4.0f;
  relais_plant_ext d;
  relais_plant_ext_init(&d, 0.3f, &damped);
  for (int i = 0; i < 4000; i++) relais_plant_ext_step(&d, 0.0f, 0.0f, 0.005f);
  T_CHECK(fabsf(d.pos) < 0.01f);
  /* Without the stiffness the rack stays where it was left. */
  relais_plant_ext free_plant;
  relais_plant_ext_init(&free_plant, 0.3f, NULL);
  for (int i = 0; i < 4000; i++) relais_plant_ext_step(&free_plant, 0.0f, 0.0f, 0.005f);
  T_CHECK(fabsf(free_plant.pos - 0.3f) < 1e-6f);
}

/* --------------------------------------- the gap between the two models */

static void test_gap_between_the_models(void) {
  t_section("M30 the tracking gap of the faithful model, published");
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  relais_plant plain;
  relais_ctl_state ctl_plain;
  loop_stats st_plain;
  stats_reset(&st_plain);
  loop_run(&ctl_plain, &plain, &tr, &p, 4000, 1.0, &st_plain);
  relais_phys_params phys;
  relais_phys_params_demo(&phys);
  relais_plant_ext faithful;
  relais_plant_ext_init(&faithful, 0.0f, &phys);
  relais_ctl_state ctl_faithful;
  loop_stats st_faithful;
  stats_reset(&st_faithful);
  loop_run_ext(&ctl_faithful, &faithful, &tr, &p, 4000, 1.0, &st_faithful);
  T_CHECK(st_plain.finite && st_faithful.finite);
  /* The published model tracks the slalom inside 10 % of the amplitude; the
   * faithful one is allowed to be worse, but must stay bounded and of the
   * same order (the number is published in the mission report). */
  T_CHECK(st_plain.max_err < 0.1f * tr.amp);
  T_CHECK(st_faithful.max_err < 0.5f * tr.amp);
  T_CHECK(st_faithful.max_pos < 2.0f);
  printf("  gap: published max_err=%.6f faithful max_err=%.6f ratio=%.2f\n",
         (double)st_plain.max_err, (double)st_faithful.max_err,
         (double)(st_faithful.max_err / (st_plain.max_err > 0.0f ? st_plain.max_err : 1e-9f)));
}

int main(void) {
  test_zeroed_model_is_the_published_one();
  test_each_phenomenon_is_independent();
  test_faithful_model_is_stable();
  test_friction_holds_the_rack();
  test_play_delays_the_motion();
  test_current_limit_caps_the_acceleration();
  test_stiffness_centres_the_rack();
  test_gap_between_the_models();
  return t_summary();
}
