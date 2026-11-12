/* Relais — the k-induction engine (spec 7.3, idea 2). See induction.h for the
 * inference rule and docs/INDUCTION.md for the proof it establishes.
 *
 * The engine has two halves:
 *
 *   1. the BASE CASE: the existing depth-first search of search.c, reused as
 *      it is, up to the depth k -- the bounded part, concrete states;
 *   2. the INDUCTION STEP: a search over the EQUIVALENCE CLASSES of the
 *      world (the "situations"), which is finite, and which is closed: the
 *      engine explores it until no new situation appears. Over a closed set
 *      of situations, "every successor of a P-situation is a P-situation" is
 *      a statement about ALL depths, not about the first k.
 *
 * Nothing here re-implements the safety invariants: the oracle is the core
 * library's own checker (relais_inv_*), reached through explore_world_ok.
 */
#include "induction.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "explore.h"
#include "shrink.h"

#define IND_AGE_MAX 255u
#define IND_FRAMES 8192
#define IND_EVENTS 64

/* ------------------------------------------------------------------ */
/* The equivalence class: the protocol situation of a world.          */
/* ------------------------------------------------------------------ */

typedef struct {
  uint8_t used; /* the cell is occupied (the hash set needs an empty marker) */
  uint8_t holder, certified_node, lamp, cause, misses;
  uint8_t certify_streak, shadow_misses, disagree_streak, flags;
  uint8_t present, ever_seen, request_seen, window_phase;
  uint8_t gap, testament_run, fallback_ticks, lamp_delay;
  uint8_t reintegration_ticks, mandate_changes, inv_flags, inv_violations;
  uint8_t last_holder, last_mandate_rel, last_fresh_age, source;
  uint8_t node[EXPLORE_NODES][8];
  uint8_t node_age[EXPLORE_NODES][4];
  uint8_t slot[EXPLORE_LINKS][EXPLORE_SLOTS][5];
  uint8_t cut, dead, degraded_ticks, windows_elapsed;
  int8_t channel_link;
  uint8_t burst, pending_mutations;
} ind_class;

/* The ages, coarsened to the horizon that matters (see docs/INDUCTION.md):
 * 0 = never, 1 = this very tick, 2 = inside the window, 3 = beyond it. The
 * horizon of the actuator's liveness window is LIVENESS_TICKS (core/src/
 * arbiter.c); beyond it a fact cannot change any verdict, so the exact age is
 * not kept. */
#define IND_LIVENESS 8u

static uint8_t age_bin(relais_tick now, relais_tick then, uint32_t window) {
  if (then == RELAIS_TICK_NONE || then == 0u) return 0u;
  if (now <= then) return 1u;
  uint64_t d = now - then;
  if (d <= (uint64_t)window) return 2u;
  return 3u;
}

/* The age of the last fresh order: the horizon of I8 is N+1 ticks, so the
 * exact age is kept up to N+2 and saturated beyond. */
static uint8_t age_fresh(relais_tick now, relais_tick then, uint32_t horizon) {
  if (then == 0u) return 0u;
  if (now <= then) return 1u;
  uint64_t d = now - then;
  if (d > (uint64_t)horizon) return (uint8_t)(horizon + 1u);
  return (uint8_t)(d + 1u);
}

/* The relation of a mandate number to the actuator's: the absolute number
 * grows with every handover, the relation does not (MAN-03). */
static uint8_t mandate_rel(relais_mandate a, relais_mandate b) {
  if (a == b) return 0u;
  return a < b ? 1u : 2u;
}

/* 0 when the computer runs, else 1 + the ticks left frozen. */
static uint8_t frozen_age(const explore_world *w, int i) {
  uint32_t fu = w->frozen_until[i];
  if (fu < w->tick) return 0u;
  uint32_t d = fu - w->tick;
  if (d >= IND_AGE_MAX) return (uint8_t)IND_AGE_MAX;
  return (uint8_t)(d + 1u);
}

static uint8_t node_mask(const bool *flags, int from, int to) {
  uint8_t m = 0u;
  for (int i = from; i <= to; i++)
    if (flags[i]) m = (uint8_t)(m | (uint8_t)(1u << (i - from)));
  return m;
}

