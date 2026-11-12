/* Relais — the symbolic arbiter model shared by the CBMC harnesses (M36).
 *
 * One implementation, included by every arbiter harness: the parameters, a
 * contract-legal symbolic state and a symbolic frame. Static inline, so each
 * harness keeps its own copy and there is no second definition anywhere.
 */
#ifndef RELAIS_MC_ARB_MODEL_H
#define RELAIS_MC_ARB_MODEL_H

#include <string.h>

#include "mc_cbmc.h"
#include "relais/arbiter.h"
#include "relais/control.h"

static inline void mc_build_params(relais_params *p) {
  memset(p, 0, sizeof *p);
  p->period_ms = 5;
  p->patience = (uint8_t)(MC_NONDET_U32() % 4u);
  p->testament_len = 8;
  p->certify_ticks = 3;
  p->bail_ticks = 4;
  p->request_burst = 4;
  p->request_window = 4;
  p->epsilon = 0.005f;
  p->delta = 0.02f;
  p->dissimilar_epsilon = 0.0f;
  p->dissimilar_delta = 0.02f;
  p->degraded_ticks = 6000;
  p->max_amplitude = RELAIS_FULL_SCALE;
  p->max_slew = 0.25f;
  p->slow_motion = false;
  p->slow_factor = 10;
}

static inline void mc_build_state(relais_arb *a, const relais_params *p) {
  memset(a, 0, sizeof *a);
  a->params = *p;
  a->mandate = MC_NONDET_U32() % 4u;
  a->holder = (relais_node)(MC_NONDET_U32() % 5u); /* NONE, A, B, C, ACTUATOR */
  a->misses = (uint8_t)(MC_NONDET_U32() % 5u);
  a->certified = MC_NONDET_BOOL();
  a->certified_node = (relais_node)(MC_NONDET_U32() % 5u);
  a->testament_len = (uint8_t)(MC_NONDET_U32() % 9u);
  a->testament_base = MC_NONDET_U64();
  a->present[RELAIS_NODE_A] = MC_NONDET_BOOL();
  a->present[RELAIS_NODE_B] = MC_NONDET_BOOL();
  a->present[RELAIS_NODE_C] = MC_NONDET_BOOL();
  a->ever_seen[RELAIS_NODE_A] = MC_NONDET_BOOL();
  a->ever_seen[RELAIS_NODE_B] = MC_NONDET_BOOL();
  a->ever_seen[RELAIS_NODE_C] = MC_NONDET_BOOL();
  a->stop_requested = MC_NONDET_BOOL();
  a->cession_pending = MC_NONDET_BOOL();
  a->request_window_tick = (uint16_t)(MC_NONDET_U32() % 4u);
  a->request_seen = (uint8_t)(MC_NONDET_U32() % 6u);
}

static inline void mc_build_frame(relais_msg *m, const relais_arb *a, relais_tick tick) {
  memset(m, 0, sizeof *m);
  m->type = (relais_msg_type)(1u + MC_NONDET_U32() % 7u);
  m->sender = (relais_node)(1u + MC_NONDET_U32() % 4u); /* A, B, C, ACTUATOR */
  m->flags = 0;
  m->tick = (MC_NONDET_BOOL()) ? tick : tick + MC_NONDET_U64();
  m->mandate = a->mandate + (MC_NONDET_U32() % 3u) - 1u;
  m->value = MC_NONDET_FLOAT();
  m->has_order = MC_NONDET_BOOL();
  m->testament_len = (uint8_t)(MC_NONDET_U32() % 9u);
  m->replica_age = 0;
  m->seen_mandate = m->mandate;
  m->reason = 0;
  m->ctl_state_len = 0;
  m->last_value = 0.0f;
  m->ready = false;
}

#endif /* RELAIS_MC_ARB_MODEL_H */
