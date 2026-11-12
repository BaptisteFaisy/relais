/* Relais — baseline 1: heartbeat and watchdog.
 *
 * The most widespread way to keep a redundant pair alive: a static priority
 * (A is the primary, B is the standby), a heartbeat every tick and a silence
 * timeout. The two timeouts quoted for the field in docs/SPEC.md section 1.4
 * (100 ms for the guardian, 2000 ms for the infusion pump) are exactly this
 * mechanism, and the timeout is the only knob of the family.
 *
 * How it is implemented here:
 *   - the primary sends its order to the actuator and its state (ETAT) to the
 *     peer every tick: that frame is the heartbeat the standby watches;
 *   - the standby counts the ticks without a heartbeat; after `timeout` ticks
 *     of silence it declares the primary dead and starts commanding;
 *   - the actuator selects the source with a watchdog relay: it keeps the
 *     primary while the primary is fresh and switches to the standby after
 *     `timeout` ticks without a fresh command from the primary;
 *   - when the primary comes back the relay is re-energised and the primary
 *     commands again. Nobody tells the primary that it was deposed: for one
 *     tick both computers command. That is the weakness the lease family
 *     removes with its term, and it is measured, not asserted.
 *
 * What this baseline deliberately does NOT have (it is not Relais in disguise):
 *   - the actuator counts no evidence, it only times out;
 *   - no mandate is published, so the computers cannot know who is in charge
 *     (the status frame carries the measurements only);
 *   - the standby sends nothing to the actuator while it is a standby: no
 *     shadow, hence a tick without a fresh command during the handover.
 */
#include "proto.h"

#include "relais/control.h"

#include <string.h>

typedef struct {
  uint32_t timeout;
  uint8_t mux;            /* the source the relay selects */
  uint32_t stale[BL_NODES];
  float fb_state;
  float last_value;
  bool have_last;
  relais_params params;
} hb_act;

typedef struct {
  uint32_t timeout;
  uint8_t index;
  uint8_t peer;
  bool primary;
  bool active;        /* the standby has taken over */
  uint32_t silence;   /* ticks without a heartbeat from the primary */
  uint32_t last_etat; /* the last state received from the peer */
  bool have_etat;
  relais_ctl_state ctl;
  float last_value;
  relais_params params;
} hb_node;

static uint8_t hb_index(relais_node node) {
  if (node == RELAIS_NODE_A) return 0u;
  if (node == RELAIS_NODE_B) return 1u;
  return BL_NONE;
}

static relais_node hb_node_of(uint8_t idx) {
  return idx == 0u ? RELAIS_NODE_A : RELAIS_NODE_B;
}

static void hb_act_init(bl_act *a, const relais_params *p, uint32_t timeout) {
  hb_act *s = (hb_act *)a->s.raw;
  memset(s, 0, sizeof *s);
  s->params = *p;
  s->timeout = timeout > 0 ? timeout : 1u;
  s->mux = 0u; /* the primary holds the relay */
  relais_fallback_init(&s->fb_state, NULL);
}

