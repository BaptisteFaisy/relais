/* Relais — control law, testament and fallback loop (spec 4.2, 4.4, 4.8, 5.1).
 *
 * The law is a pure function of its state and of the measurements published by
 * the actuator: same binary, same inputs, same bits on both machines (CMD-03),
 * which is what lets the actuator compare an order with a shadow (DET-09).
 *
 * The state is small enough to be replicated whole every tick (REP-01) and the
 * testament is a model-based prediction, so the successor that takes over from
 * a replicated state computes what the holder would have computed (REL-03).
 */
#include "relais/control.h"

#include <math.h>
#include <string.h>

#include "relais/frame.h"

/* Tuning of the position loop, closed on the rack model of plant.c. The
 * velocity loop of the actuator is fast (20 ms), the position loop is tuned
 * for ~5 % tracking error on a 0.5 Hz slalom and a 0.1 s settling time. */
#define CTL_KP 60.0f
#define CTL_KD 0.5f
#define CTL_KI 4.0f
#define CTL_FILTER 0.35f
#define CTL_INTEG_LIMIT 0.25f
#define CTL_RATE_LIMIT 20.0f /* full scale per second, bounded extrapolation */
#define CTL_STATE_VERSION 1u
#define CTL_FLOAT_COUNT 8u
#define CTL_STATE_LEN (2u + CTL_FLOAT_COUNT * 4u + 4u) /* 30 bytes, well under 256 */
#define FALLBACK_DAMP 0.5f

float relais_clamp_value(float v, const relais_params *p) {
  if (p == NULL) return v;
  if (!(v > -p->max_amplitude)) return -p->max_amplitude; /* also catches NaN */
  if (v > p->max_amplitude) return p->max_amplitude;
  return v;
}

float relais_clamp_slew(float previous, float wanted, const relais_params *p) {
  if (p == NULL) return wanted;
  float delta = wanted - previous;
  if (delta > p->max_slew) return previous + p->max_slew;
  if (delta < -p->max_slew) return previous - p->max_slew;
  return wanted;
}

void relais_ctl_init(relais_ctl_state *s, const relais_params *p) {
  if (s == NULL) return;
  memset(s, 0, sizeof *s);
  (void)p;
}

/* CMD-02: one period of control, from the measurements of STATUT(n) to the
 * command for tick n+1. CMD-06: bounded in amplitude and in rate of change. */
float relais_ctl_step(relais_ctl_state *s, const relais_meas *m, const relais_params *p) {
  if (s == NULL || m == NULL || p == NULL) return 0.0f;
  float dt = (float)relais_params_period_s(p);
  /* The slope of the steering request, measured over one period and bounded,
   * is what lets the will follow the manoeuvre instead of freezing it. */
  float rate = (m->setpoint - s->prev_setpoint) / dt;
  if (rate > CTL_RATE_LIMIT) rate = CTL_RATE_LIMIT;
  if (rate < -CTL_RATE_LIMIT) rate = -CTL_RATE_LIMIT;
  s->setpoint_rate += CTL_FILTER * (rate - s->setpoint_rate);
  s->prev_setpoint = m->setpoint;
  float error = m->setpoint - m->pos;
  s->filt += CTL_FILTER * (m->pos - s->filt);
  s->integ += error * dt;
  if (s->integ > CTL_INTEG_LIMIT) s->integ = CTL_INTEG_LIMIT;
  if (s->integ < -CTL_INTEG_LIMIT) s->integ = -CTL_INTEG_LIMIT;

  float wanted = CTL_KP * error - CTL_KD * m->vel + CTL_KI * s->integ;
  float bounded = relais_clamp_value(wanted, p);
  if (bounded != wanted) s->integ -= error * dt; /* anti-windup by back-calculation */
  float value = relais_clamp_slew(s->last_value, bounded, p);

  s->last_value = value;
  s->last_pos = m->pos;
  s->phase += dt;
  s->fb_value = value;
  (void)s->filt;
  s->ticks++;
  return value;
}

uint8_t relais_ctl_export(const relais_ctl_state *s, uint8_t *buf, size_t cap) {
  if (s == NULL || buf == NULL || cap < CTL_STATE_LEN) return 0;
  memset(buf, 0, CTL_STATE_LEN);
  buf[0] = CTL_STATE_VERSION;
  buf[1] = (uint8_t)CTL_STATE_LEN;
  size_t at = 2;
  const float fields[CTL_FLOAT_COUNT] = {s->integ,   s->filt,       s->last_pos,
                                         s->last_value, s->phase,  s->fb_value,
                                         s->prev_setpoint, s->setpoint_rate};
  for (unsigned i = 0; i < CTL_FLOAT_COUNT; i++) at += relais_frame_write_f32(buf + at, fields[i]);
  relais_frame_write_u32(buf + at, s->ticks);
  return (uint8_t)CTL_STATE_LEN;
}

