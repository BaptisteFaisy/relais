/* Relais — CBMC property P-ARB-1: the actuator applies an order ONLY if it
 * comes from the holder, carries the current mandate and targets the tick
 * being applied (docs/SPEC.md 4.3, "Acceptance"; MAN-02, SEC-03, SEC-04).
 *
 * This is the central safety property of the project: two machines may
 * believe they are holders, but only the order of the appointed holder, for
 * the coming tick, is applied. The harness drives the REAL
 * relais_arb_step() with a symbolic state and symbolic frames.
 *
 * Command: see host/modelcheck/cbmc/run.sh.
 */
#include <stddef.h>

#include "mc_arb_model.h"

#define FRAMES 2u

int main(void) {
  relais_params p;
  mc_build_params(&p);
  relais_arb a;
  mc_build_state(&a, &p);
  relais_tick tick = 1u + MC_NONDET_U64() % 64u;
  relais_msg msgs[FRAMES];
  for (uint32_t i = 0; i < FRAMES; i++) mc_build_frame(&msgs[i], &a, tick);
  uint32_t n = MC_NONDET_U32() % (FRAMES + 1u);
  relais_meas meas;
  meas.pos = MC_NONDET_FLOAT();
  meas.vel = MC_NONDET_FLOAT();
  meas.applied = MC_NONDET_FLOAT();
  meas.setpoint = MC_NONDET_FLOAT();
  relais_mandate before = a.mandate;
  relais_node holder_before = a.holder;
  relais_arb_out out;
  relais_arb_step(&a, tick, msgs, n, &meas, &out);

  /* I1 (ACT-01): exactly one value, from one of the four published sources. */
  MC_ASSERT(out.applied, "P-ARB-1: no value applied at this tick");
  MC_ASSERT(out.source >= RELAIS_SRC_HOLDER && out.source <= RELAIS_SRC_FALLBACK,
            "P-ARB-1: value applied from an unknown source");
  /* I2 (MAN-03): the mandate never decreases and moves by at most one step. */
  MC_ASSERT(a.mandate >= before, "P-ARB-1: the mandate went backwards");
  MC_ASSERT(a.mandate - before <= 1u, "P-ARB-1: the mandate moved twice in one tick");
  MC_ASSERT(!(holder_before != RELAIS_NODE_NONE && out.holder != RELAIS_NODE_NONE &&
              out.holder != holder_before && a.mandate == before),
            "P-ARB-1: two holders of the same mandate number");
  /* ACT-05: the applied value is bounded. */
  MC_ASSERT(out.value <= p.max_amplitude && out.value >= -p.max_amplitude,
            "P-ARB-1: the applied value is out of bounds");
  /* MAN-02 / SEC-03 / SEC-04: a value taken from the holder comes from an
   * order that named this tick and carried the current mandate. */
  if (out.source == RELAIS_SRC_HOLDER) {
    bool entitled = false;
    for (uint32_t i = 0; i < n; i++) {
      if (msgs[i].type != RELAIS_MSG_ORDRE) continue;
      if (msgs[i].sender == holder_before && msgs[i].mandate == before && msgs[i].tick == tick)
        entitled = true;
    }
    MC_ASSERT(entitled, "P-ARB-1: an order of a machine that is not the holder was applied");
  }
  return 0;
}
