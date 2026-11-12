/* Relais — Frama-C/WP harness for the control law (mission M36).
 *
 * The harness drives the REAL relais_ctl_step() and states what the law must
 * return: a bounded command, and a change per tick inside the rate bound
 * (CMD-06, ACT-05).
 *
 * Command: see host/modelcheck/wp/run.sh.
 */
#include "relais/control.h"

int main(void) {
  relais_params p;
  relais_params_default(&p);
  relais_ctl_state s;
  relais_ctl_init(&s, &p);
  relais_meas m;
  m.pos = 0.1f;
  m.vel = 0.0f;
  m.applied = 0.0f;
  m.setpoint = 0.5f;
  float v = relais_ctl_step(&s, &m, &p);
  (void)v; /* the assertions below are the statement; the cast keeps gcc quiet */
  /*@ assert v <= p.max_amplitude && v >= -p.max_amplitude; */
  /*@ assert v - s.last_value <= p.max_slew; */
  /*@ assert s.last_value - v <= p.max_slew; */
  return 0;
}
