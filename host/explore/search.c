/* Relais — the bounded depth-first search engine of host/explore (spec 7.3).
 *
 * Split out of the command-line explorer so the test drivers (test_symbolic,
 * test_shrink) link the very same search instead of a copy: one implementation
 * of the DFS, the visited set and the counter-example dump, shared by relais-
 * explore and by the tests. explore.c keeps only argument parsing and main().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "explore.h"
#include "shrink.h"
#include "symbolic.h"
#include "world.h"

#define MAX_EVENTS 64
#define MAX_DEPTH 192

typedef struct {
  explore_world world;
  explore_event events[MAX_EVENTS];
  explore_event taken; /* the move that produced this state */
  int count;
  int index;
} frame_t;

typedef struct {
  uint64_t *cells;
  size_t size;
  size_t mask;
  size_t used;
} visited_t;

static bool visited_init(visited_t *v, size_t want) {
  size_t size = 1024;
  while (size < want * 4) size *= 2;
  v->cells = calloc(size, sizeof *v->cells);
  if (v->cells == NULL) return false;
  v->size = size;
  v->mask = size - 1;
  v->used = 0;
  return true;
}

static bool visited_seen(visited_t *v, uint64_t hash) {
  uint64_t key = hash != 0 ? hash : 1;
  size_t i = (size_t)(key & v->mask);
  while (v->cells[i] != 0) {
    if (v->cells[i] == key) return true;
    i = (i + 1) & v->mask;
  }
  v->cells[i] = key;
  v->used++;
  return false;
}

/* The deduplication key: the whole world when brute-force, the equivalence
 * class when symbolic (see symbolic.h for why the quotient is safe). */
static uint64_t key_of(const explore_world *w, bool symbolic) {
  return symbolic ? sym_hash(w) : explore_world_hash(w);
}

static void print_move(const explore_world *w, const explore_event *e) {
  const char *what = explore_kind_name(e->kind);
  if (e->link >= 0)
    printf("  tick %5u  %-9s %s\n", (unsigned)w->tick, what,
           explore_link_name((explore_link)e->link));
  else if (e->node >= 0)
    printf("  tick %5u  %-9s machine %s\n", (unsigned)w->tick, what,
           e->node == 0 ? "A" : "B");
  else
    printf("  tick %5u  %s\n", (unsigned)w->tick, what);
}

/* The path from the root, as a replayable scenario (spec 7.3, item 6). Machine
 * faults go to "faults" and the simulator replays them as they are; channel
 * moves go to "channel", which the simulator approximates with --loss/--delay
 * and the explorer replays exactly. */
static void dump_path(const frame_t *stack, int sp, const char *path,
                      const explore_world *bad) {
  explore_event trace[SHRINK_TMP_MAX];
  int n = 0;
  for (int i = 1; i <= sp && n < SHRINK_TMP_MAX; i++) {
    print_move(&stack[i].world, &stack[i].taken);
    trace[n++] = stack[i].taken;
  }
  if (path != NULL) {
    FILE *f = fopen(path, "w");
    shrink_write_scenario(f, &bad->params, &bad->traj, bad->complete, trace, (size_t)n);
    if (f != NULL) fclose(f);
  }
}

/* Set up the root frame and register its deduplication key. */
static void search_init_root(frame_t *stack, visited_t *seen, const relais_params *params,
                             const relais_traj *traj, bool complete, int faults,
                             bool symbolic) {
  explore_world_init(&stack[0].world, params, traj, complete, complete);
  stack[0].taken.kind = EX_TICK;
  stack[0].taken.link = -1;
  stack[0].taken.node = -1;
  stack[0].count = explore_world_enumerate(&stack[0].world, stack[0].events, MAX_EVENTS, faults);
  stack[0].index = 0;
  visited_seen(seen, key_of(&stack[0].world, symbolic));
}

/* A breach was found at stack[ci]: capture the counter-example into res, print
 * the verdict, and (when asked) reduce it and write the minimal scenario. */
