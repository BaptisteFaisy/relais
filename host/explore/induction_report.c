/* Relais — the published report of the k-induction engine (spec 7.3, idea 2).
 *
 * The engine itself lives in induction.c; this file holds only the table of
 * invariants x k, the verdicts and the printed report, so that induction.c
 * stays under the 500-line limit of the project rules.
 *
 * The invariants listed here are exactly the ones the core library checks
 * (core/src/invariants.c): one implementation of the safety checks, and this
 * table only names them. Nothing here re-implements a check.
 */
#include "induction.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The per-invariant table and the report.                             */
/* ------------------------------------------------------------------ */

typedef struct {
  const char *id;
  bool exercised;
  const char *what;
} ind_spec;

/* The invariants the core library checks (core/src/invariants.c). "exercised"
 * says whether the explorer can reach the check at all: I5 needs the twin
 * without fault and DET-09 needs a value fault, and the world of host/explore
 * has neither (world.c feeds no twin and injects no value fault). */
static const ind_spec IND_SPECS[] = {
    {"I1", true, "exactly one value per tick, from a known source"},
    {"I2", true, "the mandate never goes backwards, never changes twice in a tick"},
    {"I3", true, "no order of a deposed machine is applied"},
    {"I4", true, "the gap between two fresh orders stays inside p+1"},
    {"I5", false, "continuity against the twin (needs the twin: out of scope)"},
    {"I6", true, "the will is never played past its horizon"},
    {"I7", true, "no handover without a fault injected"},
    {"I8", true, "the fallback loop is reached within N+1 ticks"},
    {"I9", true, "a returning machine does not disturb the command"},
    {"I10", true, "a loss of redundancy shows on the lamp within two ticks"},
    {"I11", true, "the tick numbering is contiguous"},
    {"DET-03", true, "the actuator changes hands only on its own evidence"},
    {"DET-09", false, "a wrong value is reported (needs a value fault: out of scope)"},
};
#define IND_SPEC_COUNT (sizeof IND_SPECS / sizeof IND_SPECS[0])

size_t induction_verdicts(const ind_result *res, ind_verdict *out, size_t cap) {
  size_t n = 0;
  for (size_t i = 0; i < IND_SPEC_COUNT && n < cap; i++) {
    ind_verdict *v = &out[n++];
    memset(v, 0, sizeof *v);
    v->id = IND_SPECS[i].id;
    v->exercised = IND_SPECS[i].exercised;
    v->k_min = res->proved ? res->k_close : 0u;
    if (!res->proved && res->first_id[0] == '\0') {
      /* No violation, but the set of situations did not close: the result
       * stays bounded, and the table says so instead of claiming a proof. */
      v->inductive = false;
      snprintf(v->note, sizeof v->note, "bounded: the situations did not close (explored to k=%u)",
               (unsigned)res->k_close);
    } else if (!res->proved) {
      v->inductive = false;
      if (strcmp(res->first_id, v->id) == 0)
        snprintf(v->note, sizeof v->note, "refuted at depth k=%u: %.100s", res->k_close, res->first);
      else
        snprintf(v->note, sizeof v->note, "not established: the proof stops at %s", res->first_id);
    } else if (!v->exercised) {
      v->inductive = false;
      snprintf(v->note, sizeof v->note, "not exercised by the explorer (%s)", IND_SPECS[i].what);
    } else {
      v->inductive = true;
      snprintf(v->note, sizeof v->note, "conjunct of the inductive property, k=%u", res->k_close);
    }
  }
  return n;
}

void induction_report(FILE *f, const ind_config *cfg, const ind_result *res,
                      const ind_property *prop) {
  ind_verdict verdicts[IND_SPEC_COUNT];
  size_t n = induction_verdicts(res, verdicts, IND_SPEC_COUNT);
  size_t proved = 0;
  for (size_t i = 0; i < n; i++)
    if (verdicts[i].inductive) proved++;
  fprintf(f, "INDUCTION property=%s level=%s faults<=%d k=%u\n", prop->id,
          cfg->complete ? "complet" : "socle", cfg->faults, (unsigned)cfg->k);
  fprintf(f, "  base case : %ld concrete states, %ld expanded, depth %u ticks\n",
          res->base_states, res->base_transitions, (unsigned)res->k_base);
  fprintf(f, "  step      : %ld situations, %ld concrete states, %ld transitions, deepest tick %u\n",
          res->classes, res->class_states, res->class_transitions, (unsigned)res->max_tick);
  fprintf(f, "  quotient closed: %s (k_close=%u)\n", res->closed ? "yes" : "NO",
          (unsigned)res->k_close);
  fprintf(f, "  invariants proved inductive: %zu / %zu\n", proved, n);
  for (size_t i = 0; i < n; i++)
    fprintf(f, "    %-7s %-12s k=%-4u %s\n", verdicts[i].id,
            verdicts[i].inductive ? "INDUCTIVE" : "not proved", (unsigned)verdicts[i].k_min,
            verdicts[i].note);
  if (res->proved)
    fprintf(f, "  VERDICT: PROVED for every depth (no bound on k)\n");
  else if (res->first_id[0] == '\0')
    fprintf(f, "  VERDICT: BOUNDED -- no violation in the %ld concrete states examined, but the "
               "set of situations did not close within the budget (explored to depth %u)\n",
            res->class_states, (unsigned)res->k_close);
  else
    fprintf(f, "  VERDICT: REFUSED -- %s\n", res->first);
  fprintf(f, "  time: %.1f s\n", res->seconds);
}

