/* Relais — lifecycle and time fault world (mission M19, spec 7.4).
 *
 * One deterministic tick pipeline over the pure core, with an environment that
 * carries the lifecycle faults of ideas 13 (clock), 14 (power loss, truncated
 * frame), 15 (boot loop), 19 (demand storm) and 20 (OTA interrupted). The
 * actuator stays the only writer of the mandate and the only source of time.
 */
#include "faultlife.h"

#include <string.h>

static relais_node self_of(int idx) {
  return idx == 0 ? RELAIS_NODE_A : RELAIS_NODE_B;
}

static float setpoint_at(const lw_t *w, uint32_t tick) {
  double seconds = (double)(tick > 0 ? tick - 1 : 0) * relais_params_period_s(&w->params);
  return relais_traj_setpoint(&w->traj, seconds);
}

void lw_init(lw_t *w, const relais_params *p, const relais_traj *t) {
  memset(w, 0, sizeof *w);
  w->params = *p;
  w->traj = *t;
  relais_arb_init(&w->arb, p);
  relais_plant_init(&w->plant, 0.0f, t->seed);
  relais_node_init(&w->node[0], p, RELAIS_NODE_A);
  relais_node_init(&w->node[1], p, RELAIS_NODE_B);
  for (int i = 0; i < LW_NODES; i++) {
    w->wd_num[i] = 1;
    w->wd_den[i] = 1;
  }
}

void lw_restart_node(lw_t *w, int idx) {
  relais_node_init(&w->node[idx], &w->params, self_of(idx));
  w->dead[idx] = false;
  w->wd_acc[idx] = 0;
}

void lw_power_loss(lw_t *w, int idx, bool mid_send) {
  w->dead[idx] = true;
  if (!mid_send) return;
  /* The wire was cut mid-byte (14): every frame from this machine still in
   * flight is partial and never decodes, so it never reaches the actuator. */
  for (uint32_t i = 0; i < w->pending_count; i++) {
    if (w->pending[i].used) continue;
    if (w->pending[i].msg.sender == self_of(idx)) {
      w->pending[i].used = true;
      w->truncated_frames++;
    }
  }
}

void lw_set_boot_period(lw_t *w, int idx, uint32_t k) { w->boot_period[idx] = k; }
void lw_set_storm(lw_t *w, int idx, bool on) { w->storm[idx] = on; }

void lw_set_watchdog(lw_t *w, int idx, int num, int den) {
  w->wd_num[idx] = num > 0 ? num : 1;
  w->wd_den[idx] = den > 0 ? den : 1;
  w->wd_acc[idx] = 0;
}

void lw_watchdog_jump(lw_t *w, int idx, uint32_t at) {
  w->wd_jump_at[idx] = at;
  w->wd_jumped[idx] = false;
}

/* How many silence (bail) ticks this machine's local clock counts for one real
 * silent tick. A 2x clock counts 2, a 0.5x clock counts every other tick, and a
 * clock that jumps after a gel counts the whole bail horizon at once. */
static int silence_calls(lw_t *w, int idx, uint32_t tick) {
  if (w->wd_jump_at[idx] != 0 && !w->wd_jumped[idx] && tick >= w->wd_jump_at[idx]) {
    w->wd_jumped[idx] = true;
    return w->params.bail_ticks;
  }
  w->wd_acc[idx] += w->wd_num[idx];
  int calls = w->wd_acc[idx] / w->wd_den[idx];
  w->wd_acc[idx] %= w->wd_den[idx];
  return calls;
}

static void run_node(lw_t *w, int idx, const relais_msg *statut, relais_node_out *out,
                     uint32_t tick) {
  relais_node_clear_out(out);
  if (w->dead[idx]) return;
  if (tick <= w->boot_dead_until[idx]) return; /* 15: the machine is rebooting */
  if (w->deaf[idx]) {
    int calls = silence_calls(w, idx, tick);
    for (int c = 0; c < calls; c++)
      relais_node_on_silence(&w->node[idx], (relais_tick)tick, out);
    return;
  }
  w->wd_acc[idx] = 0;
  relais_node_on_status(&w->node[idx], statut, out);
}

static void deliver_peer(lw_t *w, int peer_idx, const relais_node_out *out) {
  relais_node_out ignored;
  for (uint8_t i = 0; i < out->count; i++) {
    const relais_msg *m = &out->out[i];
    if (!relais_msg_to_peer(m->type)) continue;
    if (m->type == RELAIS_MSG_ETAT)
      relais_node_on_etat(&w->node[peer_idx], m, &ignored);
    else
      relais_node_on_suivi(&w->node[peer_idx], m, &ignored);
  }
}

static void queue_act(lw_t *w, const relais_msg *m, uint32_t deliver_at) {
  if (w->pending_count >= LW_MAX_PENDING) {
    w->queue_drops++;
    return;
  }
  lw_pending_t *p = &w->pending[w->pending_count++];
  p->msg = *m;
  p->deliver_at = deliver_at;
  p->used = false;
}

