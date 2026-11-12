/* Relais — the faithful rack model (CMD-05, mission M30).
 *
 * One integration step of the rack with, optionally, dry friction (Stribeck),
 * mechanical play with a contact-side memory, motor current saturation and
 * the centring stiffness of the column. With every parameter at zero the step
 * is the published one, computed in the same order on the same values, so the
 * two models agree bit for bit (test_plant_ext.c proves it over 100 000
 * steps).
 *
 * The play is modelled as a rigid tooth pair: the relative travel of the load
 * against the motor is geometrically bounded by the play, which is what a
 * real rack does, and the coupling needs no tuning parameter.
 *
 * The model is pure: no clock, no file, no allocation (spec 2.2).
 */
#include "relais/plant_ext.h"

#include <math.h>
#include <string.h>

/* The velocity bound of the published model, kept identical (CMD-05). */
#define EXT_VEL_LIMIT (10.0f * RELAIS_PLANT_GAIN)
/* Stribeck speed scale used when the caller leaves it at zero. */
#define EXT_DEFAULT_STRIBECK_V 0.05f

void relais_phys_params_default(relais_phys_params *p) {
  if (p == NULL) return;
  memset(p, 0, sizeof *p);
}

void relais_phys_params_demo(relais_phys_params *p) {
  if (p == NULL) return;
  memset(p, 0, sizeof *p);
  /* Demonstration values, see the header for the provenance. */
  p->friction_static = 20.0f;    /* ~10 % of the nominal drive acceleration */
  p->friction_kinetic = 12.0f;
  p->friction_viscous = 2.0f;    /* at 4 position/s: 8 position/s^2 */
  p->friction_velocity = 0.5f;   /* Stribeck scale, position/s */
  p->friction_band = 0.02f;      /* stick band, position/s */
  p->backlash = 0.02f;           /* 2 % of the travel */
  /* 60 position/s^2 is about a third of the acceleration the velocity loop
   * asks for at full command: it does not bind in steady state, and it does
   * bind on a fast transient, which is what a current limit is for. */
  p->current_limit = 60.0f;
  p->stiffness = 50.0f;          /* ~5 % of the drive at 10 % of travel */
}

bool relais_phys_params_enabled(const relais_phys_params *p) {
  if (p == NULL) return false;
  return p->friction_static != 0.0f || p->friction_kinetic != 0.0f ||
         p->friction_viscous != 0.0f || p->backlash != 0.0f ||
         p->current_limit != 0.0f || p->stiffness != 0.0f;
}

void relais_plant_ext_init(relais_plant_ext *pl, float pos0, const relais_phys_params *p) {
  if (pl == NULL) return;
  memset(pl, 0, sizeof *pl);
  if (p != NULL) pl->params = *p;
  pl->pos = pos0;
  pl->m_pos = pos0;
}

/* True when the dry friction is armed (any of its three terms is non zero). */
static bool friction_armed(const relais_phys_params *p) {
  return p->friction_static != 0.0f || p->friction_kinetic != 0.0f ||
         p->friction_viscous != 0.0f;
}

/* Magnitude of the Stribeck curve at a given speed: the breakaway force decays
 * towards the kinetic force as the speed grows, and the viscous term adds a
 * part proportional to the speed. */
static float stribeck_magnitude(const relais_phys_params *p, float speed) {
  float scale = p->friction_velocity > 0.0f ? p->friction_velocity
                                            : EXT_DEFAULT_STRIBECK_V;
  float ratio = speed / scale;
  float decay = expf(-(ratio * ratio));
  float dry = p->friction_static + (p->friction_kinetic - p->friction_static) * decay;
  return dry + p->friction_viscous * speed;
}

/* The friction of one tick, as a velocity increment. In the stick band the
 * friction either balances the drive exactly (the rack holds) or lets it go
 * with the breakaway value; outside the band it opposes the motion with the
 * Stribeck magnitude. `stick` tells the caller that the rack did not move. */
static float friction_increment(const relais_phys_params *p, float vel, float drive_inc,
                                float dt, bool *stick) {
  *stick = false;
  if (!friction_armed(p)) return 0.0f;
  float speed = fabsf(vel);
  float band = p->friction_band;
  if (band > 0.0f && speed <= band) {
    if (fabsf(drive_inc) <= p->friction_static * dt) {
      *stick = true;
      return -drive_inc; /* the friction holds the rack exactly */
    }
    float breakaway = p->friction_static * dt;
    return drive_inc >= 0.0f ? -breakaway : breakaway;
  }
  float force = stribeck_magnitude(p, speed) * dt;
  return vel >= 0.0f ? -force : force;
}

/* The forces that do not come from the motor, for one tick: the road, the dry
 * friction and the centring stiffness of the column. Each one is written to
 * the state for the diagnostics and for the tests. */