/* REP-02: a state of an unknown version or length is refused, the replica
 * keeps what it had rather than jumping to something it cannot trust. */
bool relais_ctl_import(relais_ctl_state *s, const uint8_t *buf, size_t len) {
  if (s == NULL || buf == NULL) return false;
  if (len != CTL_STATE_LEN || buf[0] != CTL_STATE_VERSION || buf[1] != CTL_STATE_LEN)
    return false;
  size_t at = 2;
  float fields[CTL_FLOAT_COUNT];
  for (unsigned i = 0; i < CTL_FLOAT_COUNT; i++) fields[i] = relais_frame_read_f32(buf + at), at += 4;
  s->integ = fields[0];
  s->filt = fields[1];
  s->last_pos = fields[2];
  s->last_value = fields[3];
  s->phase = fields[4];
  s->fb_value = fields[5];
  s->prev_setpoint = fields[6];
  s->setpoint_rate = fields[7];
  s->ticks = relais_frame_read_u32(buf + at);
  return true;
}

/* REP-04: with no ETAT any more, rebuild a state that continues the motion
 * already applied, so the takeover does not create a bump. */
void relais_ctl_reconstruct(relais_ctl_state *s, const relais_meas *m, const relais_params *p) {
  if (s == NULL || m == NULL) return;
  s->last_value = relais_clamp_value(m->applied, p);
  s->fb_value = s->last_value;
  s->filt = m->pos;
  s->last_pos = m->pos;
  s->integ = 0.0f;
}

/* ACT-02: the will of the last order. The prediction runs the same law on the
 * same rack model, from the current state, so the values stay close to what the
 * holder would have sent. */
void relais_testament_fill(const relais_ctl_state *s, const relais_meas *m,
                           const relais_params *p, float *out, uint8_t *len) {
  if (out == NULL || len == NULL) return;
  *len = 0;
  if (s == NULL || m == NULL || p == NULL) return;
  uint8_t n = p->testament_len > RELAIS_TESTAMENT_MAX ? RELAIS_TESTAMENT_MAX
                                                      : p->testament_len;
  relais_ctl_state copy = *s;
  relais_plant predicted;
  relais_plant_init(&predicted, m->pos, 0);
  predicted.vel = m->vel;
  float dt = (float)relais_params_period_s(p);
  float last = copy.last_value;
  for (uint8_t i = 0; i < n; i++) {
    /* The rack moves under the last command before the next one is computed,
     * exactly as it does between two ticks: the will is a prediction of what
     * the holder would have sent, not a replay of what it just sent. */
    relais_plant_step(&predicted, last, 0.0f, dt);
    relais_meas ahead;
    ahead.pos = predicted.pos;
    ahead.vel = predicted.vel;
    ahead.applied = last;
    /* ACT-02: the manoeuvre is extrapolated, so the curve shows no hole while
     * the handover happens (spec 4.2, idea 3). */
    ahead.setpoint = m->setpoint + copy.setpoint_rate * dt * (float)(i + 1);
    out[i] = relais_ctl_step(&copy, &ahead, p);
    last = out[i];
  }
  *len = n;
}

/* I6: nothing is played beyond the horizon of the will. */
bool relais_testament_at(const float *testament, uint8_t len, relais_tick base,
                         relais_tick tick, float *out) {
  if (testament == NULL || out == NULL || len == 0) return false;
  if (base == RELAIS_TICK_NONE || tick < base) return false;
  relais_tick offset = tick - base;
  if (offset >= (relais_tick)len) return false;
  *out = testament[(size_t)offset];
  return true;
}

void relais_fallback_init(float *fb_state, const relais_meas *m) {
  if (fb_state == NULL) return;
  *fb_state = m != NULL ? m->applied : 0.0f;
}

/* ACT-03, ACT-04: a local damped loop that holds the position, rate limited so
 * leaving the fallback towards a valid order cannot create a bump. */
float relais_fallback_step(float *fb_state, const relais_meas *m, const relais_params *p) {
  if (fb_state == NULL) return 0.0f;
  float velocity = m != NULL ? m->vel : 0.0f;
  float wanted = relais_clamp_value(-FALLBACK_DAMP * velocity, p);
  *fb_state = relais_clamp_slew(*fb_state, wanted, p);
  return *fb_state;
}
