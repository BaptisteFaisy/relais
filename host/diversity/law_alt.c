/* Relais — alternative control laws for machine B (mission M37, DEG-08, CMD-09).
 * See law_alt.h for why these exist: they are DESIGN diversity, not a second
 * compilation of the same source.
 */
#include "law_alt.h"

#include <string.h>

/* Tuned by hand on the rack model of core/src/plant.c (steady rack velocity is
 * 4 x command), independently of the built-in PID: a proportional loop of 22
 * leaves about 2 % of full scale of tracking error on a 0.5 Hz slalom, and the
 * first order reference model of 60 ms keeps the command smooth without the
 * filtered derivative of the built-in law. */
#define ALT_KP 60.0f
#define ALT_KD 0.50f
#define ALT_TAU 0.02f
/* Ramp follower: 0.25 is the inverse of the plant velocity gain, so the feed
 * forward term asks for exactly the command that holds the requested slope. */
#define ALT_FF 0.25f
#define ALT_FB 8.0f
/* Bounded slope of the request: the reference trajectories never ask for more
 * than about 2 full scale per second, so 4 leaves room for a step onset without
 * letting the very first tick saturate the feed forward term. */
#define ALT_RATE_LIMIT 4.0f
#define ALT_VERSION 0xA1u
#define ALT_STATE_LEN 20u /* version, length, kind, pad, 3 floats, tick count */

law_alt_kind law_alt_parse(const char *name) {
  if (name == NULL) return LAW_ALT_NONE;
  if (strcmp(name, "first-order") == 0 || strcmp(name, "first_order") == 0)
    return LAW_ALT_FIRST_ORDER;
  if (strcmp(name, "ramp") == 0) return LAW_ALT_RAMP;
  return LAW_ALT_NONE;
}

const char *law_alt_name(law_alt_kind kind) {
  switch (kind) {
    case LAW_ALT_FIRST_ORDER: return "first-order";
    case LAW_ALT_RAMP: return "ramp";
    default: return "none";
  }
}

static void alt_init(void *ctx, const relais_params *p) {
  law_alt_state *s = (law_alt_state *)ctx;
  if (s == NULL) return;
  law_alt_kind keep = s->kind;
  memset(s, 0, sizeof *s);
  s->kind = keep;
  (void)p;
}

/* First order compensator: the command relaxes towards a proportional and
 * derivative target with the time constant ALT_TAU. There is no integrator, no
 * anti-windup back calculation and no filtered measurement, so a ramp is
 * followed with a permanent offset and a phase lag the built-in PID does not
 * have. That difference is what the order/shadow comparison measures. */
static float alt_step_first_order(law_alt_state *s, const relais_meas *m,
                                  const relais_params *p) {
  float dt = (float)relais_params_step_s(p);
  if (!(dt > 0.0f)) return s->last;
  float target = ALT_KP * (m->setpoint - m->pos) - ALT_KD * m->vel;
  s->rate = target;
  s->ref += (target - s->ref) * (dt / ALT_TAU);
  s->last = relais_clamp_slew(s->last, relais_clamp_value(s->ref, p), p);
  return s->last;
}

/* Ramp follower: the slope of the request is measured over one period, bounded,
 * and fed forward; the position feedback is weak and there is no filter. */
static float alt_step_ramp(law_alt_state *s, const relais_meas *m,
                           const relais_params *p) {
  float dt = (float)relais_params_step_s(p);
  if (!(dt > 0.0f)) return s->last;
  float slope = (m->setpoint - s->ref) / dt;
  if (slope > ALT_RATE_LIMIT) slope = ALT_RATE_LIMIT;
  if (slope < -ALT_RATE_LIMIT) slope = -ALT_RATE_LIMIT;
  s->ref = m->setpoint;
  s->rate = slope;
  float wanted = ALT_FF * slope + ALT_FB * (m->setpoint - m->pos);
  s->last = relais_clamp_slew(s->last, relais_clamp_value(wanted, p), p);
  return s->last;
}

static float alt_step(void *ctx, const relais_meas *m, const relais_params *p) {
  law_alt_state *s = (law_alt_state *)ctx;
  if (s == NULL || m == NULL || p == NULL) return 0.0f;
  s->ticks++;
  return s->kind == LAW_ALT_RAMP ? alt_step_ramp(s, m, p)
                                : alt_step_first_order(s, m, p);
}

static uint8_t alt_export(void *ctx, uint8_t *buf, size_t cap) {
  const law_alt_state *s = (const law_alt_state *)ctx;
  if (s == NULL || buf == NULL || cap < ALT_STATE_LEN) return 0;
  memset(buf, 0, ALT_STATE_LEN);
  buf[0] = ALT_VERSION;
  buf[1] = ALT_STATE_LEN;
  buf[2] = (uint8_t)s->kind;
  relais_frame_write_f32(buf + 4, s->ref);
  relais_frame_write_f32(buf + 8, s->last);
  relais_frame_write_f32(buf + 12, s->rate);
  relais_frame_write_u32(buf + 16, s->ticks);
  return ALT_STATE_LEN;
}

/* REP-02: a state of another version, of another length or of another law is
 * refused, so the second keeps its own state instead of trusting a replica it
 * cannot interpret — which is also why an alternative law does not depend on
 * the state replication for its own correctness. */
static bool alt_import(void *ctx, const uint8_t *buf, size_t len) {
  law_alt_state *s = (law_alt_state *)ctx;
  if (s == NULL || buf == NULL) return false;
  if (len != ALT_STATE_LEN || buf[0] != ALT_VERSION || buf[1] != ALT_STATE_LEN)
    return false;
  if (buf[2] != (uint8_t)s->kind) return false;
  s->ref = relais_frame_read_f32(buf + 4);
  s->last = relais_frame_read_f32(buf + 8);
  s->rate = relais_frame_read_f32(buf + 12);
  s->ticks = relais_frame_read_u32(buf + 16);
  return true;
}

void law_alt_hooks(law_alt_state *ctx, relais_law *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof *out);
  if (ctx == NULL || ctx->kind == LAW_ALT_NONE) return;
  out->ctx = ctx;
  out->init = alt_init;
  out->step = alt_step;
  out->export_state = alt_export;
  out->import_state = alt_import;
}
