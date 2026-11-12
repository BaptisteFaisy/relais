/* Relais — a state observer for the rack (OBS-02, REP-04, mission M30).
 *
 * The two computers are driven by the measurements the actuator publishes.
 * When a computer has to rebuild its state without the replica (REP-04), or
 * when the position sensor is noisy or momentarily absent, the published
 * rebuild takes the last applied value and the raw measurement, which makes
 * the first order of the successor depend on the noise of one sample.
 *
 * This module estimates the position and the velocity from the command that
 * was applied and from a measurement that may be noisy OR ABSENT. It is a
 * Luenberger observer of the published rack model: the model predicts, the
 * measurement corrects. It is pure C, no allocation, no clock (spec 2.2).
 *
 *   prediction:  v+ = v + (gain*u - v)*(1 - exp(-dt/tau))   (the rack model)
 *                p+ = p + v+*dt
 *   correction:  p  = p+ + alpha*(meas - p+)
 *                v  = v+ + beta *(meas - p+)
 *
 * With no measurement the observer runs open loop on the model, which is why
 * it can bridge a sensor outage of a few ticks (DEG-04 spirit).
 *
 * Spec: OBS-02 (state as text), REP-04 (rebuild without the replica),
 *       CMD-03 (the estimate is a pure function of its inputs).
 */
#ifndef RELAIS_OBSERVER_H
#define RELAIS_OBSERVER_H

#include "relais/control.h"

typedef struct {
  float alpha; /* position correction gain, dimensionless, 0..1 */
  float beta;  /* velocity correction gain, 1/s */
  float tau;   /* time constant of the model used for the prediction, s */
  float gain;  /* command to steady velocity gain of the model */
} relais_obs_params;

typedef struct {
  relais_obs_params params;
  float pos;      /* estimated position */
  float vel;      /* estimated velocity */
  uint32_t steps; /* steps run */
  uint32_t blind; /* consecutive steps without a usable measurement */
} relais_observer;

void relais_obs_params_default(relais_obs_params *p);
void relais_observer_init(relais_observer *o, const relais_obs_params *p, float pos0, float vel0);

/* One step. `applied` is the command the actuator applied during the period,
 * `meas_pos` the measurement of the position, `meas_valid` false when the
 * sensor gave nothing usable this period. */
void relais_observer_step(relais_observer *o, float applied, float meas_pos, bool meas_valid,
                          float dt);

/* The estimate as a measurement the control law can consume: the position and
 * the velocity of the observer replace the ones of `in`, the applied value and
 * the setpoint are kept (they are not what the observer estimates). */
void relais_observer_meas(const relais_observer *o, const relais_meas *in, relais_meas *out);

/* REP-04 with the observer: the same rebuild as relais_ctl_reconstruct(), but
 * on the filtered estimate instead of the raw sample. */
void relais_ctl_reconstruct_observer(relais_ctl_state *s, const relais_observer *o,
                                     const relais_meas *in, const relais_params *p);

/* Diagnostic text, OBS-02 style. Returns the number of bytes written. */
int relais_observer_status_text(const relais_observer *o, char *buf, size_t cap);

#endif /* RELAIS_OBSERVER_H */
