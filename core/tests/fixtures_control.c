/* Relais — shared fixtures, see fixtures_control.h. */
#include "fixtures_control.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- fixtures */

void stats_reset(loop_stats *st) { memset(st, 0, sizeof *st); st->finite = true; }

/* Close the position loop over the rack for `ticks` periods of `p`, following
 * `tr`, and report the extremes. The same driver serves the tracking, REP-04
 * and REL-03 tests: one implementation, three uses. */
float loop_run(relais_ctl_state *s, relais_plant *pl, const relais_traj *tr,
                      const relais_params *p, int ticks, double settle_s, loop_stats *st) {
  const float dt = (float)relais_params_period_s(p);
  relais_ctl_init(s, p);
  relais_plant_init(pl, 0.0f, 0u);
  float previous = 0.0f;
  for (int i = 0; i < ticks; i++) {
    const double t = (double)i * (double)dt;
    relais_meas m;
    m.pos = pl->pos;
    m.vel = pl->vel;
    m.applied = previous;
    m.setpoint = relais_traj_setpoint(tr, t + (double)dt);
    float v = relais_ctl_step(s, &m, p);
    relais_plant_step(pl, v, 0.0f, dt);
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

/* A measurement set, built in one place (four values, no repeated literals). */
void meas_set(relais_meas *m, float pos, float vel, float applied, float setpoint) {
  m->pos = pos;
  m->vel = vel;
  m->applied = applied;
  m->setpoint = setpoint;
}

/* A random measurement in the documented domain, identical on both instances. */
void meas_random(relais_meas *m, t_rng *rng, float applied) {
  m->pos = (float)((int)t_rng_below(rng, 2001u) - 1000) / 1000.0f;
  m->vel = (float)((int)t_rng_below(rng, 401u) - 200) / 100.0f;
  m->applied = applied;
  m->setpoint = (float)((int)t_rng_below(rng, 2001u) - 1000) / 1000.0f;
}

