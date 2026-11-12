/* Relais — space B: bounded exhaustion of the arbiter's input space (M36).
 *
 * Every case calls the REAL relais_arb_step() of core/src/arbiter.c. The oracle
 * is the published protocol (docs/SPEC.md 4.3/4.4, docs/PROTOCOL.md 2) and the
 * published invariants (core/src/invariants.c: I1, I2, I3), not the code under
 * test: the acceptance rule below is transcribed from the spec table.
 *
 * Enumerations (all of them complete inside the bounds of mc.h):
 *   B1 every state x every tick offset x every frame of the cross product
 *      type x sender x tick label x mandate x value.
 *   B2 every ordered pair of a 16-frame vocabulary, for every state.
 *   B3 every sequence of up to three frames, one tick each, for every state.
 *   B4 a corrupted frame is equivalent to a frame that was never sent (WA-5).
 *   B5 the boundary of the arbiter's documented input contract: what happens
 *      with a sender the decoder can never produce (published, not asserted).
 *
 * Properties checked on every in-contract case:
 *   I1  (ACT-01) exactly one value per tick, from one of the four sources, and
 *       the counter of that source is the one that moved.
 *   I2  (MAN-03) the mandate never decreases, moves by at most one per tick,
 *       and two machines never hold the same mandate number.
 *   I3  (MAN-02, SEC-03, SEC-04) a value comes from the holder only when a
 *       frame named this tick, came from the holder and carried its mandate;
 *       a stale order is rejected and counted (DET-04).
 *   ACT-05 the applied value is a finite number inside max_amplitude.
 */
#include "mc.h"

#include <math.h>
#include <string.h>

#include "relais/arbiter.h"
#include "relais/control.h"

uint64_t mc_obs_holder_not_computer = 0;
uint64_t mc_obs_demande_from_actuator = 0;
uint64_t mc_obs_contract_violation = 0;

/* ------------------------------------------------------------- states ---- */

static void fixed_meas(relais_meas *m) {
  m->pos = 0.1f;
  m->vel = 0.05f;
  m->applied = 0.0f;
  m->setpoint = 0.2f;
}

static void step(relais_arb *a, relais_tick t, const relais_msg *m, size_t n, relais_arb_out *o) {
  relais_meas meas;
  fixed_meas(&meas);
  relais_arb_step(a, t, m, n, &meas, o);
}

static void grant_to(relais_arb *a, relais_tick t, relais_node who, const relais_params *p) {
  relais_msg d;
  relais_arb_out o;
  t_demande(&d, who, a->mandate, t, false, 0.0f, p);
  step(a, t, &d, 1, &o);
}

static void order_shadow(relais_arb *a, relais_tick t, const relais_params *p, float v,
                         bool agree) {
  relais_msg m[2];
  relais_arb_out o;
  relais_node second = a->holder == RELAIS_NODE_A ? RELAIS_NODE_B : RELAIS_NODE_A;
  t_order(&m[0], a->holder, a->mandate, t, v, p);
  t_shadow(&m[1], second, t, agree ? v : v + 0.5f, 0);
  step(a, t, m, 2, &o);
}

static void silent(relais_arb *a, relais_tick from, uint32_t n) {
  relais_arb_out o;
  t_arb_silent_ticks(a, from, n, &o);
}

/* The published prefixes of the 13 states. Each one is a deterministic
 * sequence of ticks, so the state is the same on every run and the count of
 * cases is stable. docs/MODELCHECK.md lists them. */
static void prefix_grant(mc_state *st, const relais_params *p, relais_tick *t) {
  grant_to(&st->arb, *t, RELAIS_NODE_A, p);
  (*t)++;
}

static void prefix_certified(mc_state *st, const relais_params *p, relais_tick *t) {
  prefix_grant(st, p, t);
  for (unsigned i = 0; i < 3; i++) { /* certify_ticks = 3: the second is certified */
    order_shadow(&st->arb, *t, p, 0.25f, true);
    (*t)++;
  }
}

