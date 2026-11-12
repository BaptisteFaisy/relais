/* Relais — the adaptive control law (CMD-03, CMD-09, REP-01, mission M30).
 *
 * The published law with gains scheduled on the measured speed, the change of
 * gain being rate limited. With flat gains the law is the published law, bit
 * for bit (the test proves it on 10 000 steps).
 *
 * Pure C, no allocation, no clock (spec 2.2).
 */
#include "relais/adaptive.h"

#include <math.h>
#include <string.h>

#include "relais/frame.h"

/* Replication format of the state (REP-01): version, length, ten floats and
 * two counters. Fifty bytes, well under the 256 bytes of a full state. */
#define ADAPTIVE_VERSION 1u
#define ADAPTIVE_FLOATS 10u
#define ADAPTIVE_LEN (2u + ADAPTIVE_FLOATS * 4u + 8u)

void relais_adaptive_params_default(relais_adaptive_params *p) {
  if (p == NULL) return;
  memset(p, 0, sizeof *p);
  /* The tuning of the published law (core/src/control.c): flat gains, so the
   * two laws compute the same bits. */
  p->kp_low = 60.0f;
  p->kp_high = 60.0f;
  p->kd_low = 0.5f;
  p->kd_high = 0.5f;
  p->ki = 4.0f;
  p->vel_ref = 1.0f;
  p->gain_slew = 1000.0f; /* large enough not to interfere with the flat case */
  p->integ_limit = 0.25f;
  p->rate_limit = 20.0f;
  p->filt = 0.35f;
}

void relais_adaptive_params_demo(relais_adaptive_params *p) {
  if (p == NULL) return;
  relais_adaptive_params_default(p);
  p->kp_low = 45.0f;
  p->kp_high = 75.0f;
  p->kd_low = 0.35f;
  p->kd_high = 0.65f;
  p->vel_ref = 2.0f;
  p->gain_slew = 5.0f; /* at most five units of gain per tick */
}

void relais_adaptive_init(relais_adaptive *a, const relais_adaptive_params *p) {
  if (a == NULL) return;
  memset(a, 0, sizeof *a);
  if (p != NULL) a->params = *p;
  else relais_adaptive_params_default(&a->params);
  a->state.kp = a->params.kp_low;
  a->state.kd = a->params.kd_low;
}

/* The scheduling: the gains follow the speed, but never faster than the
 * robustness guard allows, and never outside the announced range. */
static void schedule(relais_adaptive_state *s, const relais_adaptive_params *p, float vel) {
  float speed = fabsf(vel);
  float ratio = p->vel_ref > 0.0f ? speed / p->vel_ref : 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  float kp_target = p->kp_low + (p->kp_high - p->kp_low) * ratio;
  float kd_target = p->kd_low + (p->kd_high - p->kd_low) * ratio;
  float dk = kp_target - s->kp;
  float dd = kd_target - s->kd;
  float bound = p->gain_slew;
  if (dk > bound) dk = bound;
  if (dk < -bound) dk = -bound;
  if (dd > bound) dd = bound;
  if (dd < -bound) dd = -bound;
  if (dk != 0.0f || dd != 0.0f) s->changes++;
  s->kp += dk;
  s->kd += dd;
}

float relais_adaptive_step(relais_adaptive *a, const relais_meas *m, const relais_params *p) {
  if (a == NULL || m == NULL || p == NULL) return 0.0f;
  const relais_adaptive_params *ap = &a->params;
  relais_adaptive_state *s = &a->state;
  float dt = (float)relais_params_step_s(p);
  /* The slope of the steering request, measured over one period and bounded,
   * is what lets the will follow the manoeuvre instead of freezing it. */
  float rate = (m->setpoint - s->prev_setpoint) / dt;
  if (rate > ap->rate_limit) rate = ap->rate_limit;
  if (rate < -ap->rate_limit) rate = -ap->rate_limit;
  s->setpoint_rate += ap->filt * (rate - s->setpoint_rate);
  s->prev_setpoint = m->setpoint;
  float error = m->setpoint - m->pos;
  s->filt += ap->filt * (m->pos - s->filt);
  s->integ += error * dt;
  if (s->integ > ap->integ_limit) s->integ = ap->integ_limit;
  if (s->integ < -ap->integ_limit) s->integ = -ap->integ_limit;
  schedule(s, ap, m->vel);
  float wanted = s->kp * error - s->kd * m->vel + ap->ki * s->integ;
  float bounded = relais_clamp_value(wanted, p);
  if (bounded != wanted) s->integ -= error * dt; /* anti-windup by back-calculation */
  float value = relais_clamp_slew(s->last_value, bounded, p);
  s->last_value = value;
  s->last_pos = m->pos;
  s->phase += dt;
  s->fb_value = value;
  s->ticks++;
  return value;
}

