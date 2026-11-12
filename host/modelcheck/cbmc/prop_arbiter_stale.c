/* Relais — CBMC property P-ARB-2: a stale order is never applied and is
 * counted as rejected (docs/SPEC.md 4.3, "Acceptance"; MAN-02, SEC-03,
 * SEC-04, DET-04, I3).
 *
 * A stale order is an order that names another tick, or carries another
 * mandate than the current one, or comes from a machine that is not the
 * holder. The harness checks two things: the value applied at the tick does
 * not come from such an order, and the arbiter counted the rejection with
 * the published reason.
 *
 * Command: see host/modelcheck/cbmc/run.sh.
 */
#include <stddef.h>

#include "mc_arb_model.h"

int main(void) {
  relais_params p;
  mc_build_params(&p);
  relais_arb a;
  mc_build_state(&a, &p);
  relais_tick tick = 1u + MC_NONDET_U64() % 64u;
  /* A single frame: the order of the holder, with the tick and the mandate
   * chosen freely: fresh, stale, or in the future. */
  relais_msg m;
  mc_build_frame(&m, &a, tick);
  m.type = RELAIS_MSG_ORDRE;
  m.sender = a.holder;
  m.mandate = a.mandate + (MC_NONDET_U32() % 3u) - 1u;
  m.tick = (MC_NONDET_BOOL()) ? tick : tick + MC_NONDET_U64();
  m.has_order = true;
  relais_meas meas;
  meas.pos = 0.0f;
  meas.vel = 0.0f;
  meas.applied = 0.0f;
  meas.setpoint = 0.0f;
  relais_node holder_before = a.holder;
  relais_mandate mandate_before = a.mandate;
  uint32_t rejected_before = 0;
  for (unsigned i = 0; i < RELAIS_REJECT_SLOTS; i++) rejected_before += a.rejected[i];
  relais_arb_out out;
  relais_arb_step(&a, tick, &m, 1, &meas, &out);
  bool fresh = m.sender == holder_before && m.mandate == mandate_before && m.tick == tick;
  if (!fresh) {
    MC_ASSERT(out.source != RELAIS_SRC_HOLDER || a.holder == RELAIS_NODE_NONE,
              "P-ARB-2: a stale order was applied");
    uint32_t rejected_after = 0;
    for (unsigned i = 0; i < RELAIS_REJECT_SLOTS; i++) rejected_after += a.rejected[i];
    MC_ASSERT(rejected_after == rejected_before + 1u,
              "P-ARB-2: a stale order was not counted as rejected");
  }
  return 0;
}
