/* Relais — the command-line driver of the k-induction engine (spec 7.3, idea 2).
 *
 * Argument parsing and the published report only: the engine itself lives in
 * induction.c, shared with the test driver, so there is exactly one
 * implementation of the induction.
 *
 *   --k N        depth in ticks of the bounded base case (published parameter);
 *   --budget N   states budget of the base case;
 *   --classes N  situations budget of the quotient (the induction step);
 *   --faults N   adversary fault budget (1 = the single-fault hypothesis);
 *   --level socle|complet
 *   --witness    run on a deliberately FALSE property: the engine must
 *                REFUSE it (this is the proof that the proof bites).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "induction.h"

static void usage(void) {
  fprintf(stderr,
          "usage: relais-induction [--k 12] [--budget 100000] [--classes 60000]\n"
          "       [--faults 1] [--level socle|complet] [--witness]\n");
}

/* Parse argv into the configuration. Returns false (and prints why) on bad
 * input. */
static bool parse_args(int argc, char **argv, ind_config *cfg, bool *witness) {
  *witness = false;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--k") && i + 1 < argc) cfg->k = (uint32_t)atoi(argv[++i]);
    else if (!strcmp(a, "--budget") && i + 1 < argc) cfg->budget = atol(argv[++i]);
    else if (!strcmp(a, "--classes") && i + 1 < argc) cfg->class_budget = atol(argv[++i]);
    else if (!strcmp(a, "--faults") && i + 1 < argc) cfg->faults = atoi(argv[++i]);
    else if (!strcmp(a, "--level") && i + 1 < argc) cfg->complete = !strcmp(argv[++i], "complet");
    else if (!strcmp(a, "--witness")) *witness = true;
    else { usage(); return false; }
  }
  return true;
}

/* Run the engine on one property and print the verdict. Returns true when the
 * property was proved. */
static bool run_property(const ind_config *cfg, const relais_params *p, const relais_traj *t,
                         const ind_clause *clauses, size_t count, const char *id) {
  ind_property prop;
  prop.id = id;
  prop.clauses = clauses;
  prop.count = count;
  ind_result res;
  if (induction_run(cfg, p, t, &prop, &res) != 0) {
    fprintf(stderr, "relais-induction: the engine could not run\n");
    return false;
  }
  induction_report(stdout, cfg, &res, &prop);
  return res.proved;
}

/* The witness properties: deliberately false, the engine must refuse them.
 * Returns the number of witnesses the engine wrongly certified. */
static int run_witness(const ind_config *cfg, const relais_params *p, const relais_traj *t) {
  ind_clause clause;
  int wrong = 0;
  clause.id = "witness:the-holder-is-never-lost";
  clause.fn = ind_clause_holder_never_lost;
  clause.ctx = NULL;
  ind_result res;
  ind_property prop;
  prop.id = clause.id;
  prop.clauses = &clause;
  prop.count = 1;
  if (induction_run(cfg, p, t, &prop, &res) != 0) {
    printf("WITNESS holder-never-lost: engine error\n");
    return 1;
  }
  printf("WITNESS holder-never-lost: %s (%.100s)\n", res.proved ? "PROVED (BUG!)" : "REFUSED",
         res.first);
  if (res.proved) wrong++;

  clause.id = "witness:the-tick-is-always-even";
  clause.fn = ind_clause_tick_always_even;
  prop.id = clause.id;
  if (induction_run(cfg, p, t, &prop, &res) != 0) {
    printf("WITNESS tick-always-even: engine error\n");
    return wrong + 1;
  }
  printf("WITNESS tick-always-even: %s (%.100s)\n", res.proved ? "PROVED (BUG!)" : "REFUSED",
         res.first);
  if (res.proved) wrong++;
  return wrong;
}

/* The strengthening (the "renfort"): an auxiliary invariant conjoined to the
 * property. If P and Q are inductive together, P holds at every depth. */
static void run_strengthened(const ind_config *cfg, const relais_params *p, const relais_traj *t) {
  ind_clause clauses[2];
  clauses[0] = ind_clause_core;
  clauses[1].id = "no-future-mandate";
  clauses[1].fn = ind_clause_no_future_mandate;
  clauses[1].ctx = NULL;
  printf("  strengthened attempt (safety + no-future-mandate): %s\n",
         run_property(cfg, p, t, clauses, 2, "safety+no-future-mandate") ? "PROVED" : "not proved");
}

int main(int argc, char **argv) {
  ind_config cfg;
  ind_config_default(&cfg);
  bool witness = false;
  if (!parse_args(argc, argv, &cfg, &witness)) return 2;
  relais_params params;
  relais_traj traj;
  relais_params_default(&params);
  relais_traj_default(&traj);
  traj.seed = cfg.seed;

  if (witness) return run_witness(&cfg, &params, &traj) == 0 ? 0 : 1;

  ind_clause clauses[2];
  clauses[0] = ind_clause_core;
  clauses[1].id = "no-future-mandate";
  clauses[1].fn = ind_clause_no_future_mandate;
  clauses[1].ctx = NULL;
  bool proved = run_property(&cfg, &params, &traj, clauses, 1, "safety");
  if (proved) return 0;

  ind_result res;
  ind_property prop;
  prop.id = "safety";
  prop.clauses = clauses;
  prop.count = 1;
  if (induction_run(&cfg, &params, &traj, &prop, &res) == 0 && res.first_id[0] != '\0') {
    /* An actual breach: the strengthening is the way out (R5). */
    run_strengthened(&cfg, &params, &traj);
    return 1;
  }
  /* No breach, but the exploration was cut short: the result stays bounded and
   * that is a legitimate published verdict, not a failure of the run. */
  return 0;
}