uint8_t relais_adaptive_export(const relais_adaptive *a, uint8_t *buf, size_t cap) {
  if (a == NULL || buf == NULL || cap < ADAPTIVE_LEN) return 0;
  memset(buf, 0, ADAPTIVE_LEN);
  buf[0] = (uint8_t)ADAPTIVE_VERSION;
  buf[1] = (uint8_t)ADAPTIVE_LEN;
  size_t at = 2;
  const float fields[ADAPTIVE_FLOATS] = {
      a->state.integ,     a->state.filt,      a->state.last_pos, a->state.last_value,
      a->state.phase,     a->state.fb_value,  a->state.prev_setpoint,
      a->state.setpoint_rate, a->state.kp,    a->state.kd};
  for (unsigned i = 0; i < ADAPTIVE_FLOATS; i++) at += relais_frame_write_f32(buf + at, fields[i]);
  at += relais_frame_write_u32(buf + at, a->state.ticks);
  at += relais_frame_write_u32(buf + at, a->state.changes);
  return (uint8_t)ADAPTIVE_LEN;
}

bool relais_adaptive_import(relais_adaptive *a, const uint8_t *buf, size_t len) {
  if (a == NULL || buf == NULL) return false;
  if (len != ADAPTIVE_LEN || buf[0] != (uint8_t)ADAPTIVE_VERSION || buf[1] != (uint8_t)ADAPTIVE_LEN)
    return false;
  size_t at = 2;
  float fields[ADAPTIVE_FLOATS];
  for (unsigned i = 0; i < ADAPTIVE_FLOATS; i++) {
    fields[i] = relais_frame_read_f32(buf + at);
    at += 4;
  }
  a->state.integ = fields[0];
  a->state.filt = fields[1];
  a->state.last_pos = fields[2];
  a->state.last_value = fields[3];
  a->state.phase = fields[4];
  a->state.fb_value = fields[5];
  a->state.prev_setpoint = fields[6];
  a->state.setpoint_rate = fields[7];
  a->state.kp = fields[8];
  a->state.kd = fields[9];
  a->state.ticks = relais_frame_read_u32(buf + at);
  a->state.changes = relais_frame_read_u32(buf + at + 4);
  return true;
}

size_t relais_adaptive_state_size(void) { return (size_t)ADAPTIVE_LEN; }

/* ---- the CMD-09 vtable ---- */

static void adaptive_law_init(void *ctx, const relais_params *p) {
  (void)p;
  relais_adaptive *a = (relais_adaptive *)ctx;
  if (a == NULL) return;
  float kp = a->params.kp_low;
  float kd = a->params.kd_low;
  memset(&a->state, 0, sizeof a->state);
  a->state.kp = kp;
  a->state.kd = kd;
}

static float adaptive_law_step(void *ctx, const relais_meas *m, const relais_params *p) {
  return relais_adaptive_step((relais_adaptive *)ctx, m, p);
}

static uint8_t adaptive_law_export(void *ctx, uint8_t *buf, size_t cap) {
  return relais_adaptive_export((const relais_adaptive *)ctx, buf, cap);
}

static bool adaptive_law_import(void *ctx, const uint8_t *buf, size_t len) {
  return relais_adaptive_import((relais_adaptive *)ctx, buf, len);
}

void relais_adaptive_law(relais_law *law, relais_adaptive *a) {
  if (law == NULL) return;
  law->ctx = a;
  law->init = adaptive_law_init;
  law->step = adaptive_law_step;
  law->export_state = adaptive_law_export;
  law->import_state = adaptive_law_import;
}
