/* Relais — command-line driver of the exhaustive explorer (spec 7.3, level 1).
 *
 * Argument parsing and the verdict line only: the depth-first search, the
 * visited set, the brute-force / symbolic key choice and the counter-example
 * dump live in search.c, shared with the test drivers so there is exactly one
 * implementation of the exploration.
 *
 *   --symbolic  deduplicate on the equivalence class of symbolic.h instead of
 *               the whole world, and report classes reached vs raw states;
 *   --shrink    reduce the counter-example (shrink.c) before writing it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "explore.h"

static void usage(void) {
  fprintf(stderr,
          "usage: relais-explore [--budget 100000] [--depth 12] [--faults 1|2]\n"
          "       [--level socle|complet] [--params T=5,p=1,N=8]\n"
          "       [--trajectory \"kind=slalom amp=0.6 freq=0.5\"] [--out FILE]\n"
          "       [--seed S] [--symbolic] [--shrink] [--quiet]\n");
}


typedef struct {
  long budget;
  uint32_t depth;
  int faults;
  bool complete;
  bool quiet;
  bool symbolic;
  bool shrink;
  const char *out;
} explore_cfg;

/* Parse argv into cfg/params/traj. Returns false (and prints why) on bad input. */
static bool parse_args(int argc, char **argv, explore_cfg *cfg, relais_params *params,
                       relais_traj *traj) {
  char param_spec[128] = "";
  char traj_spec[192] = "";
  char why[128] = "";
  unsigned seed = 1;
  cfg->budget = 100000;
  cfg->depth = 12;
  cfg->faults = 1;
  cfg->complete = cfg->quiet = cfg->symbolic = cfg->shrink = false;
  cfg->out = NULL;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--budget") && i + 1 < argc) cfg->budget = atol(argv[++i]);
    else if (!strcmp(a, "--depth") && i + 1 < argc) cfg->depth = (uint32_t)atoi(argv[++i]);
    else if (!strcmp(a, "--faults") && i + 1 < argc) cfg->faults = atoi(argv[++i]);
    else if (!strcmp(a, "--level") && i + 1 < argc) cfg->complete = !strcmp(argv[++i], "complet");
    else if (!strcmp(a, "--seed") && i + 1 < argc) seed = (unsigned)atoi(argv[++i]);
    else if (!strcmp(a, "--out") && i + 1 < argc) cfg->out = argv[++i];
    else if (!strcmp(a, "--params") && i + 1 < argc)
      snprintf(param_spec, sizeof param_spec, "%s", argv[++i]);
    else if (!strcmp(a, "--trajectory") && i + 1 < argc)
      snprintf(traj_spec, sizeof traj_spec, "%s", argv[++i]);
    else if (!strcmp(a, "--quiet")) cfg->quiet = true;
    else if (!strcmp(a, "--symbolic")) cfg->symbolic = true;
    else if (!strcmp(a, "--shrink")) cfg->shrink = true;
    else { usage(); return false; }
  }
  relais_params_default(params);
  if (param_spec[0] && !relais_params_parse(params, param_spec, why, sizeof why)) {
    fprintf(stderr, "bad params: %s\n", why);
    return false;
  }
  relais_traj_default(traj);
  traj->seed = seed;
  if (traj_spec[0] && !relais_traj_parse(traj, traj_spec, why, sizeof why)) {
    fprintf(stderr, "bad trajectory: %s\n", why);
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  explore_cfg cfg;
  relais_params params;
  relais_traj traj;
  if (!parse_args(argc, argv, &cfg, &params, &traj)) return 2;
  explore_result res = explore_search(&params, &traj, cfg.complete, cfg.budget, cfg.depth,
                                      cfg.faults, cfg.symbolic, cfg.shrink, cfg.quiet, cfg.out);
  if (!res.ok) return 2;
  if (!res.failed) {
    if (cfg.symbolic)
      printf("EXPLORE-OK %ld states, %zu classes, deepest tick %u, 0 violation\n", res.states,
             res.classes, (unsigned)res.max_tick);
    else
      printf("EXPLORE-OK %ld states, deepest tick %u, 0 violation\n", res.states,
             (unsigned)res.max_tick);
  }
  return res.failed ? 1 : 0;
}