static void prefix_silent(mc_state *st, const relais_params *p, relais_tick *t, uint32_t n) {
  (void)p;
  silent(&st->arb, *t, n);
  *t += n;
}

static void prefix_cession(mc_state *st, const relais_params *p, relais_tick *t) {
  relais_msg c;
  relais_arb_out o;
  prefix_grant(st, p, t);
  t_cession(&c, RELAIS_NODE_A, st->arb.mandate, 1);
  step(&st->arb, *t, &c, 1, &o);
  (*t)++; /* REL-06: the mandate is freed at the NEXT tick */
}

static void prefix_order(mc_state *st, const relais_params *p, relais_tick *t) {
  relais_msg o;
  relais_arb_out out;
  prefix_grant(st, p, t);
  t_order(&o, RELAIS_NODE_A, st->arb.mandate, *t, 0.25f, p);
  step(&st->arb, *t, &o, 1, &out);
  (*t)++; /* the will is loaded, its base is the next tick */
}

static void prefix_will_exhausted(mc_state *st, const relais_params *p, relais_tick *t) {
  prefix_order(st, p, t);
  prefix_silent(st, p, t, 9); /* the will covers 8 ticks: the 9th falls back */
}

static void prefix_disagree(mc_state *st, const relais_params *p, relais_tick *t) {
  prefix_grant(st, p, t);
  for (unsigned i = 0; i < 3; i++) { /* DET-09: three ticks above epsilon */
    order_shadow(&st->arb, *t, p, 0.25f, false);
    (*t)++;
  }
}

static void prefix_holder_c(mc_state *st, const relais_params *p, relais_tick *t) {
  grant_to(&st->arb, *t, RELAIS_NODE_C, p);
  (*t)++;
}

/* Build the state `which`; next = the tick the enumeration must run next. */
void mc_arb_build_state(unsigned which, mc_state *st) {
  relais_params p;
  relais_tick t = 1;
  mc_params_set(&p, which == 11u ? 1u : 0u);
  relais_arb_init(&st->arb, &p);
  switch (which) {
    case 0: break; /* fresh: vacant, mandate 0 */
    case 1: prefix_grant(st, &p, &t); break;
    case 2: prefix_certified(st, &p, &t); break;
    case 3: prefix_grant(st, &p, &t); prefix_silent(st, &p, &t, 1); break;
    case 4: prefix_grant(st, &p, &t); prefix_silent(st, &p, &t, 2); break;
    case 5: prefix_cession(st, &p, &t); break;
    case 6: prefix_order(st, &p, &t); break;
    case 7: prefix_will_exhausted(st, &p, &t); break;
    case 8: prefix_certified(st, &p, &t); prefix_silent(st, &p, &t, 2); break;
    case 9: prefix_disagree(st, &p, &t); break;
    case 10: prefix_grant(st, &p, &t); prefix_silent(st, &p, &t, p.degraded_ticks); break;
    case 11: prefix_grant(st, &p, &t); prefix_silent(st, &p, &t, p.patience); break;
    default: prefix_holder_c(st, &p, &t); break;
  }
  st->next = t;
}

/* -------------------------------------------------------------- oracle ---- */

/* The mandate at the moment the scan runs: REL-06, a cession recorded at the
 * previous tick frees the mandate at the start of this one, so the scan
 * already sees the new holder (SPEC 4.3, "Revocation"). */
static void holder_at_scan(const relais_arb *b, relais_node *holder, relais_mandate *mandate) {
  *holder = b->holder;
  *mandate = b->mandate;
  if (!b->cession_pending) return;
  if (b->certified && b->certified_node != RELAIS_NODE_NONE &&
      b->certified_node != b->holder) {
    *holder = b->certified_node;
    *mandate = b->mandate + 1u;
    return;
  }
  *holder = RELAIS_NODE_NONE;
}

/* The mandate at the moment a DEMANDE is considered: the miss is counted
 * first, then the revocation happens (SPEC 4.3), then the demand is granted
 * if the mandate is vacant (SPEC 4.3, "Assignment, base"). */