static void project_arbiter(const explore_world *w, ind_class *c) {
  const relais_arb *a = &w->arb;
  c->holder = (uint8_t)a->holder;
  c->certified_node = (uint8_t)a->certified_node;
  c->lamp = (uint8_t)a->lamp;
  c->cause = (uint8_t)a->cause;
  c->misses = a->misses;
  c->certify_streak = a->certify_streak;
  c->shadow_misses = a->shadow_misses;
  c->disagree_streak = a->disagree_streak;
  c->flags = (uint8_t)((a->switch_pending ? 1u : 0u) | (a->certified ? 2u : 0u) |
                        (a->disagreement ? 4u : 0u) | (a->in_fallback ? 8u : 0u) |
                        (a->cession_pending ? 16u : 0u) | (a->stop_requested ? 32u : 0u) |
                        (a->certified_dissimilar ? 64u : 0u) | (a->widen_seen ? 128u : 0u));
  c->present = node_mask(a->present, RELAIS_NODE_A, RELAIS_NODE_ACTUATOR);
  c->ever_seen = node_mask(a->ever_seen, RELAIS_NODE_A, RELAIS_NODE_ACTUATOR);
  c->request_seen = a->request_seen;
  c->degraded_ticks = (uint8_t)(a->degraded_ticks > 255u ? 255u : a->degraded_ticks);
  uint32_t win = a->params.request_window ? a->params.request_window : 1u;
  c->window_phase = (uint8_t)(w->tick % win);
  uint32_t window_now = w->tick / win;
  uint32_t elapsed = window_now > a->request_window_tick ? window_now - a->request_window_tick : 0u;
  c->windows_elapsed = (uint8_t)(elapsed > 255u ? 255u : elapsed);
}

static void project_accumulator(const explore_world *w, ind_class *c) {
  const relais_inv *v = &w->inv;
  c->gap = (uint8_t)(v->gap > 255u ? 255u : v->gap);
  c->testament_run = (uint8_t)(v->testament_run > 255u ? 255u : v->testament_run);
  c->fallback_ticks = (uint8_t)(v->fallback_ticks > 255u ? 255u : v->fallback_ticks);
  c->lamp_delay = (uint8_t)(v->lamp_delay > 255u ? 255u : v->lamp_delay);
  c->reintegration_ticks = (uint8_t)(v->reintegration_ticks > 255u ? 255u : v->reintegration_ticks);
  c->mandate_changes = (uint8_t)v->mandate_changes_this_tick;
  c->inv_flags = (uint8_t)((v->fault_free_run ? 1u : 0u) | (v->lossy_channel ? 2u : 0u) |
                            (v->single_fault ? 4u : 0u) | (v->expect_certified_second ? 8u : 0u) |
                            (v->in_fallback ? 16u : 0u) | (v->mandate_vacant ? 32u : 0u) |
                            (v->cession_since_change ? 64u : 0u) | (v->disagreement_seen ? 128u : 0u));
  c->inv_violations = v->violations > 0u ? 1u : 0u;
  c->last_holder = (uint8_t)v->last_holder;
  c->last_mandate_rel = mandate_rel(v->last_mandate, w->arb.mandate);
  c->last_fresh_age = age_fresh(w->tick, v->last_fresh_tick, w->params.testament_len + 2u);
  c->source = (uint8_t)w->source;
}

static void project_node(const explore_world *w, int i, ind_class *c) {
  const relais_node_sm *n = &w->node[i];
  uint8_t *d = c->node[i];
  d[0] = (uint8_t)n->role;
  d[1] = (uint8_t)n->seen_holder;
  d[2] = mandate_rel(n->mandate, w->arb.mandate);
  d[3] = mandate_rel(n->seen_mandate, w->arb.mandate);
  d[4] = n->replica_age;
  d[5] = n->bail_ticks;
  d[6] = n->link_lost_ticks;
  d[7] = (uint8_t)((n->ready ? 1u : 0u) | (n->link_lost ? 2u : 0u) | (n->deposed ? 4u : 0u) |
                   (n->without_actuator ? 8u : 0u));
  c->node_age[i][0] = age_bin(w->tick, n->last_statut, 1u);
  c->node_age[i][1] = age_bin(w->tick, n->last_etat, 1u);
  c->node_age[i][2] = age_bin(w->tick, w->arb.last_heard[n->self], IND_LIVENESS);
  c->node_age[i][3] = frozen_age(w, i);
}

