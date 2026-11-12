/* Relais — minimal test harness shared by every unit test. No dependency, no
 * allocation beyond stdio. Include it, use T_CHECK / T_EQ / T_NEAR, return
 * t_summary() from main.
 */
#ifndef RELAIS_TEST_UTIL_H
#define RELAIS_TEST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "relais/frame.h"
#include "relais/invariants.h"
#include "relais/journal.h"
#include "relais/node.h"
#include "relais/params.h"

extern long t_checks;
extern long t_failures;

void t_fail(const char *file, int line, const char *what);
int t_summary(void);
void t_section(const char *name);

#define T_CHECK(cond)                                                        \
  do {                                                                       \
    t_checks++;                                                              \
    if (!(cond)) t_fail(__FILE__, __LINE__, #cond);                          \
  } while (0)

#define T_MSG(cond, msg)                                                     \
  do {                                                                       \
    t_checks++;                                                              \
    if (!(cond)) t_fail(__FILE__, __LINE__, msg);                            \
  } while (0)

#define T_EQ_U(a, b)                                                         \
  do {                                                                       \
    uint64_t va_ = (uint64_t)(a), vb_ = (uint64_t)(b);                       \
    t_checks++;                                                              \
    if (va_ != vb_) t_fail_eq_u(__FILE__, __LINE__, #a, va_, #b, vb_);       \
  } while (0)

#define T_NE(a, b)                                                           \
  do {                                                                       \
    t_checks++;                                                              \
    if ((a) == (b)) t_fail(__FILE__, __LINE__, #a " unexpectedly equal to " #b); \
  } while (0)

#define T_NEAR(a, b, tol)                                                    \
  do {                                                                       \
    double va_ = (double)(a), vb_ = (double)(b);                             \
    t_checks++;                                                              \
    if (!(va_ - vb_ <= (tol) && vb_ - va_ <= (tol)))                         \
      t_fail_near(__FILE__, __LINE__, #a, va_, #b, vb_, (double)(tol));      \
  } while (0)

void t_fail_eq_u(const char *file, int line, const char *na, uint64_t va,
                 const char *nb, uint64_t vb);
void t_fail_near(const char *file, int line, const char *na, double va,
                 const char *nb, double vb, double tol);

/* ---- shared fixtures: one place, no copy-paste between tests ---- */

/* Parse a 32-hex-digit key and check that it parsed: shared by the crypto tests
 * so no test file re-implements key loading. */
bool t_key(relais_key *k, const char *hex);

/* Deterministic PRNG, same stream everywhere (xorshift64*). */
typedef struct { uint64_t s; } t_rng;
void t_rng_seed(t_rng *r, uint64_t seed);
uint64_t t_rng_next(t_rng *r);
uint32_t t_rng_below(t_rng *r, uint32_t n);

/* Build a STATUT as the actuator publishes it. */
void t_statut(relais_msg *m, relais_tick tick, relais_mandate mandate,
              relais_node holder, relais_source src, uint8_t misses,
              const relais_meas *meas);
/* Build an ORDRE of `from` targeting `tick`, with a flat testament. */
void t_order(relais_msg *m, relais_node from, relais_mandate mandate,
             relais_tick tick, float value, const relais_params *p);
void t_shadow(relais_msg *m, relais_node from, relais_tick tick, float value,
              uint8_t replica_age);
void t_demande(relais_msg *m, relais_node from, relais_mandate seen,
               relais_tick tick, bool with_order, float value,
               const relais_params *p);
void t_cession(relais_msg *m, relais_node from, relais_mandate mandate, uint8_t reason);
void t_etat(relais_msg *m, relais_node from, relais_tick tick,
            const relais_ctl_state *st, float last_value);

/* Run the arbiter for `n` ticks with no incoming frame: the holder is silent. */
void t_arb_silent_ticks(relais_arb *a, relais_tick from, uint32_t n,
                        relais_arb_out *last);

#endif /* RELAIS_TEST_UTIL_H */
