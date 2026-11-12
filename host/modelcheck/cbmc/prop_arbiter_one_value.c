/* Relais — CBMC property P-ARB-3: exactly one value is applied per tick, and
 * the counter of the source that served it is the only one that moves
 * (docs/SPEC.md 4.4; ACT-01, I1, DET-01).
 *
 * The harness drives the REAL relais_arb_step() with a symbolic state and up
 * to two symbolic frames, then checks the four published sources and the
 * per-source counters of the arbiter.
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
  uint32_t before[5];
  for (unsigned i = 0; i < 5; i++) before[i] = a.applied_by_source[i];
  relais_tick tick = 1u + MC_NONDET_U64() % 64u;
  relais_msg msgs[2];
  for (uint32_t i = 0; i < 2; i++) mc_build_frame(&msgs[i], &a, tick);
  uint32_t n = MC_NONDET_U32() % 3u;
  relais_meas meas;
  meas.pos = MC_NONDET_FLOAT();
  meas.vel = MC_NONDET_FLOAT();
  meas.applied = MC_NONDET_FLOAT();
  meas.setpoint = MC_NONDET_FLOAT();
  relais_arb_out out;
  relais_arb_step(&a, tick, msgs, n, &meas, &out);

  uint32_t moved = 0;
  for (unsigned i = 0; i < 5; i++) {
    uint32_t delta = a.applied_by_source[i] - before[i];
    MC_ASSERT(delta <= 1u, "P-ARB-3: one source served more than one value");
    moved += delta;
  }
  MC_ASSERT(moved == 1u, "P-ARB-3: the tick did not serve exactly one value");
  MC_ASSERT(out.applied, "P-ARB-3: no value applied at this tick");
  MC_ASSERT(out.source >= RELAIS_SRC_HOLDER && out.source <= RELAIS_SRC_FALLBACK,
            "P-ARB-3: value applied from an unknown source");
  MC_ASSERT(a.applied_by_source[out.source] == before[out.source] + 1u,
            "P-ARB-3: the counter of the source that served the tick did not move");
  return 0;
}
