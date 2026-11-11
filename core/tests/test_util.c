/* Relais — shared test harness and fixtures. One implementation, used by every
 * unit test (no copy-paste between test files). */
#include "test_util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

long t_checks = 0;
long t_failures = 0;

void t_section(const char *name) { printf("-- %s\n", name); }

void t_fail(const char *file, int line, const char *what) {
  t_failures++;
  printf("FAIL %s:%d %s\n", file, line, what);
}

void t_fail_eq_u(const char *file, int line, const char *na, uint64_t va,
                 const char *nb, uint64_t vb) {
  t_failures++;
  printf("FAIL %s:%d %s=%llu != %s=%llu\n", file, line, na,
         (unsigned long long)va, nb, (unsigned long long)vb);
}

void t_fail_near(const char *file, int line, const char *na, double va,
                 const char *nb, double vb, double tol) {
  t_failures++;
  printf("FAIL %s:%d %s=%.6g not within %.6g of %s=%.6g\n", file, line, na, va,
         tol, nb, vb);
}

int t_summary(void) {
  if (t_failures != 0) {
    printf("FAILED %ld/%ld checks\n", t_failures, t_checks);
    return 1;
  }
  printf("OK %ld checks\n", t_checks);
  return 0;
}

void t_rng_seed(t_rng *r, uint64_t seed) { r->s = seed ? seed : 0x9e3779b97f4a7c15ull; }

uint64_t t_rng_next(t_rng *r) {
  uint64_t x = r->s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  r->s = x;
  return x * 0x2545f4914f6cdd1dull;
}

uint32_t t_rng_below(t_rng *r, uint32_t n) {
  if (n == 0) return 0;
  return (uint32_t)(t_rng_next(r) % n);
}

/* The fixtures below need frame.c and control.c. A test that only exercises
 * the journal or the invariants compiles with -DRELAIS_TEST_HARNESS_ONLY and
 * links without them: same harness, no duplicated counters. */
#ifndef RELAIS_TEST_HARNESS_ONLY

void t_statut(relais_msg *m, relais_tick tick, relais_mandate mandate,
              relais_node holder, relais_source src, uint8_t misses,
              const relais_meas *meas) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_STATUT;
  m->sender = RELAIS_NODE_ACTUATOR;
  m->tick = tick;
  m->mandate = mandate;
  m->holder = holder;
  m->source = src;
  m->misses = misses;
  m->certified = false;
  m->certified_node = RELAIS_NODE_NONE;
  m->lamp = RELAIS_LAMP_OFF;
  m->cause = RELAIS_CAUSE_NONE;
  if (meas != NULL) m->meas = *meas;
}

void t_order(relais_msg *m, relais_node from, relais_mandate mandate,
             relais_tick tick, float value, const relais_params *p) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ORDRE;
  m->sender = from;
  m->tick = tick;
  m->mandate = mandate;
  m->value = value;
  m->has_order = true;
  relais_ctl_state st;
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  relais_ctl_init(&st, p);
  relais_testament_fill(&st, &meas, p, m->testament, &m->testament_len);
}

void t_shadow(relais_msg *m, relais_node from, relais_tick tick, float value,
              uint8_t replica_age) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_OMBRE;
  m->sender = from;
  m->tick = tick;
  m->value = value;
  m->replica_age = replica_age;
}

void t_demande(relais_msg *m, relais_node from, relais_mandate seen,
               relais_tick tick, bool with_order, float value,
               const relais_params *p) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_DEMANDE;
  m->sender = from;
  m->tick = tick;
  m->seen_mandate = seen;
  m->reason = 1;
  m->has_order = with_order;
  if (with_order) {
    m->value = value;
    m->flags |= RELAIS_FLAG_HAS_ORDER;
    relais_ctl_state st;
    relais_meas meas;
    memset(&meas, 0, sizeof meas);
    relais_ctl_init(&st, p);
    relais_testament_fill(&st, &meas, p, m->testament, &m->testament_len);
  }
}

void t_cession(relais_msg *m, relais_node from, relais_mandate mandate, uint8_t reason) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_CESSION;
  m->sender = from;
  m->mandate = mandate;
  m->reason = reason;
  m->flags |= RELAIS_FLAG_CESSION;
}

void t_etat(relais_msg *m, relais_node from, relais_tick tick,
            const relais_ctl_state *st, float last_value) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ETAT;
  m->sender = from;
  m->tick = tick;
  m->last_value = last_value;
  m->ctl_state_len = relais_ctl_export(st, m->ctl_state, sizeof m->ctl_state);
}

void t_arb_silent_ticks(relais_arb *a, relais_tick from, uint32_t n,
                        relais_arb_out *last) {
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  for (uint32_t i = 0; i < n; i++) {
    relais_arb_step(a, from + i, NULL, 0, &meas, last);
  }
}

#endif /* RELAIS_TEST_HARNESS_ONLY */
