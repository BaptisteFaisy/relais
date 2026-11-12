/* Relais — counter-example reduction by delta-debugging (spec 7.3, idea 7).
 *
 * The reducer is exercised on a real breach: built against a mutant
 * (-DRELAIS_MUTANT=n), the explorer hands back a counter-example trace, and the
 * reducer must return a STRICTLY SHORTER trace that still breaches the SAME
 * invariant. Against the nominal core no breach exists, so the test checks the
 * mechanism instead: a trace that never violates is left untouched (the oracle
 * rejects every deletion), and the scenario writer still produces a replayable
 * file. Both paths share one implementation, so the check is the same code the
 * explorer runs with --shrink.
 */
#include <string.h>

#include "explore.h"
#include "shrink.h"
#include "test_util.h"

#define SHR_BUDGET 20000
#define SHR_DEPTH 14

/* Pull the invariant id ("DET-03", "I10", ...) out of a relais_inv_report. */
static void first_id(const char *report, char *out, size_t cap) {
  out[0] = '\0';
  const char *at = strstr(report, "first: ");
  if (at == NULL) return;
  at += 7; /* strlen("first: ") */
  size_t i = 0;
  while (*at != '\0' && *at != ' ' && i + 1 < cap) out[i++] = *at++;
  out[i] = '\0';
}

/* A calm trace: pure ticks, no fault. On the nominal core the holder is armed
 * and keeps ordering, so nothing is breached and the reducer must give the
 * trace back whole (every deletion is rejected by the oracle). */
static size_t build_calm_trace(explore_event *ev) {
  size_t n = 0;
  for (int i = 0; i < 12; i++) { ev[n].kind = EX_TICK; ev[n].link = -1; ev[n].node = -1; n++; }
  return n;
}

static void test_shrink_mechanism(void) {
  t_section("reducer only deletes what the oracle accepts");
  relais_params p;
  relais_traj t;
  relais_params_default(&p);
  relais_traj_default(&t);
  explore_event ev[16];
  explore_event out[16];
  size_t n = build_calm_trace(ev);
  bool bad = shrink_violates(&p, &t, true, ev, n, NULL, 0);
  size_t m = shrink_trace(&p, &t, true, ev, n, out, 16);
  if (!bad) {
    /* Nominal core: pure ticking breaches nothing, so no deletion survives the
     * oracle and the trace comes back whole. */
    T_CHECK(!bad);
    T_EQ_U(m, n);
  } else {
    /* A mutant whose lamp defect (M8) breaches I10 even on pure ticks: the
     * reducer may shorten it, but the shortened trace must still breach. */
    T_CHECK(m <= n);
    T_CHECK(shrink_violates(&p, &t, true, out, m, NULL, 0));
  }
}

static void test_shrink_counterexample(void) {
  t_section("reducer minimises a real counter-example");
  relais_params p;
  relais_traj t;
  relais_params_default(&p);
  relais_traj_default(&t);

  explore_result res =
      explore_search(&p, &t, true, SHR_BUDGET, SHR_DEPTH, 1, false, false, true, NULL);
  T_CHECK(res.ok);

  if (!res.failed) {
    /* Nominal core: nothing to minimise; the mechanism test above is the proof. */
    T_CHECK(res.trace_len == 0);
    return;
  }

  /* A mutant build: the raw counter-example breaches an invariant. */
  char raw_report[RELAIS_INV_MSG_MAX * 2];
  T_CHECK(shrink_violates(&p, &t, true, res.trace, res.trace_len, raw_report,
                          sizeof raw_report));
  char raw_id[32];
  first_id(raw_report, raw_id, sizeof raw_id);
  T_CHECK(raw_id[0] != '\0');

  explore_event reduced[SHRINK_TMP_MAX];
  size_t rn = shrink_trace(&p, &t, true, res.trace, res.trace_len, reduced, SHRINK_TMP_MAX);

  /* Strictly shorter, and still breaching the SAME invariant. */
  T_CHECK(rn < res.trace_len);
  char red_report[RELAIS_INV_MSG_MAX * 2];
  T_CHECK(shrink_violates(&p, &t, true, reduced, rn, red_report, sizeof red_report));
  char red_id[32];
  first_id(red_report, red_id, sizeof red_id);
  T_CHECK(strcmp(raw_id, red_id) == 0);

  /* The reduced trace is written as a replayable CONTRACT.md scenario. */
  FILE *f = tmpfile();
  T_CHECK(f != NULL);
  if (f != NULL) {
    shrink_write_scenario(f, &p, &t, true, reduced, rn);
    long sz = ftell(f);
    T_CHECK(sz > 0);
    fclose(f);
  }
}

int main(void) {
  test_shrink_mechanism();
  test_shrink_counterexample();
  return t_summary();
}
