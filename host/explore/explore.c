/* Relais — exhaustive exploration of the core (spec 7.3, level 1).
 *
 * Depth-first, iterative, bounded: at every state the adversary may deliver,
 * drop, delay or duplicate a frame in flight, kill, freeze, thaw or restart a
 * machine, cut or restore any of the six half-links, or let the tick beat.
 * Every resulting state is hashed and never expanded twice. The invariants are
 * the very ones the simulator and the verifier use, so what is explored is what
 * runs (spec 4.10).
 *
 * The first violation stops the search and is written out as a scenario the
 * seeded simulator can replay.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void usage(void) {
  fprintf(stderr,
          "usage: relais-explore [--budget 100000] [--depth 12] [--faults 1|2]\n"
          "       [--level socle|complet] [--params T=5,p=1,N=8]\n"
          "       [--trajectory \"kind=slalom amp=0.6 freq=0.5\"] [--out FILE]\n"
          "       [--seed S] [--quiet]\n");
}

/* One half-link, as the scenario format names it (CONTRACT.md). */
static void link_names(int link, const char **name, const char **direction) {
  static const char *names[EXPLORE_LINKS] = {"A-actuator", "B-actuator", "A-actuator",
                                             "B-actuator", "A-B", "A-B"};
  static const char *dirs[EXPLORE_LINKS] = {"forward", "forward", "reverse",
                                            "reverse", "forward", "reverse"};
  if (link < 0 || link >= EXPLORE_LINKS) {
    *name = "A-B";
    *direction = "both";
    return;
  }
  *name = names[link];
  *direction = dirs[link];
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
  FILE *f = path != NULL ? fopen(path, "w") : NULL;
  if (f != NULL) {
    fprintf(f, "{\"id\": 9001, \"name\": \"counterexample\", \"ticks\": %u,\n",
            (unsigned)bad->tick + 4);
    fprintf(f, " \"params\": {\"T\": %u, \"p\": %u, \"N\": %u},\n",
            (unsigned)bad->params.period_ms, (unsigned)bad->params.patience,
            (unsigned)bad->params.testament_len);
    fprintf(f, " \"trajectory\": {\"kind\": \"slalom\", \"amp\": %g, \"freq\": %g},\n",
            (double)bad->traj.amp, (double)bad->traj.freq);
    fprintf(f, " \"faults\": [");
  }
  int first_fault = 1;
  int first_channel = 1;
  char channel[2048];
  size_t at = 0;
  for (int i = 0; i <= sp; i++) {
    const explore_event *e = &stack[i].taken;
    print_move(&stack[i].world, e);
    if (f == NULL) continue;
    uint32_t when = stack[i].world.tick + 1;
    if (e->kind == EX_KILL || e->kind == EX_FREEZE1 || e->kind == EX_FREEZE3) {
      const char *type = e->kind == EX_KILL ? "death" : "freeze";
      at += (size_t)fprintf(f, "%s\n   {\"at_tick\": %u, \"type\": \"%s\", \"target\": \"%s\"%s}",
                            first_fault ? "" : ",", (unsigned)when, type,
                            e->node == 0 ? "A" : "B",
                            e->kind == EX_FREEZE3 ? ", \"ticks\": 3" : "");
      first_fault = 0;
    } else if (e->kind == EX_CUT) {
      const char *name;
      const char *dir;
      link_names(e->link, &name, &dir);
      at += (size_t)fprintf(f,
                            "%s\n   {\"at_tick\": %u, \"type\": \"cut\", \"link\": \"%s\","
                            " \"direction\": \"%s\"}",
                            first_fault ? "" : ",", (unsigned)when, name, dir);
      first_fault = 0;
    } else if (e->kind == EX_DROP || e->kind == EX_DELAY || e->kind == EX_DUP) {
      const char *name;
      const char *dir;
      link_names(e->link, &name, &dir);
      if (at + 160 < sizeof channel)
        at += (size_t)snprintf(channel + at, sizeof channel - at,
                               "%s\n   {\"at_tick\": %u, \"move\": \"%s\", \"link\": \"%s\","
                               " \"direction\": \"%s\"}",
                               first_channel ? "" : ",", (unsigned)when,
                               explore_kind_name(e->kind), name, dir);
      first_channel = 0;
    }
  }
  (void)at;
  if (f != NULL) {
    fprintf(f, "\n ],\n \"channel\": [%s\n ],\n", first_channel ? "" : channel);
    fprintf(f, " \"note\": \"written by relais-explore\"\n}\n");
    fclose(f);
  }
}