static void report_violation(explore_result *res, const frame_t *stack, int ci,
                             const relais_params *params, const relais_traj *traj, bool complete,
                             bool shrink, bool quiet, const char *out, long states) {
  res->trace_len = 0;
  for (int i = 1; i <= ci && res->trace_len < EXPLORE_TRACE_MAX; i++)
    res->trace[res->trace_len++] = stack[i].taken;
  printf("EXPLORE-ECHEC after %ld states, tick %u\n", states, (unsigned)stack[ci].world.tick);
  if (!quiet) {
    char report[RELAIS_INV_MSG_MAX * 2];
    relais_inv_report(&stack[ci].world.inv, report, sizeof report);
    printf("%s\n", report);
    printf("counter-example, from the root:\n");
    dump_path(stack, ci, shrink ? NULL : out, &stack[ci].world);
  }
  if (!shrink) {
    if (out != NULL && !quiet) printf("scenario written to %s\n", out);
    return;
  }
  explore_event reduced[SHRINK_TMP_MAX];
  size_t rn =
      shrink_trace(params, traj, complete, res->trace, res->trace_len, reduced, SHRINK_TMP_MAX);
  if (!quiet)
    printf("shrink: %zu events -> %zu events (minimal counter-example)\n", res->trace_len, rn);
  if (out != NULL) {
    FILE *fo = fopen(out, "w");
    shrink_write_scenario(fo, params, traj, complete, reduced, rn);
    if (fo != NULL) fclose(fo);
    if (!quiet) printf("minimal scenario written to %s\n", out);
  }
}

/* The closing summary line. */
static void report_summary(long states, size_t classes, long expansions, uint32_t max_tick,
                           time_t started, bool symbolic, bool quiet, int faults) {
  if (quiet) return;
  double secs = difftime(time(NULL), started);
  if (symbolic)
    printf("explored: %ld states, %zu classes, %ld expanded, deepest tick %u, %.1f s,"
           " faults <= %d\n",
           states, classes, expansions, (unsigned)max_tick, secs, faults);
  else
    printf("explored: %ld states, %ld expanded, deepest tick %u, %.1f s, faults <= %d\n", states,
           expansions, (unsigned)max_tick, secs, faults);
}

/* Periodic progress note while the search runs. */
static void search_progress(long states, long expansions, uint32_t max_tick, time_t started,
                            bool quiet) {
  if (!quiet && expansions % 10000 == 0)
    printf("  ... %ld states, %ld expanded, depth %u ticks, %.0f s\n", states, expansions,
           (unsigned)max_tick, difftime(time(NULL), started));
}

explore_result explore_search(const relais_params *params, const relais_traj *traj, bool complete,
                              long budget, uint32_t depth, int faults, bool symbolic, bool shrink,
                              bool quiet, const char *out) {
  explore_result res;
  memset(&res, 0, sizeof res);
  res.ok = true;
  frame_t *stack = calloc(MAX_DEPTH, sizeof *stack);
  visited_t seen;
  if (stack == NULL || !visited_init(&seen, (size_t)budget)) {
    fprintf(stderr, "explore: not enough memory\n");
    free(stack);
    res.ok = false;
    return res;
  }
  search_init_root(stack, &seen, params, traj, complete, faults, symbolic);
  long states = 1, expansions = 0;
  uint32_t max_tick = 0; int sp = 0; bool failed = false;
  time_t started = time(NULL);
  while (sp >= 0) {
    frame_t *f = &stack[sp];
    if (f->index >= f->count) { sp--; continue; }
    explore_event ev = f->events[f->index++];
    if (sp + 1 >= MAX_DEPTH) continue;
    frame_t *child = &stack[sp + 1];
    child->world = f->world;
    child->taken = ev;
    explore_world_apply(&child->world, &ev);
    states++;
    if (child->world.tick > max_tick) max_tick = child->world.tick;
    if (!explore_world_ok(&child->world)) {
      failed = true;
      report_violation(&res, stack, sp + 1, params, traj, complete, shrink, quiet, out, states);
      break;
    }
    if (visited_seen(&seen, key_of(&child->world, symbolic))) continue;
    if (child->world.tick >= depth || states >= budget) continue;
    child->count = explore_world_enumerate(&child->world, child->events, MAX_EVENTS, faults);
    child->index = 0;
    sp++;
    expansions++;
    search_progress(states, expansions, max_tick, started, quiet);
  }
  report_summary(states, seen.used, expansions, max_tick, started, symbolic, quiet, faults);
  res.states = states; res.classes = seen.used; res.expansions = expansions;
  res.max_tick = max_tick; res.failed = failed;
  free(seen.cells);
  free(stack);
  return res;
}
