/* Relais — shared fixtures for the control-law and the rack-model tests.
 * One implementation, two test binaries: no copy-paste between them.
 */
#ifndef RELAIS_FIXTURES_CONTROL_H
#define RELAIS_FIXTURES_CONTROL_H

#include "test_util.h"

typedef struct {
  bool finite;
  float max_value; /* |command| */
  float max_slew;  /* |command - previous command| */
  float max_pos;   /* |rack position| */
  float max_err;   /* |setpoint - position| once settled */
} loop_stats;

void stats_reset(loop_stats *st);
/* Close the position loop over the rack for `ticks` periods and report the
 * extremes; returns the last command. */
float loop_run(relais_ctl_state *s, relais_plant *pl, const relais_traj *tr,
               const relais_params *p, int ticks, double settle_s, loop_stats *st);
void meas_set(relais_meas *m, float pos, float vel, float applied, float setpoint);
void meas_random(relais_meas *m, t_rng *rng, float applied);

#endif /* RELAIS_FIXTURES_CONTROL_H */