static void project_slot(const explore_world *w, int l, int s, ind_class *c) {
  const explore_slot *sl = &w->slot[l][s];
  uint8_t *d = c->slot[l][s];
  memset(d, 0, sizeof c->slot[l][s]);
  if (!sl->busy) return;
  d[0] = (uint8_t)(1u + (unsigned)sl->msg.type);
  d[1] = (uint8_t)sl->msg.sender;
  d[2] = mandate_rel(sl->msg.mandate, w->arb.mandate);
  d[3] = age_bin(w->tick, sl->msg.tick, 1u);
  d[4] = age_bin(w->tick, sl->deliver_at, IND_LIVENESS);
}

/* The situation of a world: the protocol facts the safety invariants read,
 * with the absolute time and the absolute mandate numbers replaced by ages
 * and relations. */
static void project_world(const explore_world *w, ind_class *c) {
  memset(c, 0, sizeof *c);
  project_arbiter(w, c);
  project_accumulator(w, c);
  for (int i = 0; i < EXPLORE_NODES; i++) project_node(w, i, c);
  for (int l = 0; l < EXPLORE_LINKS; l++)
    for (int s = 0; s < EXPLORE_SLOTS; s++) project_slot(w, l, s, c);
  for (int l = 0; l < EXPLORE_LINKS; l++)
    if (w->cut[l]) c->cut = (uint8_t)(c->cut | (uint8_t)(1u << l));
  for (int i = 0; i < EXPLORE_NODES; i++)
    if (w->dead[i]) c->dead = (uint8_t)(c->dead | (uint8_t)(1u << i));
  c->channel_link = w->channel_link;
  c->burst = w->burst_this_tick;
  c->pending_mutations = w->pending_mutations;
}

/* ------------------------------------------------------------------ */
/* The set of situations (a hash set, no allocation in the core).     */
/* ------------------------------------------------------------------ */

typedef struct {
  ind_class *cells;
  size_t size;
  size_t mask;
  size_t used;
} ind_set;

static uint64_t class_hash(const ind_class *c) {
  const uint64_t prime = 0x100000001b3ull;
  uint64_t h = 0xcbf29ce484222325ull;
  const unsigned char *p = (const unsigned char *)c;
  for (size_t i = 0; i < sizeof *c; i++) {
    h ^= p[i];
    h *= prime;
  }
  return h;
}

static bool set_init(ind_set *s, size_t want) {
  size_t size = 1024;
  while (size < want * 2u) size *= 2u;
  s->cells = calloc(size, sizeof *s->cells);
  if (s->cells == NULL) return false;
  s->size = size;
  s->mask = size - 1u;
  s->used = 0;
  return true;
}

static void set_free(ind_set *s) {
  free(s->cells);
  s->cells = NULL;
}

/* Insert a situation; returns true when it was not there yet. */
static bool set_add(ind_set *s, const ind_class *k) {
  ind_class entry = *k;
  entry.used = 1u;
  uint64_t key = class_hash(&entry);
  if (key == 0u) key = 1u;
  size_t i = (size_t)(key & s->mask);
  while (s->cells[i].used != 0u) {
    if (memcmp(&s->cells[i], &entry, sizeof entry) == 0) return false;
    i = (i + 1u) & s->mask;
  }
  s->cells[i] = entry;
  s->used++;
  return true;
}

/* ------------------------------------------------------------------ */
/* The induction step: the search over the situations.               */
/* ------------------------------------------------------------------ */

typedef struct {
  explore_world world;
  explore_event events[IND_EVENTS];
  explore_event taken; /* the move that produced this frame */
  int count;
  int index;
} ind_frame;

/* The clause of the property that fails, or -1 when the property holds. */
static int failing_clause(const ind_property *prop, const explore_world *w) {
  for (size_t i = 0; i < prop->count; i++)
    if (!prop->clauses[i].fn(w, prop->clauses[i].ctx)) return (int)i;
  return -1;
}

