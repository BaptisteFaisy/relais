/* Relais — the Relais protocol seen through the comparison harness.
 *
 * This adapter does NOT reimplement anything: it drives the real core
 * (core/src/arbiter.c and core/src/node.c) through the interface of
 * host/baseline/proto.h, so the numbers published for "Relais" are the
 * numbers of the shipped protocol, not of a sketch of it.
 *
 * The only translation is the wire vocabulary: the harness speaks in
 * bl_status / bl_act_out, the core speaks in relais_msg / relais_arb_out.
 */
#include "proto.h"

#include "relais/arbiter.h"
#include "relais/node.h"

#include <string.h>

typedef struct {
  relais_arb arb;
  relais_params params;
} rb_act;

typedef struct {
  relais_node_sm sm;
  relais_params params;
} rb_node;

static uint8_t rb_index(relais_node node) {
  if (node == RELAIS_NODE_A) return 0u;
  if (node == RELAIS_NODE_B) return 1u;
  return BL_NONE;
}

static relais_node rb_node_of(uint8_t idx) {
  if (idx == 0u) return RELAIS_NODE_A;
  if (idx == 1u) return RELAIS_NODE_B;
  return RELAIS_NODE_NONE;
}

static void rb_act_init(bl_act *a, const relais_params *p, uint32_t timeout) {
  (void)timeout; /* Relais counts its evidence, it never waits for a timeout */
  rb_act *s = (rb_act *)a->s.raw;
  memset(s, 0, sizeof *s);
  s->params = *p;
  relais_arb_init(&s->arb, p);
}

static void rb_act_step(bl_act *a, uint32_t tick, const relais_msg *msgs, size_t n,
                        const bl_meas *m, bl_act_out *out) {
  rb_act *s = (rb_act *)a->s.raw;
  relais_meas rm;
  rm.pos = m->pos;
  rm.vel = m->vel;
  rm.applied = m->applied;
  rm.setpoint = m->setpoint;
  relais_arb_out ao;
  relais_arb_step(&s->arb, (relais_tick)tick, msgs, n, &rm, &ao);
  out->value = ao.value;
  out->fresh = ao.source == RELAIS_SRC_HOLDER || ao.source == RELAIS_SRC_SHADOW;
  out->fresh_node = BL_NONE;
  if (ao.source == RELAIS_SRC_HOLDER) out->fresh_node = rb_index(ao.holder);
  else if (ao.source == RELAIS_SRC_SHADOW) out->fresh_node = rb_index(ao.certified_node);
  out->commander = rb_index(ao.holder);
  out->term = ao.mandate;
  out->misses = ao.misses;
  out->certified = ao.certified;
  out->certified_node = rb_index(ao.certified_node);
}

static void rb_act_counters(const bl_act *a, uint32_t *out, size_t n) {
  const rb_act *s = (const rb_act *)a->s.raw;
  if (n > 0) out[0] = s->arb.rejected[RELAIS_REJ_NOT_HOLDER];
  if (n > 1) out[1] = relais_arb_rejected_total(&s->arb);
}

static void rb_node_init(bl_node *nd, int idx, const relais_params *p, uint32_t timeout) {
  (void)timeout;
  rb_node *s = (rb_node *)nd->s.raw;
  memset(s, 0, sizeof *s);
  s->params = *p;
  relais_node_init(&s->sm, p, rb_node_of((uint8_t)idx));
}

/* The STATUT the core expects: the mandate, the misses, the certification and
 * the measurements (MAN-07). */
static void rb_make_statut(const bl_status *st, relais_msg *m) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_STATUT;
  m->sender = RELAIS_NODE_ACTUATOR;
  m->tick = (relais_tick)st->tick;
  m->mandate = st->term;
  m->holder = rb_node_of(st->commander);
  m->misses = st->misses;
  m->certified = st->certified;
  m->certified_node = rb_node_of(st->certified_node);
  m->meas.pos = st->meas.pos;
  m->meas.vel = st->meas.vel;
  m->meas.applied = st->meas.applied;
  m->meas.setpoint = st->meas.setpoint;
}

/* The first order frame of the tick, if any: that is what "the computer
 * believes it commands" means for every implementation. */
static void rb_pick_order(const relais_node_out *no, bl_node_out *out) {
  for (uint8_t i = 0; i < no->count; i++) {
    if (no->out[i].type != RELAIS_MSG_ORDRE) continue;
    out->commands = true;
    out->command = no->out[i].value;
    return;
  }
}

static void rb_node_step(bl_node *nd, int idx, const bl_status *st, const relais_msg *peer,
                         size_t n_peer, bl_node_out *out) {
  (void)idx;
  rb_node *s = (rb_node *)nd->s.raw;
  relais_node_out no;
  /* The state of the holder first, then the status: the second computes on
   * the state the holder had before it computed (REP-01). */
  for (size_t i = 0; i < n_peer; i++) {
    if (peer[i].type == RELAIS_MSG_ETAT) relais_node_on_etat(&s->sm, &peer[i], &no);
    else if (peer[i].type == RELAIS_MSG_SUIVI) relais_node_on_suivi(&s->sm, &peer[i], &no);
  }
  if (!st->heard) {
    relais_node_on_silence(&s->sm, (relais_tick)st->tick, &no);
    return;
  }
  relais_msg statut;
  rb_make_statut(st, &statut);
  relais_node_on_status(&s->sm, &statut, &no);
  out->count = no.count > BL_OUT_MAX ? BL_OUT_MAX : no.count;
  for (uint8_t i = 0; i < out->count; i++) out->m[i] = no.out[i];
  rb_pick_order(&no, out);
}

static void rb_node_fault(bl_node *nd, int idx, uint8_t reason, bl_node_out *out) {
  (void)idx;
  rb_node *s = (rb_node *)nd->s.raw;
  relais_node_out no;
  /* DET-05: a critical local process died, the machine cedes in the same
   * tick. The actuation decides, the next tick (REL-06). */
  relais_node_on_local_fault(&s->sm, reason, &no);
  out->count = no.count > BL_OUT_MAX ? BL_OUT_MAX : no.count;
  for (uint8_t i = 0; i < out->count; i++) out->m[i] = no.out[i];
  rb_pick_order(&no, out);
}

static const bl_impl RB_IMPL = {
    .name = "relais",
    .act_init = rb_act_init,
    .act_step = rb_act_step,
    .act_counters = rb_act_counters,
    .node_init = rb_node_init,
    .node_step = rb_node_step,
    .node_fault = rb_node_fault,
};

const bl_impl *bl_impl_relais(void) { return &RB_IMPL; }
