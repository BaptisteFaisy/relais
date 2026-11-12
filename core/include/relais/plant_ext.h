/* Relais — a more faithful rack model (CMD-05, mission M30).
 *
 * The published model (core/src/plant.c) is a first order velocity loop: the
 * command drives a velocity, the road pushes back. It is deliberately simple,
 * because 19 659 published runs were made with it, and it is the reference of
 * every proof so far.
 *
 * This module adds four phenomena, each one OPTIONAL and INDEPENDENT:
 *   1. dry friction with a Stribeck curve (static, kinetic, viscous),
 *   2. mechanical play in the transmission (a dead zone that remembers which
 *      side of the play is in contact),
 *   3. saturation of the motor current (the drive force is capped),
 *   4. stiffness of the steering column / pneumatic return (a centring
 *      spring pulling the rack back to the straight-ahead position).
 *
 * The rule of the module: a parameter left at zero DISABLES its phenomenon and
 * the phenomenon is then not even computed, so a fully zeroed parameter
 * structure reproduces the published model bit for bit (see the test "the
 * zeroed model is the published one, bit for bit"). The faithful model is
 * therefore an OPTION, never a replacement (mission M30, non-regression).
 *
 * Units: the model works in the same normalised units as the published one
 * (full scale = 1.0). An acceleration means position units per second
 * squared; every internal term is expressed as a velocity increment per tick,
 * which is the convention of the published model (vel += (target - vel)*alpha,
 * with alpha = 1 - exp(-dt/tau) the exact integration of a first order lag).
 *
 * Provenance of the constants: the values shipped by
 * relais_phys_params_demo() are DEMONSTRATION VALUES. Their orders of
 * magnitude are those of a passenger car steering rack (breakaway friction of
 * the order of ten per cent of the nominal drive acceleration, a play of the
 * order of two per cent of the travel, a current limit that only binds during
 * a fast reversal, a centring stiffness of the order of five per cent of the
 * drive at ten per cent of the travel), but NO specific vehicle is claimed and
 * NO published data sheet is cited: they are plausible, documented and
 * tunable. The published model keeps its own constants, shared with this
 * module through the macros below.
 *
 * Spec: CMD-05 (rack model), CMD-08 (braking profile), REP-04 (reconstruction).
 */
#ifndef RELAIS_PLANT_EXT_H
#define RELAIS_PLANT_EXT_H

#include "relais/control.h"

/* The constants of the published rack, shared with core/src/plant.c so that
 * the two models integrate the very same velocity loop (one set of numbers, no
 * duplicated magic values). */
#define RELAIS_PLANT_GAIN 4.0f
#define RELAIS_PLANT_DAMP 1.0f
#define RELAIS_PLANT_TAU 0.02f

/* Parameters of the faithful model. Every field is zero by default, which
 * means "the phenomenon is off". */
typedef struct {
  /* 1. Dry friction, Stribeck shape. */
  float friction_static;   /* breakaway acceleration, position/s^2 */
  float friction_kinetic;  /* acceleration once sliding, position/s^2 */
  float friction_viscous;  /* viscous coefficient, position/s^3 */
  float friction_velocity; /* Stribeck speed scale, position/s */
  float friction_band;     /* speed band under which the rack may stick */

  /* 2. Mechanical play of the transmission, position units (total width).
   * The tooth pair is modelled as a rigid, perfectly inelastic coupling:
   * outside the band the load takes the velocity of the motor, inside the
   * band it coasts on its own forces. No tuning parameter is needed, and the
   * relative travel stays geometrically inside the play. */
  float backlash;

  /* 3. Saturation of the motor current, position/s^2 (drive acceleration). */
  float current_limit;

  /* 4. Stiffness of the column / pneumatic return, 1/s^2. */
  float stiffness;
} relais_phys_params;

/* State of the faithful model. */
typedef struct {
  relais_phys_params params;
  float pos;       /* load side position (the measured one) */
  float vel;       /* load side velocity */
  float m_pos;     /* motor side position (before the play) */
  float m_vel;     /* motor side velocity */
  float contact;   /* -1, 0 or +1: the side of the play in contact */
  float drive;     /* drive acceleration applied, position/s^2 */
  float friction;  /* friction velocity increment applied, per tick */
  float spring;    /* centring velocity increment applied, per tick */
  float energy;    /* accumulated kinetic energy, for the energy bound */
  uint32_t steps;
} relais_plant_ext;

/* All zeros: the published model. */
void relais_phys_params_default(relais_phys_params *p);
/* A demonstration set, with the four phenomena enabled at once. */
void relais_phys_params_demo(relais_phys_params *p);
/* True when at least one phenomenon is enabled. */
bool relais_phys_params_enabled(const relais_phys_params *p);

void relais_plant_ext_init(relais_plant_ext *pl, float pos0, const relais_phys_params *p);
/* One integration step, dt in seconds. Same convention as
 * relais_plant_step(): the command drives the velocity, the road pushes. */
void relais_plant_ext_step(relais_plant_ext *pl, float value, float disturbance, float dt);

#endif /* RELAIS_PLANT_EXT_H */
