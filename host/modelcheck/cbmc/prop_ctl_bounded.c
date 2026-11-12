/* Relais — CBMC property P-CTL-1: the control law always emits a finite
 * command inside the published bounds, whatever the measurement (docs/SPEC.md
 * 4.2; CMD-06, ACT-05, ROB-09).
 *
 * The law is the only thing that turns a measurement into a command, and the
 * actuator applies that command to the rack: a corrupted measurement must
 * never produce a NaN, an infinity or a command outside max_amplitude.
 *
 * Command: see host/modelcheck/cbmc/run.sh.
 */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "mc_cbmc.h"
#include "relais/control.h"

int main(void) {
  relais_params p;
  memset(&p, 0, sizeof p);
  p.period_ms = 5;
  p.patience = 1;
  p.testament_len = 8;
  p.certify_ticks = 3;
  p.bail_ticks = 4;
  p.request_burst = 4;
  p.request_window = 4;
  p.epsilon = 0.005f;
  p.delta = 0.02f;
  p.dissimilar_epsilon = 0.0f;
  p.dissimilar_delta = 0.02f;
  p.degraded_ticks = 6000;
  p.max_amplitude = MC_NONDET_FLOAT();
  MC_ASSUME(p.max_amplitude > 0.0f && p.max_amplitude <= 10.0f);
  p.max_slew = MC_NONDET_FLOAT();
  MC_ASSUME(p.max_slew > 0.0f && p.max_slew <= 10.0f);
  p.slow_motion = false;
  p.slow_factor = 10;
  relais_meas m;
  m.pos = MC_NONDET_FLOAT();
  m.vel = MC_NONDET_FLOAT();
  m.applied = MC_NONDET_FLOAT();
  m.setpoint = MC_NONDET_FLOAT();
  relais_ctl_state s;
  relais_ctl_init(&s, &p);
  float out = relais_ctl_step(&s, &m, &p);
  MC_ASSERT(isfinite(out), "P-CTL-1: the command is not a finite number");
  MC_ASSERT(out <= p.max_amplitude && out >= -p.max_amplitude,
            "P-CTL-1: the command is outside the amplitude bound");
  MC_ASSERT(fabsf(out - 0.0f) <= p.max_slew + 1e-6f,
            "P-CTL-1: the command jumped more than the rate bound");
  return 0;
}