static void holder_at_grant(const relais_arb *b, bool fresh, relais_node *holder,
                            relais_mandate *mandate) {
  holder_at_scan(b, holder, mandate);
  if (fresh) return; /* a valid order: no miss, so no revocation */
  uint8_t misses = b->misses < 255u ? (uint8_t)(b->misses + 1u) : 255u;
  if (*holder == RELAIS_NODE_NONE || misses <= b->params.patience) return;
  if (b->certified && b->certified_node != RELAIS_NODE_NONE &&
      b->certified_node != *holder) {
    *holder = b->certified_node;
    *mandate = *mandate + 1u;
    return;
  }
  *holder = RELAIS_NODE_NONE;
}

/* SEC-06: a burst of demandes is refused once the window is full. */
static bool rate_allowed(const relais_arb *b, relais_tick tick) {
  uint16_t window =
      (uint16_t)(tick / (b->params.request_window ? b->params.request_window : 1u));
  if (window != b->request_window_tick) return true; /* the counter restarts */
  return b->request_seen < b->params.request_burst;
}

/* SPEC 4.3, "Acceptance": an ORDRE counts only if it comes from the holder,
 * carries the current mandate and targets the tick being applied. */
static bool order_accepted(const relais_msg *m, relais_node h, relais_mandate md, relais_tick t) {
  return m->sender == h && m->mandate == md && m->tick == t;
}

static relais_reject order_reason(const relais_msg *m, relais_node h, relais_mandate md,
                                  relais_tick t) {
  if (m->sender != h) return RELAIS_REJ_NOT_HOLDER;
  if (m->mandate > md) return RELAIS_REJ_MANDATE_FUTURE; /* SEC-04 */
  if (m->mandate != md) return RELAIS_REJ_MANDATE_STALE;
  if (m->tick != t) return RELAIS_REJ_TICK_MISSED;       /* SEC-03 */
  return RELAIS_REJ_NONE;
}

/* MAN-05: a DEMANDE granted this tick may carry the first order of the new
 * holder. Only the FIRST demand of the period is considered, the grant needs
 * a vacant mandate and a free rate-limit window (SEC-06). */
static bool granted_order(const relais_msg *msgs, size_t n, const relais_arb *b,
                          const relais_arb *a, relais_tick t, bool fresh) {
  const relais_msg *dem = NULL;
  for (size_t i = 0; i < n; i++) {
    if (msgs[i].type != RELAIS_MSG_DEMANDE) continue;
    dem = &msgs[i];
    break;
  }
  if (dem == NULL) return false;
  relais_node holder;
  relais_mandate mandate;
  holder_at_grant(b, fresh, &holder, &mandate);
  if (holder != RELAIS_NODE_NONE) return false; /* the mandate was not vacant */
  if (!rate_allowed(b, t)) return false;
  if (!dem->has_order || dem->tick != t) return false;
  return a->holder == dem->sender && a->mandate == mandate + 1u;
}

/* ---------------------------------------------------------- properties ---- */

static uint32_t source_total(const relais_arb *a) {
  uint32_t sum = 0;
  for (unsigned i = 0; i < 5; i++) sum += a->applied_by_source[i];
  return sum;
}

static void check_i1(const relais_arb *b, const relais_arb *a, const relais_arb_out *o) {
  char d[288];
  mc_fail(d, sizeof d, "I1 no value applied");
  T_MSG(o->applied, d);
  T_MSG(o->source >= RELAIS_SRC_HOLDER && o->source <= RELAIS_SRC_FALLBACK, d);
  mc_fail(d, sizeof d, "I1 not exactly one value");
  T_MSG(source_total(a) == source_total(b) + 1u, d);
  mc_fail(d, sizeof d, "I1 wrong source counter");
  T_MSG(a->applied_by_source[o->source] == b->applied_by_source[o->source] + 1u, d);
  mc_fail(d, sizeof d, "I1 order applied with no holder");
  T_MSG(o->source != RELAIS_SRC_HOLDER || o->holder != RELAIS_NODE_NONE, d);
  mc_fail(d, sizeof d, "I1 substitution by an uncertified second");
  T_MSG(o->source != RELAIS_SRC_SHADOW || o->certified, d);
}

