/* Relais — control law, plant model, trajectories, testament (spec 4.2, 4.4,
 * 4.8, 5.1).
 *
 * The law is a pure function of its state and of the measurements: same binary,
 * same inputs, same bits on both machines (CMD-03). The interface is three
 * functions so another law can be dropped in without touching the protocol
 * (CMD-09): init, step, export/import.
 */
#ifndef RELAIS_CONTROL_H
#define RELAIS_CONTROL_H

#include "relais/params.h"
#include "relais/types.h"

typedef struct {
  float integ;     /* integral term, anti-windup limited */
  float filt;      /* filtered measurement */
  float last_pos;  /* previous position, for the derivative term */
  float last_value;/* previous command, for the rate limit */
  float phase;     /* position in the trajectory, seconds */
  float fb_value;  /* value the fallback law would apply now */
  float prev_setpoint; /* previous steering request, for the extrapolation */
  float setpoint_rate; /* its slope, per second: the will follows the manoeuvre */
  uint32_t ticks;  /* ticks computed, for diagnostics */
} relais_ctl_state;

/* Rack model: second order, with a road disturbance (CMD-05). */
typedef struct {
  float pos;
  float vel;
  float t;           /* elapsed seconds */
  float disturbance;
  uint32_t seed;     /* deterministic pseudo-random disturbance */
} relais_plant;

typedef enum {
  RELAIS_TRAJ_SLALOM = 0,
  RELAIS_TRAJ_LANE_CHANGE = 1,
  RELAIS_TRAJ_STEP = 2,
  RELAIS_TRAJ_CONST = 3,
  RELAIS_TRAJ_BRAKE = 4 /* CMD-08: second demonstration profile */
} relais_traj_kind;

typedef struct {
  relais_traj_kind kind;
  float amp;        /* amplitude, normalised */
  float freq;       /* Hz, for the slalom */
  float offset;
  float t_start;    /* seconds, for the step and the lane change */
  float dist_amp;   /* road disturbance amplitude */
  float dist_freq;
  uint32_t seed;
} relais_traj;

void relais_traj_default(relais_traj *t);
/* Trajectories and disturbances are functions of time only: the twin without
 * fault and the faulted run share the same reference. */
float relais_traj_setpoint(const relais_traj *t, double seconds);
float relais_traj_disturbance(const relais_traj *t, double seconds);
/* Parse a scenario file body ("kind=slalom amp=0.6 freq=0.5 ..."). */
bool relais_traj_parse(relais_traj *t, const char *body, char *why, size_t why_len);

void relais_ctl_init(relais_ctl_state *s, const relais_params *p);
/* One period of control: returns the bounded command for the next tick. */
float relais_ctl_step(relais_ctl_state *s, const relais_meas *m, const relais_params *p);
uint8_t relais_ctl_export(const relais_ctl_state *s, uint8_t *buf, size_t cap);
bool relais_ctl_import(relais_ctl_state *s, const uint8_t *buf, size_t len);
/* REP-04: rebuild a bump-free state from what the actuator publishes, used by
 * a second that no longer receives ETAT. */
void relais_ctl_reconstruct(relais_ctl_state *s, const relais_meas *m, const relais_params *p);

/* Testament (spec 4.2 idea 3, ACT-02): the values the holder expects for the
 * next N ticks, so the actuator can keep playing its plan while the handover
 * happens. Written into out[0..*len-1]. */
void relais_testament_fill(const relais_ctl_state *s, const relais_meas *m,
                           const relais_params *p, float *out, uint8_t *len);
/* Value of the testament for a tick, or false when the horizon is over (I6). */
bool relais_testament_at(const float *testament, uint8_t len, relais_tick base,
                         relais_tick tick, float *out);

/* Fallback law of the actuator: damped local loop that holds the position. */
void relais_fallback_init(float *fb_state, const relais_meas *m);
float relais_fallback_step(float *fb_state, const relais_meas *m, const relais_params *p);

void relais_plant_init(relais_plant *pl, float pos0, uint32_t seed);
/* One integration step of the rack, dt in seconds. */
void relais_plant_step(relais_plant *pl, float value, float disturbance, float dt);

/* Bounds applied by the emitting machine (CMD-06) and by the actuator (ACT-05). */
float relais_clamp_value(float v, const relais_params *p);
float relais_clamp_slew(float previous, float wanted, const relais_params *p);

#endif /* RELAIS_CONTROL_H */
