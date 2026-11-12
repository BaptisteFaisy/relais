/* Relais — k-induction over the world of host/explore (spec 7.3, idea 2).
 *
 * The exhaustive explorer proves the safety invariants up to a depth: its
 * depth-first search stops at a tick bound, so what it establishes is "no
 * violation in the first k ticks". This module removes the bound.
 *
 * THE INFERENCE RULE (the whole proof is in docs/INDUCTION.md):
 *
 *   (R1) BASE   every state reachable in at most k moves satisfies P;
 *   (R2) STEP   every successor of a P-state satisfies P, checked over the
 *               WHOLE set of reachable protocol situations, not only the
 *               first k levels;
 *   (R3) CLOSE  that set is closed: no successor leaves it;
 *   (R4) SOUND  P depends only on the situation (the equivalence class),
 *               not on what the class forgets (the floats, the plant, the
 *               absolute tick).
 *
 *   R1 + R2 + R3 + R4  =>  P holds on every reachable state, at every depth,
 *   for every k: the bound of the depth-first search is gone.
 *
 * The "situation" is the equivalence class of induction.c: the protocol facts
 * the safety invariants read (the actuator's evidence, the two computers'
 * discrete state, the liveness of the frames in flight, the fault state),
 * with every absolute tick replaced by an AGE and every mandate number
 * replaced by its RELATION to the actuator's mandate. The analysis of what may
 * be forgotten is the one of symbolic.h, extended with the tick normalisation.
 *
 * A property is a conjunction of clauses, so a strengthening ("renfort") is
 * one more clause: the engine proves P and Q together, and Q then implies P.
 */
#ifndef RELAIS_EXPLORE_INDUCTION_H
#define RELAIS_EXPLORE_INDUCTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "relais/invariants.h"
#include "relais/params.h"
#include "world.h"

#define IND_TRACE_MAX 256
#define IND_ID_MAX 16

/* One clause of a property: a predicate over a world. */
typedef bool (*ind_clause_fn)(const explore_world *w, void *ctx);

typedef struct {
  const char *id;   /* "core", "holder-alive", ... */
  ind_clause_fn fn;
  void *ctx;
} ind_clause;

/* The property to prove: the conjunction of its clauses. */
typedef struct {
  const char *id;
  const ind_clause *clauses;
  size_t count;
} ind_property;

/* The verdict for one invariant of the specification (spec 7.2). */
typedef struct {
  const char *id;    /* "I1" ... "DET-03" */
  bool inductive;    /* proved for every depth */
  bool exercised;    /* the explorer can reach this check */
  uint32_t k_min;    /* the k at which the induction closes for it */
  char note[128];   /* why, or the counter-example */
} ind_verdict;

/* How the run is parameterised. */
typedef struct {
  uint32_t k;          /* depth in ticks of the bounded base case */
  long budget;         /* states budget of the base case */
  long class_budget;   /* situations budget of the quotient */
  int faults;          /* adversary fault budget (single-fault hypothesis) */
  bool complete;       /* level C: a certified second is expected */
  unsigned seed;
} ind_config;

/* The outcome of one induction run. */
typedef struct {
  bool ok;            /* the engine ran to the end */
  bool proved;        /* base + step hold: P holds at every depth */
  bool closed;        /* the quotient reached its fixpoint */
  bool base_failed;   /* the bounded base case found a violation */
  uint32_t k_close;   /* depth at which the quotient closed */
  uint32_t k_base;    /* depth of the bounded base case */
  long base_states;   /* concrete states of the base case */
  long base_transitions;
  long classes;       /* distinct situations of the quotient */
  long class_states;  /* concrete states generated while building it */
  long class_transitions;
  uint32_t max_tick;  /* deepest tick reached */
  double seconds;
  char first[RELAIS_INV_MSG_MAX * 2]; /* the first violation, when not proved */
  char first_id[IND_ID_MAX];          /* its invariant id */
  explore_event trace[IND_TRACE_MAX];  /* the counter-example path */
  size_t trace_len;
} ind_result;

void ind_config_default(ind_config *cfg);

/* Run the induction. Returns 0 when the run completed (proved or refuted),
 * 1 on an internal error (allocation, malformed input). */
int induction_run(const ind_config *cfg, const relais_params *p, const relais_traj *t,
                  const ind_property *prop, ind_result *out);

/* The per-invariant verdict table derived from a run. */
size_t induction_verdicts(const ind_result *res, ind_verdict *out, size_t cap);

/* Print the published report: the table of invariants x k, the states and
 * transitions examined, the time, the verdict. */
void induction_report(FILE *f, const ind_config *cfg, const ind_result *res,
                      const ind_property *prop);

/* The situation of a world, hashed: two worlds with the same situation hash
 * equal. The hash forgets what cannot change a verdict (the plant, the floats,
 * the absolute tick), which is what makes the quotient finite. */
uint64_t induction_class_hash(const explore_world *w);

/* The clauses the engine ships with. `core` is the safety checker of the core
 * library (the conjunction of the invariants of spec 7.2). The two others are
 * deliberately FALSE properties: the witness test needs a property the engine
 * must refuse. */
extern const ind_clause ind_clause_core;
bool ind_clause_holder_never_lost(const explore_world *w, void *ctx);
bool ind_clause_tick_always_even(const explore_world *w, void *ctx);
bool ind_clause_no_future_mandate(const explore_world *w, void *ctx);

#endif /* RELAIS_EXPLORE_INDUCTION_H */