static void copy_trace(ind_result *res, const ind_frame *stack, int sp) {
  res->trace_len = 0;
  for (int i = 1; i <= sp && res->trace_len < IND_TRACE_MAX; i++) {
    res->trace[res->trace_len++] = stack[i].taken;
  }
}

/* The first word of a message: the invariant id ("I10", "DET-03", ...). */
static void first_token(const char *s, char *id, size_t cap) {
  id[0] = '\0';
  size_t i = 0;
  while (s[i] != '\0' && s[i] != ' ' && i + 1u < cap) {
    id[i] = s[i];
    i++;
  }
  id[i] = '\0';
}

static void record_failure(ind_result *res, const explore_world *w, const ind_property *prop,
                           int clause) {
  const relais_inv *v = &w->inv;
  if (v->first[0] != '\0') {
    snprintf(res->first, sizeof res->first, "%s", v->first);
    first_token(v->first, res->first_id, (size_t)IND_ID_MAX);
    return;
  }
  /* The core checker is happy: the clause that failed is one of ours. */
  const char *cid = clause >= 0 ? prop->clauses[clause].id : "?";
  snprintf(res->first, sizeof res->first, "clause '%.60s' does not hold at tick %u", cid,
           (unsigned)w->tick);
  first_token(cid, res->first_id, (size_t)IND_ID_MAX);
}

/* The quotient search. Returns false on an internal error. */
static bool explore_quotient(const ind_config *cfg, const relais_params *p,
                             const relais_traj *t, const ind_property *prop,
                             ind_result *res, ind_set *set) {
  ind_frame *stack = calloc(IND_FRAMES, sizeof *stack);
  if (stack == NULL) return false;
  explore_world_init(&stack[0].world, p, t, cfg->complete, cfg->complete);
  ind_class key;
  project_world(&stack[0].world, &key);
  set_add(set, &key);
  res->classes = 1;
  stack[0].count = explore_world_enumerate(&stack[0].world, stack[0].events, IND_EVENTS, cfg->faults);
  stack[0].index = 0;
  int sp = 0;
  bool truncated = false;
  while (sp >= 0) {
    ind_frame *f = &stack[sp];
    if (f->index >= f->count) { sp--; continue; }
    explore_event ev = f->events[f->index++];
    if (sp + 1 >= IND_FRAMES) { truncated = true; continue; }
    ind_frame *child = &stack[sp + 1];
    child->world = f->world;
    child->taken = ev;
    explore_world_apply(&child->world, &ev);
    res->class_states++;
    res->class_transitions++;
    if (child->world.tick > res->max_tick) res->max_tick = child->world.tick;
    int bad = failing_clause(prop, &child->world);
    if (bad >= 0) {
      record_failure(res, &child->world, prop, bad);
      copy_trace(res, stack, sp + 1);
      free(stack);
      return true;
    }
    project_world(&child->world, &key);
    if (!set_add(set, &key)) continue;
    res->classes++;
    if (res->classes > cfg->class_budget) break;
    if (child->world.tick > res->k_close) res->k_close = child->world.tick;
    child->count = explore_world_enumerate(&child->world, child->events, IND_EVENTS, cfg->faults);
    child->index = 0;
    sp++;
  }
  free(stack);
  res->closed = (res->classes <= cfg->class_budget) && !truncated;
  return true;
}

/* ------------------------------------------------------------------ */
/* The run.                                                            */
/* ------------------------------------------------------------------ */

void ind_config_default(ind_config *cfg) {
  cfg->k = 12u;
  cfg->budget = 100000;
  cfg->class_budget = 60000;
  cfg->faults = 1;
  cfg->complete = true;
  cfg->seed = 1u;
}

/* Pull the invariant id ("I10", "DET-03", ...) out of a violation report. */
static void parse_first_id(const char *report, char *id, size_t cap) {
  id[0] = '\0';
  const char *at = strstr(report, "first: ");
  if (at == NULL) return;
  first_token(at + 7, id, cap);
}

