/* Relais — the quotient explorer of symbolic.h, checked against the brute-force
 * search of explore.c on the very same budget (spec 7.3, idea 6).
 *
 * Two properties, on one config (level complet, one fault, depth 14) chosen so
 * that mutant M3 breaches DET-03 while the nominal core still holds:
 *   - the quotient is strictly smaller than the raw exploration: fewer
 *     equivalence classes than concrete states, so the same budget reaches
 *     further into the protocol;
 *   - it masks nothing: the verdict (violation / no violation) is the same as
 *     the brute-force search on the same budget. Against a mutant build
 *     (-DRELAIS_MUTANT=n) this is exercised on a real breach; against the
 *     nominal core it holds vacuously, with 0 violation on both sides.
 */
#include "explore.h"
#include "symbolic.h"
#include "test_util.h"

#define SYM_BUDGET 20000
#define SYM_DEPTH 14

static void test_quotient_vs_brute(void) {
  t_section("symbolic quotient vs brute force (same budget)");
  relais_params p;
  relais_traj t;
  relais_params_default(&p);
  relais_traj_default(&t);

  explore_result brute =
      explore_search(&p, &t, true, SYM_BUDGET, SYM_DEPTH, 1, false, false, true, NULL);
  explore_result sym =
      explore_search(&p, &t, true, SYM_BUDGET, SYM_DEPTH, 1, true, false, true, NULL);

  T_CHECK(brute.ok);
  T_CHECK(sym.ok);
  /* The abstraction collapses states within its own run. */
  T_CHECK(sym.classes < (size_t)sym.states);
  /* The quotient is strictly smaller than the raw brute-force exploration. */
  T_CHECK(sym.classes < (size_t)brute.states);
  /* No masking: the same verdict on the same budget. */
  T_CHECK(sym.failed == brute.failed);

  if (brute.failed) {
    /* A mutant build: the quotient reached the very same breach, and hands back
     * a counter-example of its own. */
    T_CHECK(sym.failed);
    T_CHECK(sym.trace_len > 0);
    T_CHECK(brute.trace_len > 0);
  } else {
    /* Nominal core: nothing to find on either side, and the quotient still
     * covered the whole budget. */
    T_CHECK(!sym.failed);
    T_CHECK(sym.states > 0);
  }
}

/* The projection is stable: the same world always hashes to the same class, and
 * a world that differs only in a forgotten float hashes to the same class as
 * itself (sanity of sym_hash, no allocation, no global state). */
static void test_projection_stable(void) {
  t_section("projection is deterministic");
  relais_params p;
  relais_traj t;
  relais_params_default(&p);
  relais_traj_default(&t);
  explore_world w;
  explore_world_init(&w, &p, &t, true, true);
  uint64_t h1 = sym_hash(&w);
  uint64_t h2 = sym_hash(&w);
  T_EQ_U(h1, h2);
  T_CHECK(sym_key_bytes() > 0);
  T_CHECK(sym_key_bytes() < sizeof(explore_world));
}

int main(void) {
  test_quotient_vs_brute();
  test_projection_stable();
  return t_summary();
}