int main(int argc, char **argv) {
  long budget = 100000;
  uint32_t depth = 12;
  int faults = 1;
  bool complete = false;
  bool quiet = false;
  unsigned seed = 1;
  const char *out = NULL;
  char param_spec[128] = "";
  char traj_spec[192] = "";
  char why[128] = "";

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--budget") && i + 1 < argc) budget = atol(argv[++i]);
    else if (!strcmp(a, "--depth") && i + 1 < argc) depth = (uint32_t)atoi(argv[++i]);
    else if (!strcmp(a, "--faults") && i + 1 < argc) faults = atoi(argv[++i]);
    else if (!strcmp(a, "--level") && i + 1 < argc) complete = !strcmp(argv[++i], "complet");
    else if (!strcmp(a, "--seed") && i + 1 < argc) seed = (unsigned)atoi(argv[++i]);
    else if (!strcmp(a, "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(a, "--params") && i + 1 < argc)
      snprintf(param_spec, sizeof param_spec, "%s", argv[++i]);
    else if (!strcmp(a, "--trajectory") && i + 1 < argc)
      snprintf(traj_spec, sizeof traj_spec, "%s", argv[++i]);
    else if (!strcmp(a, "--quiet")) quiet = true;
    else { usage(); return 2; }
  }

  relais_params params;
  relais_params_default(&params);
  if (param_spec[0] && !relais_params_parse(&params, param_spec, why, sizeof why)) {
    fprintf(stderr, "bad params: %s\n", why);
    return 2;
  }
  relais_traj traj;
  relais_traj_default(&traj);
  traj.seed = seed;
  if (traj_spec[0] && !relais_traj_parse(&traj, traj_spec, why, sizeof why)) {
    fprintf(stderr, "bad trajectory: %s\n", why);
    return 2;
  }

  frame_t *stack = calloc(MAX_DEPTH, sizeof *stack);
  visited_t seen;
  if (stack == NULL || !visited_init(&seen, (size_t)budget)) {
    fprintf(stderr, "explore: not enough memory\n");
    return 2;
  }

  explore_world_init(&stack[0].world, &params, &traj, complete, complete);
  stack[0].taken.kind = EX_TICK;
  stack[0].taken.link = -1;
  stack[0].taken.node = -1;
  stack[0].count = explore_world_enumerate(&stack[0].world, stack[0].events, MAX_EVENTS, faults);
  stack[0].index = 0;
  visited_seen(&seen, explore_world_hash(&stack[0].world));

  long states = 1;
  long expansions = 0;
  uint32_t max_tick = 0;
  int sp = 0;
  bool failed = false;
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
      printf("EXPLORE-ECHEC after %ld states, tick %u\n", states,
             (unsigned)child->world.tick);
      char report[RELAIS_INV_MSG_MAX * 2];
      relais_inv_report(&child->world.inv, report, sizeof report);
      printf("%s\n", report);
      printf("counter-example, from the root:\n");
      dump_path(stack, sp + 1, out, &child->world);
      if (out != NULL) printf("scenario written to %s\n", out);
      break;
    }
    if (visited_seen(&seen, explore_world_hash(&child->world))) continue;
    if (child->world.tick >= depth || states >= budget) continue;
    child->count = explore_world_enumerate(&child->world, child->events, MAX_EVENTS, faults);
    child->index = 0;
    sp++;
    expansions++;
    if (!quiet && expansions % 10000 == 0)
      printf("  ... %ld states, %ld expanded, depth %u ticks, %.0f s\n", states,
             expansions, (unsigned)max_tick, difftime(time(NULL), started));
  }

  if (!quiet) {
    double secs = difftime(time(NULL), started);
    printf("explored: %ld states, %ld expanded, deepest tick %u, %.1f s, faults <= %d\n",
           states, expansions, (unsigned)max_tick, secs, faults);
  }
  free(seen.cells);
  free(stack);
  if (failed) return 1;
  printf("EXPLORE-OK %ld states, deepest tick %u, 0 violation\n", states, (unsigned)max_tick);
  return 0;
}