int induction_run(const ind_config *cfg, const relais_params *p, const relais_traj *t,
                  const ind_property *prop, ind_result *out) {
  if (cfg == NULL || p == NULL || t == NULL || prop == NULL || out == NULL) return 1;
  if (cfg->class_budget <= 0 || cfg->budget <= 0 || prop->count == 0) return 1;
  memset(out, 0, sizeof *out);
  out->ok = false;
  time_t started = time(NULL);

  /* (R1) BASE CASE: the existing depth-first search, reused as it is. */
  explore_result base = explore_search(p, t, cfg->complete, cfg->budget, cfg->k, cfg->faults,
                                       false, false, true, NULL);
  if (!base.ok) return 1;
  out->k_base = cfg->k;
  out->base_states = base.states;
  out->base_transitions = base.expansions;
  if (base.failed) {
    /* The bounded exploration already refutes the property: replay its
     * counter-example to name the invariant it breaks. */
    out->base_failed = true;
    char report[RELAIS_INV_MSG_MAX * 2];
    if (shrink_violates(p, t, cfg->complete, base.trace, base.trace_len, report,
                        sizeof report))
      snprintf(out->first, sizeof out->first, "%s", report);
    else
      snprintf(out->first, sizeof out->first, "violation in the bounded base case");
    parse_first_id(out->first, out->first_id, (size_t)IND_ID_MAX);
    out->trace_len = base.trace_len < IND_TRACE_MAX ? base.trace_len : IND_TRACE_MAX;
    memcpy(out->trace, base.trace, out->trace_len * sizeof out->trace[0]);
    out->ok = true;
    out->proved = false;
    out->seconds = difftime(time(NULL), started);
    return 0;
  }

  /* (R2)+(R3) INDUCTION STEP over the closed set of situations. */
  ind_set set;
  if (!set_init(&set, (size_t)(cfg->class_budget > 0 ? cfg->class_budget : 1))) return 1;
  bool ok = explore_quotient(cfg, p, t, prop, out, &set);
  set_free(&set);
  if (!ok) return 1;
  out->ok = true;
  out->proved = out->closed && out->first_id[0] == '\0';
  if (!out->proved && out->first_id[0] == '\0')
    snprintf(out->first, sizeof out->first,
             "no violation in the %ld concrete states examined, but the set of situations did not "
             "close within the budget: the result stays bounded",
             out->class_states);
  out->seconds = difftime(time(NULL), started);
  return 0;
}

/* ------------------------------------------------------------------ */
/* The clauses the engine ships with.                                  */
/* ------------------------------------------------------------------ */

uint64_t induction_class_hash(const explore_world *w) {
  ind_class c;
  project_world(w, &c);
  return class_hash(&c);
}

static bool clause_core(const explore_world *w, void *ctx) {
  (void)ctx;
  return explore_world_ok(w);
}

const ind_clause ind_clause_core = {"core", clause_core, NULL};

/* A deliberately FALSE property: the actuator does lose its holder when the
 * holder dies (MAN-10 hands the mandate over). The engine must refuse it. */
bool ind_clause_holder_never_lost(const explore_world *w, void *ctx) {
  (void)ctx;
  if (w->arb.holder == RELAIS_NODE_NONE) return true; /* not appointed yet */
  int i = w->arb.holder == RELAIS_NODE_A ? 0 : 1;
  return !w->dead[i];
}

/* A deliberately FALSE property: the tick number is not always even. */
bool ind_clause_tick_always_even(const explore_world *w, void *ctx) {
  (void)ctx;
  return (w->tick % 2u) == 0u;
}

/* A strengthening (a "renfort"): an auxiliary invariant that holds on every
 * reachable state and constrains the situation, so that P and Q together are
 * inductive. Q implies nothing about P, but P and Q inductive implies P at
 * every depth. This one is the actuator's authority over the mandate number
 * (MAN-02, SEC-04): no computer ever believes a mandate the actuator has not
 * handed out yet. */
bool ind_clause_no_future_mandate(const explore_world *w, void *ctx) {
  (void)ctx;
  for (int i = 0; i < EXPLORE_NODES; i++)
    if (w->node[i].mandate > w->arb.mandate) return false;
  return true;
}