static void hb_act_step(bl_act *a, uint32_t tick, const relais_msg *msgs, size_t n,
                        const bl_meas *m, bl_act_out *out) {
  hb_act *s = (hb_act *)a->s.raw;
  bool fresh[BL_NODES] = {false, false};
  float value[BL_NODES] = {0.0f, 0.0f};
  for (size_t i = 0; i < n; i++) {
    const relais_msg *msg = &msgs[i];
    if (msg->type != RELAIS_MSG_ORDRE) continue;
    uint8_t idx = hb_index(msg->sender);
    if (idx == BL_NONE) continue;
    if (msg->tick != (relais_tick)tick) continue; /* a late order is dropped */
    fresh[idx] = true;
    value[idx] = msg->value;
  }
  for (uint32_t i = 0; i < BL_NODES; i++)
    s->stale[i] = fresh[i] ? 0u : s->stale[i] + 1u;
  /* The watchdog relay: it drops out after the timeout, and it is
   * re-energised by the first fresh command of the primary. */
  if (s->mux == 0u) {
    if (s->stale[0] >= s->timeout) s->mux = 1u;
  } else if (s->stale[0] == 0u) {
    s->mux = 0u;
  }
  uint8_t src = s->mux;
  if (fresh[src]) {
    out->value = relais_clamp_value(value[src], &s->params);
    out->fresh = true;
    out->fresh_node = src;
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
  out->commander = src;
  s->last_value = out->value;
  s->have_last = true;
}

static void hb_node_init(bl_node *nd, int idx, const relais_params *p, uint32_t timeout) {
  hb_node *s = (hb_node *)nd->s.raw;
  memset(s, 0, sizeof *s);
  s->params = *p;
  s->timeout = timeout > 0 ? timeout : 1u;
  s->index = (uint8_t)idx;
  s->peer = (uint8_t)(1u - (uint32_t)idx);
  s->primary = idx == 0;
  s->last_etat = 0;
  relais_ctl_init(&s->ctl, p);
}

/* The state of the holder, imported before computing (REP-01): the standby
 * then computes the very same value as the primary (CMD-03). */
static bool hb_import(hb_node *s, const relais_msg *m) {
  if (s->have_etat && m->tick <= s->last_etat) return false;
  if (!relais_ctl_import(&s->ctl, m->ctl_state, m->ctl_state_len)) return false;
  s->last_etat = (uint32_t)m->tick;
  s->have_etat = true;
  return true;
}

/* The watchdog of the standby: silence longer than the timeout means the
 * primary is dead. A clean step-down (the SUIVI frame with the cession flag)
 * means it is going away on purpose: the standby takes over at once. */
static void hb_watchdog(hb_node *s, bool alive, bool stepping_down) {
  if (s->primary) return;
  if (stepping_down) {
    s->active = true;
    s->silence = 0;
    return;
  }
  if (alive) {
    s->silence = 0;
    s->active = false; /* the primary is back: stand down */
    return;
  }
  s->silence++;
  if (s->silence >= s->timeout) s->active = true;
}

static void hb_emit_order(hb_node *s, uint32_t target, float value, bl_node_out *out) {
  if (out->count >= BL_OUT_MAX) return;
  relais_msg *m = &out->m[out->count++];
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ORDRE;
  m->sender = hb_node_of(s->index);
  m->tick = (relais_tick)target;
  m->value = value;
  m->has_order = true;
  out->commands = true;
  out->command = value;
}

static void hb_emit_state(hb_node *s, uint32_t tick, bl_node_out *out) {
  if (out->count >= BL_OUT_MAX) return;
  uint8_t snapshot[RELAIS_CTL_STATE_MAX];
  uint8_t len = relais_ctl_export(&s->ctl, snapshot, sizeof snapshot);
  relais_msg *m = &out->m[out->count++];
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ETAT;
  m->sender = hb_node_of(s->index);
  m->tick = (relais_tick)tick;
  m->ctl_state_len = len;
  if (len > 0) memcpy(m->ctl_state, snapshot, len);
}

static void hb_node_step(bl_node *nd, int idx, const bl_status *st, const relais_msg *peer,
                         size_t n_peer, bl_node_out *out) {
  (void)idx;
  hb_node *s = (hb_node *)nd->s.raw;
  if (!st->heard) return; /* the node missed its tick: it cannot compute */
  bool alive = false;
  bool stepping_down = false;
  for (size_t i = 0; i < n_peer; i++) {
    if (peer[i].type == RELAIS_MSG_ETAT) {
      alive = true;
      hb_import(s, &peer[i]);
    } else if (peer[i].type == RELAIS_MSG_SUIVI && (peer[i].flags & RELAIS_FLAG_CESSION)) {
      stepping_down = true;
    }
  }
  hb_watchdog(s, alive, stepping_down);
  float value = relais_ctl_step(&s->ctl, &st->meas, &s->params);
  value = relais_clamp_value(value, &s->params);
  s->last_value = value;
  if (s->primary || s->active) {
    hb_emit_order(s, st->tick + 1u, value, out);
    hb_emit_state(s, st->tick, out);
  }
}

static void hb_node_fault(bl_node *nd, int idx, uint8_t reason, bl_node_out *out) {
  (void)idx;
  (void)reason;
  hb_node *s = (hb_node *)nd->s.raw;
  if (!s->primary && !s->active) return;
  /* A clean step-down: the primary tells the peer it is going away, so the
   * standby can take over at once. The actuator, though, still waits for its
   * own watchdog: that is the cost of the family, and it is measured. */
  if (out->count >= BL_OUT_MAX) return;
  relais_msg *m = &out->m[out->count++];
  relais_msg_clear(m);
  m->type = RELAIS_MSG_SUIVI;
  m->sender = hb_node_of(s->index);
  m->tick = (relais_tick)s->last_etat;
  m->flags = RELAIS_FLAG_CESSION;
  s->active = false;
  s->silence = 0;
}

static const bl_impl HB_IMPL = {
    .name = "heartbeat",
    .act_init = hb_act_init,
    .act_step = hb_act_step,
    .act_counters = NULL,
    .node_init = hb_node_init,
    .node_step = hb_node_step,
    .node_fault = hb_node_fault,
};

const bl_impl *bl_impl_heartbeat(void) { return &HB_IMPL; }