static float external_increment(relais_plant_ext *pl, float drive_inc, float disturbance,
                                float dt, bool *stick) {
  const relais_phys_params *p = &pl->params;
  float road = disturbance * dt;
  pl->friction = friction_armed(p) ? friction_increment(p, pl->vel, drive_inc + road, dt, stick)
                                   : 0.0f;
  pl->spring = p->stiffness > 0.0f ? -p->stiffness * pl->pos * dt : 0.0f;
  return road;
}

/* The drive of one tick, saturated by the motor current when a limit is set. */
static float drive_increment(float vel, float target, float alpha, const relais_phys_params *p,
                             float dt) {
  float drive = (target - vel) * alpha;
  float limit = p->current_limit;
  if (limit > 0.0f) {
    float bound = limit * dt;
    if (drive > bound) drive = bound;
    else if (drive < -bound) drive = -bound;
  }
  return drive;
}

static void clamp_load(relais_plant_ext *pl) {
  if (!isfinite(pl->vel)) pl->vel = 0.0f;
  if (pl->vel > EXT_VEL_LIMIT) pl->vel = EXT_VEL_LIMIT;
  if (pl->vel < -EXT_VEL_LIMIT) pl->vel = -EXT_VEL_LIMIT;
  if (!isfinite(pl->pos)) pl->pos = 0.0f;
}

/* The accumulated kinetic energy, the bound the stability test measures. */
static void accumulate(relais_plant_ext *pl, float dt) {
  pl->energy += 0.5f * pl->vel * pl->vel * dt;
  if (!isfinite(pl->energy)) pl->energy = 0.0f;
  pl->steps++;
}

/* The published form of one tick: the drive acts directly on the load. The
 * order of the operations is the one of relais_plant_step(), so a model with
 * every parameter at zero gives the same bits. */
static void step_plain(relais_plant_ext *pl, float target, float disturbance, float dt,
                       float alpha) {
  float drive = drive_increment(pl->vel, target, alpha, &pl->params, dt);
  pl->drive = drive / dt;
  bool stick = false;
  float road = external_increment(pl, drive, disturbance, dt, &stick);
  float inc = drive + road; /* the published acceleration term */
  if (pl->friction != 0.0f) inc += pl->friction;
  if (pl->spring != 0.0f) inc += pl->spring;
  if (stick) {
    pl->vel = 0.0f; /* the rack holds: the friction balanced the drive */
  } else {
    if (!isfinite(inc)) inc = 0.0f;
    pl->vel += inc;
    clamp_load(pl);
    pl->pos += pl->vel * dt;
    if (!isfinite(pl->pos)) pl->pos = 0.0f;
  }
  accumulate(pl, dt);
}

/* The two mass form of one tick: the motor runs its own velocity loop and the
 * load is dragged through the tooth pair. Outside the play band the two are
 * rigidly coupled (perfectly inelastic contact); inside the band the load
 * coasts on its own forces, and the engaged side is remembered. */
static void step_with_play(relais_plant_ext *pl, float target, float disturbance, float dt,
                           float alpha) {
  const relais_phys_params *p = &pl->params;
  float drive = drive_increment(pl->m_vel, target, alpha, p, dt);
  pl->drive = drive / dt;
  pl->m_vel += drive;
  if (!isfinite(pl->m_vel)) pl->m_vel = 0.0f;
  if (pl->m_vel > EXT_VEL_LIMIT) pl->m_vel = EXT_VEL_LIMIT;
  if (pl->m_vel < -EXT_VEL_LIMIT) pl->m_vel = -EXT_VEL_LIMIT;
  pl->m_pos += pl->m_vel * dt;
  if (!isfinite(pl->m_pos)) pl->m_pos = 0.0f;
  bool stick = false;
  float road = external_increment(pl, 0.0f, disturbance, dt, &stick);
  float ext = road + pl->friction + pl->spring;
  float half = p->backlash * 0.5f;
  float gap = pl->m_pos - pl->pos;
  if (gap > half) {
    pl->contact = 1.0f;
    pl->pos = pl->m_pos - half;
    pl->vel = pl->m_vel + ext;
    pl->m_vel = pl->vel; /* rigid contact: the two masses share the velocity */
  } else if (gap < -half) {
    pl->contact = -1.0f;
    pl->pos = pl->m_pos + half;
    pl->vel = pl->m_vel + ext;
    pl->m_vel = pl->vel;
  } else if (!stick) {
    pl->vel += ext; /* free play: the load coasts */
    clamp_load(pl);
    pl->pos += pl->vel * dt;
  } else {
    pl->vel = 0.0f;
  }
  clamp_load(pl);
  accumulate(pl, dt);
}

void relais_plant_ext_step(relais_plant_ext *pl, float value, float disturbance, float dt) {
  if (pl == NULL || !(dt > 0.0f)) return;
  const float alpha = 1.0f - expf(-dt / RELAIS_PLANT_TAU);
  const float target = (RELAIS_PLANT_GAIN / RELAIS_PLANT_DAMP) * value;
  if (pl->params.backlash > 0.0f) step_with_play(pl, target, disturbance, dt, alpha);
  else step_plain(pl, target, disturbance, dt, alpha);
}
