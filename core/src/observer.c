/* Relais — the state observer of the rack (OBS-02, REP-04, mission M30).
 *
 * A Luenberger observer of the published rack model: the model predicts the
 * position and the velocity from the command that was applied, the measurement
 * corrects them. When the measurement is absent the prediction runs alone, so
 * a sensor outage of a few ticks does not blind the computer.
 *
 * Pure C, no allocation, no clock (spec 2.2). The estimate is a pure function
 * of the inputs, so two computers fed the same sequence produce the same bits
 * (CMD-03), which is what the replication of the law requires.
 */
#include "relais/observer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "relais/plant_ext.h" /* the published rack constants, one set only */

/* Bounds of the estimate: the observer may never hand a non finite or absurd
 * value to the control law (spec 7.4). */
#define OBS_VEL_LIMIT (10.0f * RELAIS_PLANT_GAIN)
#define OBS_POS_LIMIT 1000.0f

void relais_obs_params_default(relais_obs_params *p) {
  if (p == NULL) return;
  memset(p, 0, sizeof *p);
  p->alpha = 0.3f;              /* position: the estimate keeps 42 % of the noise */
  p->beta = 2.0f;               /* velocity correction, 1/s */
  p->tau = RELAIS_PLANT_TAU;    /* the rack of the published model */
  p->gain = RELAIS_PLANT_GAIN;
}

void relais_observer_init(relais_observer *o, const relais_obs_params *p, float pos0, float vel0) {
  if (o == NULL) return;
  memset(o, 0, sizeof *o);
  if (p != NULL) o->params = *p;
  else relais_obs_params_default(&o->params);
  if (!(o->params.tau > 0.0f)) o->params.tau = RELAIS_PLANT_TAU;
  o->pos = pos0;
  o->vel = vel0;
}

static float clamp_finite(float v, float limit, float fallback) {
  if (!isfinite(v)) return fallback;
  if (v > limit) return limit;
  if (v < -limit) return -limit;
  return v;
}

void relais_observer_step(relais_observer *o, float applied, float meas_pos, bool meas_valid,
                          float dt) {
  if (o == NULL || !(dt > 0.0f)) return;
  const relais_obs_params *p = &o->params;
  /* Prediction on the published rack model: exact integration of the first
   * order velocity loop, then one Euler step of the position. */
  float alpha_m = 1.0f - expf(-dt / p->tau);
  float command = isfinite(applied) ? applied : 0.0f;
  float vel_pred = o->vel + (p->gain * command - o->vel) * alpha_m;
  vel_pred = clamp_finite(vel_pred, OBS_VEL_LIMIT, 0.0f);
  float pos_pred = o->pos + vel_pred * dt;
  pos_pred = clamp_finite(pos_pred, OBS_POS_LIMIT, o->pos);
  if (!meas_valid || !isfinite(meas_pos)) {
    /* Blind: the model carries the estimate alone. */
    o->pos = pos_pred;
    o->vel = vel_pred;
    o->blind++;
    o->steps++;
    return;
  }
  float innovation = meas_pos - pos_pred;
  o->pos = clamp_finite(pos_pred + p->alpha * innovation, OBS_POS_LIMIT, pos_pred);
  o->vel = clamp_finite(vel_pred + p->beta * innovation, OBS_VEL_LIMIT, vel_pred);
  o->blind = 0;
  o->steps++;
}

void relais_observer_meas(const relais_observer *o, const relais_meas *in, relais_meas *out) {
  if (out == NULL) return;
  if (in != NULL) *out = *in;
  else memset(out, 0, sizeof *out);
  if (o == NULL) return;
  out->pos = o->pos;
  out->vel = o->vel;
}

void relais_ctl_reconstruct_observer(relais_ctl_state *s, const relais_observer *o,
                                     const relais_meas *in, const relais_params *p) {
  relais_meas filtered;
  relais_observer_meas(o, in, &filtered);
  relais_ctl_reconstruct(s, &filtered, p); /* REP-04, on the estimate */
}

int relais_observer_status_text(const relais_observer *o, char *buf, size_t cap) {
  if (o == NULL || buf == NULL || cap == 0) return -1;
  return snprintf(buf, cap, "observer pos=%.6f vel=%.6f steps=%u blind=%u",
                  (double)o->pos, (double)o->vel, (unsigned)o->steps, (unsigned)o->blind);
}
