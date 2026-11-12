/* Relais — delta-debugging of a counter-example trace. See shrink.h. */
#include "shrink.h"

#include <string.h>

bool shrink_violates(const relais_params *p, const relais_traj *t, bool complete,
                     const explore_event *ev, size_t n, char *report, size_t cap) {
  explore_world w;
  explore_world_init(&w, p, t, complete, complete);
  for (size_t i = 0; i < n; i++) {
    explore_world_apply(&w, &ev[i]);
    if (!explore_world_ok(&w)) {
      if (report != NULL && cap > 0) relais_inv_report(&w.inv, report, cap);
      return true;
    }
  }
  return false;
}

/* Copy work[0..n) minus the half-open range [lo,hi) into dst. */
static size_t drop_range(const explore_event *work, size_t n, size_t lo, size_t hi,
                         explore_event *dst) {
  size_t k = 0;
  for (size_t i = 0; i < n; i++)
    if (i < lo || i >= hi) dst[k++] = work[i];
  return k;
}

/* Phase 1: delete one event at a time, left to right, keeping every deletion
 * that leaves the breach in place. The cheapest pass, and the one that removes
 * the most noise (frames the search tried before finding the real witness). */
static size_t shrink_singles(const relais_params *p, const relais_traj *t, bool complete,
                             explore_event *work, size_t n) {
  explore_event tmp[SHRINK_TMP_MAX];
  size_t i = 0;
  while (i < n) {
    size_t m = drop_range(work, n, i, i + 1, tmp);
    if (m > SHRINK_TMP_MAX) { i++; continue; }
    if (shrink_violates(p, t, complete, tmp, m, NULL, 0)) {
      memcpy(work, tmp, m * sizeof *work);
      n = m; /* event i is gone; retry the same index on the shorter trace */
    } else {
      i++;
    }
  }
  return n;
}

/* Phase 2: ddmin. Try to delete a whole block; if that keeps the breach, restart
 * at a finer granularity, otherwise halve the block size. Stops when a block of
 * one can no longer be removed anywhere. */
static size_t shrink_blocks(const relais_params *p, const relais_traj *t, bool complete,
                            explore_event *work, size_t n) {
  explore_event tmp[SHRINK_TMP_MAX];
  size_t gran = 2;
  while (gran <= n && n > 1) {
    size_t block = (n + gran - 1) / gran;
    bool removed = false;
    for (size_t lo = 0; lo < n && !removed; lo += block) {
      size_t hi = lo + block < n ? lo + block : n;
      size_t m = drop_range(work, n, lo, hi, tmp);
      if (m > SHRINK_TMP_MAX) continue;
      if (shrink_violates(p, t, complete, tmp, m, NULL, 0)) {
        memcpy(work, tmp, m * sizeof *work);
        n = m;
        gran = 2;
        removed = true;
      }
    }
    if (!removed) gran *= 2;
  }
  return n;
}

size_t shrink_trace(const relais_params *p, const relais_traj *t, bool complete,
                    const explore_event *ev, size_t n, explore_event *out, size_t cap) {
  explore_event work[SHRINK_TMP_MAX];
  if (n > SHRINK_TMP_MAX) n = SHRINK_TMP_MAX;
  memcpy(work, ev, n * sizeof *work);
  n = shrink_singles(p, t, complete, work, n);
  n = shrink_blocks(p, t, complete, work, n);
  size_t keep = n < cap ? n : cap;
  memcpy(out, work, keep * sizeof *out);
  return keep;
}

/* One machine fault or cut, written straight into the "faults" array. */
static void scenario_fault(FILE *f, const explore_event *e, uint32_t when, int *first) {
  if (e->kind == EX_KILL || e->kind == EX_FREEZE1 || e->kind == EX_FREEZE3) {
    const char *type = e->kind == EX_KILL ? "death" : "freeze";
    fprintf(f, "%s\n   {\"at_tick\": %u, \"type\": \"%s\", \"target\": \"%s\"%s}",
            *first ? "" : ",", (unsigned)when, type, e->node == 0 ? "A" : "B",
            e->kind == EX_FREEZE3 ? ", \"ticks\": 3" : "");
    *first = 0;
  } else if (e->kind == EX_CUT) {
    const char *name;
    const char *dir;
    explore_link_scenario(e->link, &name, &dir);
    fprintf(f,
            "%s\n   {\"at_tick\": %u, \"type\": \"cut\", \"link\": \"%s\","
            " \"direction\": \"%s\"}",
            *first ? "" : ",", (unsigned)when, name, dir);
    *first = 0;
  }
}

/* One channel move (drop/delay/duplicate), accumulated into the caller's buffer
 * because the scenario keeps them in a separate "channel" array. */
static void scenario_channel(const explore_event *e, uint32_t when, char *buf, size_t cap,
                             size_t *at, int *first) {
  if (e->kind != EX_DROP && e->kind != EX_DELAY && e->kind != EX_DUP) return;
  const char *name;
  const char *dir;
  explore_link_scenario(e->link, &name, &dir);
  if (*at + 160 < cap)
    *at += (size_t)snprintf(buf + *at, cap - *at,
                            "%s\n   {\"at_tick\": %u, \"move\": \"%s\", \"link\": \"%s\","
                            " \"direction\": \"%s\"}",
                            *first ? "" : ",", (unsigned)when, explore_kind_name(e->kind),
                            name, dir);
  *first = 0;
}

void shrink_write_scenario(FILE *f, const relais_params *p, const relais_traj *t,
                           bool complete, const explore_event *ev, size_t n) {
  if (f == NULL) return;
  explore_world w;
  explore_world_init(&w, p, t, complete, complete);
  uint32_t when[SHRINK_TMP_MAX];
  size_t m = n < SHRINK_TMP_MAX ? n : SHRINK_TMP_MAX;
  for (size_t i = 0; i < m; i++) {
    explore_world_apply(&w, &ev[i]);
    when[i] = w.tick + 1; /* the scenario names the tick the move targets */
  }
  fprintf(f, "{\"id\": 9001, \"name\": \"counterexample\", \"ticks\": %u,\n",
          (unsigned)(w.tick + 4));
  fprintf(f, " \"params\": {\"T\": %u, \"p\": %u, \"N\": %u},\n", (unsigned)p->period_ms,
          (unsigned)p->patience, (unsigned)p->testament_len);
  fprintf(f, " \"trajectory\": {\"kind\": \"slalom\", \"amp\": %g, \"freq\": %g},\n",
          (double)t->amp, (double)t->freq);
  fprintf(f, " \"faults\": [");
  int first_fault = 1;
  int first_channel = 1;
  char channel[2048];
  size_t at = 0;
  for (size_t i = 0; i < m; i++) {
    scenario_fault(f, &ev[i], when[i], &first_fault);
    scenario_channel(&ev[i], when[i], channel, sizeof channel, &at, &first_channel);
  }
  fprintf(f, "\n ],\n \"channel\": [%s\n ],\n", first_channel ? "" : channel);
  fprintf(f, " \"note\": \"minimal counter-example, written by relais-explore\"\n}\n");
}