static void check_i2(const relais_arb *b, const relais_arb *a, const relais_arb_out *o) {
  char d[288];
  mc_fail(d, sizeof d, "I2 mandate went backwards");
  T_MSG(o->mandate >= b->mandate, d);
  mc_fail(d, sizeof d, "I2 mandate moved twice in one tick");
  T_MSG(o->mandate - b->mandate <= 1u, d);
  mc_fail(d, sizeof d, "I2 two holders of the same mandate");
  T_MSG(!(b->holder != RELAIS_NODE_NONE && o->holder != RELAIS_NODE_NONE &&
          o->holder != b->holder && o->mandate == b->mandate),
        d);
  mc_fail(d, sizeof d, "I2 published holder is not the arbiter's");
  T_MSG(o->holder == a->holder && o->mandate == a->mandate, d);
}

static void check_i3(const relais_arb *b, const relais_arb *a, const relais_msg *msgs, size_t n,
                     relais_tick t, const relais_arb_out *o) {
  char d[288];
  relais_node holder;
  relais_mandate mandate;
  uint32_t expect[RELAIS_REJECT_SLOTS];
  bool fresh = false;
  holder_at_scan(b, &holder, &mandate);
  memset(expect, 0, sizeof expect);
  for (size_t i = 0; i < n; i++) {
    const relais_msg *m = &msgs[i];
    if (m->type != RELAIS_MSG_ORDRE) continue;
    if (order_accepted(m, holder, mandate, t)) {
      fresh = true;
      continue;
    }
    expect[order_reason(m, holder, mandate, t)]++;
  }
  for (unsigned i = 0; i < RELAIS_REJECT_SLOTS; i++) {
    mc_fail(d, sizeof d, "I3 rejection counter");
    T_MSG(a->rejected[i] == b->rejected[i] + expect[i], d);
  }
  mc_fail(d, sizeof d, "I3 stale order applied");
  if (o->source == RELAIS_SRC_HOLDER)
    T_MSG(fresh || granted_order(msgs, n, b, a, t, fresh), d);
  else
    T_MSG(!fresh, d);
}

static void check_bounds(const relais_arb *a, const relais_arb_out *o) {
  char d[288];
  float amp = a->params.max_amplitude;
  mc_fail(d, sizeof d, "ACT-05 value out of bounds");
  T_MSG(isfinite(o->value), d);
  T_MSG(o->value <= amp && o->value >= -amp, d);
  mc_fail(d, sizeof d, "ACT-05 published value is not the applied one");
  T_MSG(o->statut.meas.applied == o->value, d);
}

/* One case: copy the state, run one real step, check every property. The
 * evolved arbiter is written back so a sequence can continue from it. */
void mc_run_case(mc_space *sp, const relais_arb *state, const relais_msg *msgs, size_t n,
                 relais_tick tick, bool in_contract, relais_arb *evolved,
                 relais_arb_out *outcome) {
  relais_arb a = *state;
  relais_arb_out out;
  step(&a, tick, msgs, n, &out);
  if (in_contract) {
    check_i1(state, &a, &out);
    check_i3(state, &a, msgs, n, tick, &out);
  }
  check_i2(state, &a, &out);
  check_bounds(&a, &out);
  if (out.holder != RELAIS_NODE_NONE && !relais_node_is_computer(out.holder))
    mc_obs_holder_not_computer++;
  if (out.holder == RELAIS_NODE_ACTUATOR) {
    for (size_t i = 0; i < n; i++)
      if (msgs[i].type == RELAIS_MSG_DEMANDE && msgs[i].sender == RELAIS_NODE_ACTUATOR)
        mc_obs_demande_from_actuator++;
  }
  if (evolved != NULL) *evolved = a;
  if (outcome != NULL) *outcome = out;
  sp->cases++;
}
