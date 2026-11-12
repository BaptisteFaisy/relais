/* Relais — the bounded depth-first search of host/explore, exposed so the test
 * drivers can run it directly (brute-force or symbolic) without shelling out.
 * The command-line explorer (explore.c) and test_symbolic share this one
 * implementation: no duplicated search loop. */
#ifndef RELAIS_EXPLORE_EXPLORE_H
#define RELAIS_EXPLORE_EXPLORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "world.h"

#define EXPLORE_TRACE_MAX 256

typedef struct {
  long states;       /* concrete worlds generated */
  size_t classes;    /* distinct deduplication keys inserted (== states if brute) */
  long expansions;   /* worlds actually expanded */
  uint32_t max_tick; /* deepest tick reached */
  bool failed;       /* a safety invariant was breached */
  bool ok;           /* false only on an internal error (out of memory) */
  explore_event trace[EXPLORE_TRACE_MAX]; /* the counter-example path, when failed */
  size_t trace_len;
} explore_result;

/* Run the search. `symbolic` selects the quotient key of symbolic.h; `shrink`
 * reduces the counter-example before writing it; `out` (may be NULL) receives
 * the scenario. Progress and the verdict are printed unless `quiet`. */
explore_result explore_search(const relais_params *params, const relais_traj *traj,
                              bool complete, long budget, uint32_t depth, int faults,
                              bool symbolic, bool shrink, bool quiet, const char *out);

#endif /* RELAIS_EXPLORE_EXPLORE_H */