static void deliver_actuator(lw_t *w, const relais_node_out *out, uint32_t tick) {
  for (uint8_t i = 0; i < out->count; i++) {
    const relais_msg *m = &out->out[i];
    if (relais_msg_to_peer(m->type)) continue;
    queue_act(w, m, tick + 1);
  }
}

static size_t collect(lw_t *w, uint32_t tick, relais_msg *msgs, size_t cap) {
  size_t n = 0;
  size_t keep = 0;
  for (uint32_t i = 0; i < w->pending_count; i++) {
    lw_pending_t *p = &w->pending[i];
    if (p->used) continue;
    if (p->deliver_at <= tick) {
      p->used = true;
      if (n < cap) msgs[n++] = p->msg;
      continue;
    }
    if (keep < LW_MAX_PENDING) w->pending[keep++] = *p;
  }
  w->pending_count = (uint32_t)keep;
  return n;
}

/* 19: a forced demande, emitted every tick regardless of the node's suspicion. */
static void make_storm_demande(lw_t *w, int idx, uint32_t tick, relais_msg *m) {
  relais_node_sm *s = &w->node[idx];
  relais_msg_clear(m);
  m->type = RELAIS_MSG_DEMANDE;
  m->sender = self_of(idx);
  m->tick = (relais_tick)(tick + 1);
  m->seen_mandate = s->seen_mandate;
  m->reason = 1;
  m->value = 0.0f;
  m->has_order = true;
  m->flags = RELAIS_FLAG_HAS_ORDER;
  relais_testament_fill(&s->ctl, &s->last_meas, &w->params, m->testament, &m->testament_len);
}

/* 20: a clean voluntary handover starts the OTA life cycle (REL-09). */
bool lw_ota_cede(lw_t *w, int idx) {
  relais_node_out out;
  relais_node_on_handover(&w->node[idx], &out);
  deliver_peer(w, 1 - idx, &out);
  deliver_actuator(w, &out, (uint32_t)w->node[idx].now);
  return out.count > 0;
}

/* The holder computes first so its ETAT reaches the second within the same
 * tick (spec 4.2); the second then shadows a fresh state. */
static void run_nodes(lw_t *w, relais_arb_out *out, uint32_t tick) {
  int holder = out->holder == RELAIS_NODE_B ? 1 : 0;
  int other = 1 - holder;
  relais_node_out o1;
  relais_node_out o2;
  run_node(w, holder, &out->statut, &o1, tick);
  deliver_peer(w, other, &o1);
  deliver_actuator(w, &o1, tick);
  run_node(w, other, &out->statut, &o2, tick);
  deliver_peer(w, holder, &o2);
  deliver_actuator(w, &o2, tick);
}

/* 19: a storming machine emits a forced demande every tick. */
static void apply_storm(lw_t *w, uint32_t tick) {
  for (int i = 0; i < LW_NODES; i++) {
    if (w->storm[i] && !w->dead[i]) {
      relais_msg dm;
      make_storm_demande(w, i, tick, &dm);
      queue_act(w, &dm, tick + 1);
    }
  }
}

static void track_out(lw_t *w, const relais_arb_out *out) {
  if (out->holder != w->last_holder) {
    w->mandate_changes++;
    w->last_holder = out->holder;
  }
  for (uint8_t e = 0; e < out->event_count; e++) {
    switch (out->events[e].kind) {
      case RELAIS_EV_MANDATE_GRANTED: w->granted++; break;
      case RELAIS_EV_REQUEST_DENIED:
        w->denied++;
        if (out->events[e].detail == 1) w->rate_limited++;
        break;
      default: break;
    }
  }
}

void lw_update(lw_t *w, uint32_t tick, relais_arb_out *out) {
  for (int i = 0; i < LW_NODES; i++) {
    if (w->boot_period[i] != 0 && tick > 0 && (tick % w->boot_period[i]) == 0) {
      lw_restart_node(w, i);
      w->boot_dead_until[i] = tick + LW_REBOOT_DOWN; /* 15: down while it reboots */
    }
  }

  relais_msg msgs[RELAIS_IN_MSG_MAX];
  size_t n = collect(w, tick, msgs, RELAIS_IN_MSG_MAX);

  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  meas.pos = w->plant.pos;
  meas.vel = w->plant.vel;
  meas.setpoint = setpoint_at(w, tick);
  relais_arb_step(&w->arb, (relais_tick)tick, msgs, n, &meas, out);
  float dt = (float)relais_params_period_s(&w->params);
  relais_plant_step(&w->plant, out->value,
                    relais_traj_disturbance(&w->traj, (double)tick * (double)dt), dt);

  run_nodes(w, out, tick);
  apply_storm(w, tick);
  track_out(w, out);
}