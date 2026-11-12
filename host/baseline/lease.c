/* Relais — baseline 2: a lease with a term (the Raft / etcd family, reduced).
 *
 * What is kept from the state of the art:
 *   - a lease: the leader holds it for a fixed duration and renews it with a
 *     heartbeat; the lease is the only right to command;
 *   - a term (Raft's election term): it only grows, and a frame carrying an
 *     old term is rejected by the registrar. That is what makes two leaders
 *     harmless: the loser of the election is silent for ever;
 *   - the registrar is the authority: it publishes the holder, the term and
 *     the expiry, and it grants the lease only once the previous lease has
 *     expired (Chubby / etcd lease + keepalive, Raft section 6.4 read-only
 *     leases);
 *   - a leader that is cut off steps down locally when its lease expires, so
 *     it cannot keep commanding on its own clock.
 *
 * What is simplified, and why (the point is to compare handovers, not to
 * reimplement Raft):
 *   - there is no log, no commit index and no quorum: two computers and one
 *     registrar, so the quorum is the registrar itself;
 *   - the election timeout is the lease duration itself: one knob, as in the
 *     heartbeat baseline, so the comparison is about the mechanism and not
 *     about the tuning;
 *   - no randomised backoff: with two candidates a random delay only adds
 *     latency;
 *   - the state replication is the one of the project (the ETAT frame), not a
 *     Raft log: the point being compared is the handover, not the storage.
 */
#include "proto.h"

#include "relais/control.h"

#include <string.h>

typedef struct {
  uint32_t lease_ticks;
  uint32_t term;
  uint8_t holder;   /* the lease holder, BL_NONE when the lease is free */
  uint32_t expiry;  /* the tick at which the lease expires */
  float fb_state;
  float last_value;
  bool have_last;
  relais_params params;
} ls_act;

typedef struct {
  uint32_t lease_ticks;
  uint8_t index;
  uint8_t peer;
  uint32_t term;         /* the term it believes in */
  bool leader;           /* it believes it holds the lease */
  bool candidate;        /* it asked for the lease and waits for the grant */
  uint32_t local_expiry; /* its own clock: the lease it believes it holds */
  uint32_t last_etat;
  bool have_etat;
  relais_ctl_state ctl;
  float last_value;
  relais_params params;
} ls_node;

static uint8_t ls_index(relais_node node) {
  if (node == RELAIS_NODE_A) return 0u;
  if (node == RELAIS_NODE_B) return 1u;
  return BL_NONE;
}

static relais_node ls_node_of(uint8_t idx) {
  return idx == 0u ? RELAIS_NODE_A : RELAIS_NODE_B;
}

static void ls_act_init(bl_act *a, const relais_params *p, uint32_t timeout) {
  ls_act *s = (ls_act *)a->s.raw;
  memset(s, 0, sizeof *s);
  s->params = *p;
  s->lease_ticks = timeout > 0 ? timeout : 1u;
  s->holder = BL_NONE;
  relais_fallback_init(&s->fb_state, NULL);
}

/* The lease is free when it was never granted or when it expired. */
static bool ls_free(const ls_act *s, uint32_t tick) {
  if (s->holder == BL_NONE) return true;
  return tick >= s->expiry;
}

static void ls_grant(ls_act *s, uint32_t tick, uint8_t node, uint32_t term) {
  s->term = term > s->term ? term : s->term;
  s->holder = node;
  s->expiry = tick + s->lease_ticks;
}

static void ls_act_step(bl_act *a, uint32_t tick, const relais_msg *msgs, size_t n,
                        const bl_meas *m, bl_act_out *out) {
  ls_act *s = (ls_act *)a->s.raw;
  bool command_ok = false;
  float command = 0.0f;
  uint8_t command_node = BL_NONE;
  for (size_t i = 0; i < n; i++) {
    const relais_msg *msg = &msgs[i];
    uint8_t idx = ls_index(msg->sender);
    if (idx == BL_NONE) continue;
    if (msg->type == RELAIS_MSG_OMBRE) {  /* the renewal of the lease */
      if (s->holder == idx && msg->mandate == s->term) s->expiry = tick + s->lease_ticks;
      continue;
    }
    if (msg->type == RELAIS_MSG_CESSION) { /* the leader lets the lease go */
      if (s->holder == idx && msg->mandate == s->term) {
        s->holder = BL_NONE;
        s->expiry = 0;
      }
      continue;
    }
    if (msg->type == RELAIS_MSG_DEMANDE) { /* a request for the lease */
      if (ls_free(s, tick)) ls_grant(s, tick, idx, msg->mandate);
      continue;
    }
    if (msg->type == RELAIS_MSG_ORDRE) {
      if (s->holder != idx || msg->mandate != s->term) continue;
      if (msg->tick != (relais_tick)tick) continue;
      command_ok = true;
      command = msg->value;
      command_node = idx;
    }
  }
  if (command_ok) {
    out->value = relais_clamp_value(command, &s->params);
    out->fresh = true;
    out->fresh_node = command_node;
  } else {
    relais_meas rm;
    rm.pos = m->pos;
    rm.vel = m->vel;
    rm.applied = m->applied;
    rm.setpoint = m->setpoint;
    out->value = relais_clamp_value(relais_fallback_step(&s->fb_state, &rm, &s->params),
                                     &s->params);
    out->fresh = false;
    out->fresh_node = BL_NONE;
  }
  out->commander = s->holder;
  out->term = s->term;
  out->expiry = s->expiry;
  s->last_value = out->value;
  s->have_last = true;
}

