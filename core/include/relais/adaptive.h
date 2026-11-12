/* Relais — an adaptive control law (CMD-03, CMD-09, REP-01, mission M30).
 *
 * The published law of core/src/control.c has fixed gains. This module offers
 * the same law with gains that depend on the state: the proportional and the
 * derivative gains are SCHEDULED on the measured speed, and the change of
 * gain is rate limited, so the state of the law stays smooth and bounded
 * (the robustness guard of the mission).
 *
 * With flat gains (kp_low == kp_high, kd_low == kd_high) the law computes
 * exactly what the published law computes, bit for bit: the adaptive law is a
 * strict generalisation of the published one, not a different controller.
 *
 * The state of the law is bigger than the published one (the scheduled gains
 * and the counters are part of it), so the replica carried by ETAT is bigger
 * too (REP-01). That is the price the mission asks to measure: a law that
 * changes its internal state makes the replication heavier and the comparison
 * between an order and a shadow more sensitive, because a stale replica no
 * longer implies the same gains.
 *
 * The law can be installed on a node through CMD-09 (relais_node_set_law),
 * which is how the protocol effect is measured.
 *
 * Spec: CMD-03 (same inputs, same bits), CMD-09 (law is pluggable),
 *       REP-01 (the state fits in 256 bytes), REP-02 (bad state refused),
 *       CMD-06 (amplitude and slew bounds).
 */
#ifndef RELAIS_ADAPTIVE_H
#define RELAIS_ADAPTIVE_H

#include "relais/control.h"
#include "relais/node.h"

typedef struct {
  float kp_low;      /* proportional gain at rest */
  float kp_high;     /* proportional gain at the reference speed */
  float kd_low;      /* derivative gain at rest */
  float kd_high;     /* derivative gain at the reference speed */
  float ki;          /* integral gain */
  float vel_ref;     /* speed at which the high gains apply, position/s */
  float gain_slew;   /* maximum gain change per tick (robustness guard) */
  float integ_limit; /* bound of the integral term */
  float rate_limit;  /* bound of the setpoint slope, full scale per second */
  float filt;        /* measurement filter coefficient */
} relais_adaptive_params;

typedef struct {
  float integ;          /* integral term, anti-windup limited */
  float filt;           /* filtered measurement */
  float last_pos;       /* previous position */
  float last_value;     /* previous command */
  float phase;          /* position in the trajectory, seconds */
  float fb_value;       /* value the fallback law would apply now */
  float prev_setpoint;  /* previous steering request */
  float setpoint_rate;  /* its slope, per second */
  float kp;             /* current proportional gain */
  float kd;             /* current derivative gain */
  uint32_t ticks;       /* ticks computed */
  uint32_t changes;     /* number of gain changes (diagnostic) */
} relais_adaptive_state;

typedef struct {
  relais_adaptive_state state;
  relais_adaptive_params params;
} relais_adaptive;

/* The published law's tuning: flat gains, so the law equals the published
 * one bit for bit. */
void relais_adaptive_params_default(relais_adaptive_params *p);
/* A demonstration schedule: stiffer at speed, which is what a speed
 * dependent tuning does on a steering rack. */
void relais_adaptive_params_demo(relais_adaptive_params *p);

void relais_adaptive_init(relais_adaptive *a, const relais_adaptive_params *p);
/* One period of control: the bounded command for the next tick. */
float relais_adaptive_step(relais_adaptive *a, const relais_meas *m, const relais_params *p);

/* REP-01 / REP-02: export and import of the whole state, refused when the
 * version or the length is unknown. */
uint8_t relais_adaptive_export(const relais_adaptive *a, uint8_t *buf, size_t cap);
bool relais_adaptive_import(relais_adaptive *a, const uint8_t *buf, size_t len);
/* The size of the replica, for the report. */
size_t relais_adaptive_state_size(void);

/* CMD-09: the vtable that installs the law on a node. */
void relais_adaptive_law(relais_law *law, relais_adaptive *a);

#endif /* RELAIS_ADAPTIVE_H */
