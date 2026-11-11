/* Relais — the simulated rack and the reference trajectories (CMD-05, CMD-08).
 *
 * The rack is a second order model: the command drives a velocity, the road
 * pushes back. Both machines and the actuator share it, so the twin without
 * fault and the faulted run follow the very same reference (spec 4.8).
 */
#include "relais/control.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Actuator: velocity loop of 20 ms, steady rack velocity = PLANT_GAIN * u. */
#define PLANT_GAIN 4.0f
#define PLANT_DAMP 1.0f
#define PLANT_TAU 0.02f
#define TWO_PI 6.283185307179586f
#define SPEC_BUF 256

void relais_plant_init(relais_plant *pl, float pos0, uint32_t seed) {
  if (pl == NULL) return;
  memset(pl, 0, sizeof *pl);
  pl->pos = pos0;
  pl->vel = 0.0f;
  pl->t = 0.0f;
  pl->seed = seed;
}

/* One semi-implicit Euler step. Anything non finite is clamped, so a run can
 * never silently diverge into NaN and hide a violation (spec 7.4). */
void relais_plant_step(relais_plant *pl, float value, float disturbance, float dt) {
  if (pl == NULL || !(dt > 0.0f)) return;
  float acc = (PLANT_GAIN * value - PLANT_DAMP * pl->vel) / PLANT_TAU + disturbance;
  if (!isfinite(acc)) acc = 0.0f;
  pl->vel += acc * dt;
  if (!isfinite(pl->vel)) pl->vel = 0.0f;
  if (pl->vel > 10.0f * PLANT_GAIN) pl->vel = 10.0f * PLANT_GAIN;
  if (pl->vel < -10.0f * PLANT_GAIN) pl->vel = -10.0f * PLANT_GAIN;
  pl->pos += pl->vel * dt;
  if (!isfinite(pl->pos)) pl->pos = 0.0f;
  pl->t += dt;
  pl->disturbance = disturbance;
}

void relais_traj_default(relais_traj *t) {
  if (t == NULL) return;
  memset(t, 0, sizeof *t);
  t->kind = RELAIS_TRAJ_SLALOM;
  t->amp = 0.6f;
  t->freq = 0.5f;
  t->offset = 0.0f;
  t->t_start = 1.0f;
  t->dist_amp = 0.05f;
  t->dist_freq = 2.0f;
  t->seed = 7u;
}

static float smoothstep(float x) {
  if (x <= 0.0f) return 0.0f;
  if (x >= 1.0f) return 1.0f;
  return x * x * (3.0f - 2.0f * x);
}

/* Deterministic pseudo noise in [-1, 1], a pure function of the seed and of
 * the time: no state, so the twin and the faulted run agree bit for bit. */
static float unit_noise(uint32_t seed, double seconds) {
  uint32_t step = (uint32_t)(seconds * 1000.0);
  uint64_t mixed = (uint64_t)step * 0x9e3779b97f4a7c15ull ^ ((uint64_t)seed << 32);
  mixed ^= mixed >> 33;
  mixed *= 0xff51afd7ed558ccdull;
  mixed ^= mixed >> 33;
  return (float)((double)(mixed & 0xffffffull) / 8388607.5 - 1.0);
}

float relais_traj_setpoint(const relais_traj *t, double seconds) {
  if (t == NULL) return 0.0f;
  float time = (float)seconds;
  switch (t->kind) {
    case RELAIS_TRAJ_SLALOM:
      return t->offset + t->amp * sinf(TWO_PI * t->freq * time);
    case RELAIS_TRAJ_LANE_CHANGE: {
      float progress = (time - t->t_start) / 2.0f;
      return t->offset + t->amp * (2.0f * smoothstep(progress) - 1.0f);
    }
    case RELAIS_TRAJ_STEP:
      return t->offset + (time < t->t_start ? -t->amp : t->amp);
    case RELAIS_TRAJ_CONST:
      return t->offset;
    case RELAIS_TRAJ_BRAKE: {
      /* CMD-08: a braking profile, ramp then hold, always inside the bounds. */
      float elapsed = time - t->t_start;
      if (elapsed <= 0.0f) return t->offset;
      float ramp = elapsed < 1.0f ? elapsed : 1.0f;
      return t->offset - t->amp * ramp;
    }
  }
  return t->offset;
}

float relais_traj_disturbance(const relais_traj *t, double seconds) {
  if (t == NULL || !(t->dist_amp > 0.0f)) return 0.0f;
  float time = (float)seconds;
  float wave = sinf(TWO_PI * t->dist_freq * time);
  float noise = unit_noise(t->seed, seconds);
  return t->dist_amp * (0.6f * wave + 0.4f * noise);
}

static bool traj_key(relais_traj *t, const char *k, const char *v) {
  double d = strtod(v, NULL);
  unsigned long u = strtoul(v, NULL, 10);
  if (strcmp(k, "kind") == 0) {
    if (strcmp(v, "slalom") == 0) t->kind = RELAIS_TRAJ_SLALOM;
    else if (strcmp(v, "lane_change") == 0) t->kind = RELAIS_TRAJ_LANE_CHANGE;
    else if (strcmp(v, "step") == 0) t->kind = RELAIS_TRAJ_STEP;
    else if (strcmp(v, "const") == 0) t->kind = RELAIS_TRAJ_CONST;
    else if (strcmp(v, "brake") == 0) t->kind = RELAIS_TRAJ_BRAKE;
    else return false;
    return true;
  }
  if (strcmp(k, "amp") == 0) { t->amp = (float)d; return true; }
  if (strcmp(k, "freq") == 0) { t->freq = (float)d; return true; }
  if (strcmp(k, "offset") == 0) { t->offset = (float)d; return true; }
  if (strcmp(k, "t_start") == 0) { t->t_start = (float)d; return true; }
  if (strcmp(k, "dist_amp") == 0) { t->dist_amp = (float)d; return true; }
  if (strcmp(k, "dist_freq") == 0) { t->dist_freq = (float)d; return true; }
  if (strcmp(k, "seed") == 0) { t->seed = (uint32_t)u; return true; }
  return false;
}

bool relais_traj_parse(relais_traj *t, const char *body, char *why, size_t why_len) {
  char buf[SPEC_BUF];
  if (t == NULL) return false;
  if (body == NULL || body[0] == '\0') return true; /* keep the defaults */
  if (strlen(body) >= sizeof buf) {
    if (why != NULL && why_len > 0) snprintf(why, why_len, "trajectory too long");
    return false;
  }
  strcpy(buf, body);
  for (char *save = NULL, *tok = strtok_r(buf, " \t,", &save); tok != NULL;
       tok = strtok_r(NULL, " \t,", &save)) {
    char *eq = strchr(tok, '=');
    if (eq == NULL) {
      if (why != NULL && why_len > 0) snprintf(why, why_len, "missing '=' in %s", tok);
      return false;
    }
    *eq = '\0';
    if (!traj_key(t, tok, eq + 1)) {
      if (why != NULL && why_len > 0) snprintf(why, why_len, "unknown key: %s", tok);
      return false;
    }
  }
  if (why != NULL && why_len > 0) why[0] = '\0';
  return true;
}