static void ls_node_init(bl_node *nd, int idx, const relais_params *p, uint32_t timeout) {
  ls_node *s = (ls_node *)nd->s.raw;
  memset(s, 0, sizeof *s);
  s->params = *p;
  s->lease_ticks = timeout > 0 ? timeout : 1u;
  s->index = (uint8_t)idx;
  s->peer = (uint8_t)(1u - (uint32_t)idx);
  s->term = 0;
  s->local_expiry = 0;
  relais_ctl_init(&s->ctl, p);
}

static bool ls_import(ls_node *s, const relais_msg *m) {
  if (s->have_etat && m->tick <= s->last_etat) return false;
  if (!relais_ctl_import(&s->ctl, m->ctl_state, m->ctl_state_len)) return false;
  s->last_etat = (uint32_t)m->tick;
  s->have_etat = true;
  return true;
}

static void ls_emit(ls_node *s, uint32_t target, float value, bl_node_out *out) {
  if (out->count >= BL_OUT_MAX) return;
  relais_msg *m = &out->m[out->count++];
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ORDRE;
  m->sender = ls_node_of(s->index);
  m->tick = (relais_tick)target;
  m->mandate = s->term;
  m->value = value;
  m->has_order = true;
  out->commands = true;
  out->command = value;
}

static void ls_emit_renewal(ls_node *s, uint32_t tick, bl_node_out *out) {
  if (out->count >= BL_OUT_MAX) return;
  relais_msg *m = &out->m[out->count++];
  relais_msg_clear(m);
  m->type = RELAIS_MSG_OMBRE;
  m->sender = ls_node_of(s->index);
  m->tick = (relais_tick)tick;
  m->mandate = s->term;
}

static void ls_emit_request(ls_node *s, bl_node_out *out) {
  if (out->count >= BL_OUT_MAX) return;
  relais_msg *m = &out->m[out->count++];
  relais_msg_clear(m);
  m->type = RELAIS_MSG_DEMANDE;
  m->sender = ls_node_of(s->index);
  m->mandate = s->term + 1u;
  m->seen_mandate = s->term;
  s->candidate = true;
}

static void ls_emit_state(ls_node *s, uint32_t tick, bl_node_out *out) {
  if (out->count >= BL_OUT_MAX) return;
  uint8_t snapshot[RELAIS_CTL_STATE_MAX];
  uint8_t len = relais_ctl_export(&s->ctl, snapshot, sizeof snapshot);
  relais_msg *m = &out->m[out->count++];
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ETAT;
  m->sender = ls_node_of(s->index);
  m->tick = (relais_tick)tick;
  m->ctl_state_len = len;
  if (len > 0) memcpy(m->ctl_state, snapshot, len);
}

/* The role of the computer is read from the registrar, never claimed. A
 * leader that stops hearing the registrar keeps its lease until the expiry
 * it last saw, and then it steps down (the safety of the lease). */
static void ls_role(ls_node *s, const bl_status *st) {
  if (st->commander == s->index) {
    s->leader = true;
    s->candidate = false;
    if (st->term > s->term) s->term = st->term;
    s->local_expiry = st->expiry;
    return;
  }
  if (st->term > s->term) s->term = st->term;
  s->leader = false;
  if (st->commander == s->peer) s->candidate = false;
}

static void ls_node_step(bl_node *nd, int idx, const bl_status *st, const relais_msg *peer,
                         size_t n_peer, bl_node_out *out) {
  (void)idx;
  ls_node *s = (ls_node *)nd->s.raw;
  if (!st->heard) {
    /* The computer does not hear the registrar: its own clock decides. */
    if (s->leader && st->tick >= s->local_expiry) s->leader = false;
    return;
  }
  for (size_t i = 0; i < n_peer; i++)
    if (peer[i].type == RELAIS_MSG_ETAT) ls_import(s, &peer[i]);
  ls_role(s, st);
  float value = relais_clamp_value(relais_ctl_step(&s->ctl, &st->meas, &s->params),
                                    &s->params);
  s->last_value = value;
  if (s->leader) {
    if (st->tick >= s->local_expiry) {
      s->leader = false;
      return;
    }
    ls_emit_renewal(s, st->tick + 1u, out);
    ls_emit(s, st->tick + 1u, value, out);
    ls_emit_state(s, st->tick, out);
    return;
  }
  bool expired = st->expiry == 0 || st->tick >= st->expiry;
  if (expired && st->commander != s->index) ls_emit_request(s, out);
}

static void ls_node_fault(bl_node *nd, int idx, uint8_t reason, bl_node_out *out) {
  (void)idx;
  (void)reason;
  ls_node *s = (ls_node *)nd->s.raw;
  if (!s->leader) return;
  /* A clean step-down: the leader releases the lease, so the follower can
   * take over without waiting for the expiry. */
  if (out->count >= BL_OUT_MAX) return;
  relais_msg *m = &out->m[out->count++];
  relais_msg_clear(m);
  m->type = RELAIS_MSG_CESSION;
  m->sender = ls_node_of(s->index);
  m->mandate = s->term;
  m->flags = RELAIS_FLAG_CESSION;
  s->leader = false;
}

static const bl_impl LS_IMPL = {
    .name = "lease",
    .act_init = ls_act_init,
    .act_step = ls_act_step,
    .act_counters = NULL,
    .node_init = ls_node_init,
    .node_step = ls_node_step,
    .node_fault = ls_node_fault,
};

const bl_impl *bl_impl_lease(void) { return &LS_IMPL; }
