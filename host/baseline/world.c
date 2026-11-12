/* Relais — the world of the comparison: faults, channel, plant (mission M43).
 *
 * Everything here is shared by the three implementations. The channel draws
 * are computed before the run, so the Relais protocol, the heartbeat
 * baseline and the lease baseline face exactly the same network, whatever
 * they send. That is what makes the comparison about the handover logic and
 * not about the bench.
 */
#include "world.h"

#include <stdlib.h>
#include <string.h>

#define BL_DUP_THRESHOLD 1310u /* 2 % of 65536, as in the seeded simulator */

static uint64_t bl_rand(bl_world *w) {
  uint64_t x = w->rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  w->rng = x;
  return x * 0x2545f4914f6cdd1dull;
}

/* One draw per (tick, computer): the loss, the delay and the duplication of
 * that computer come out of it. */
static void bl_draws_init(bl_world *w) {
  w->draws = calloc((size_t)w->cfg.ticks * BL_NODES, sizeof *w->draws);
  if (w->draws == NULL) return;
  for (uint32_t t = 0; t < w->cfg.ticks; t++)
    for (uint32_t d = 0; d < BL_NODES; d++)
      w->draws[(size_t)t * BL_NODES + d] = bl_rand(w);
}

static uint64_t bl_draw(const bl_world *w, uint32_t tick, uint32_t domain) {
  if (w->draws == NULL) return 0;
  return w->draws[(size_t)tick * BL_NODES + domain];
}

/* The loss decision of this tick. With the burst guard of ROB-03 (the
 * hypothesis of the project) two losses in a row are impossible; with the
 * guard off the channel may lose two frames in a row, which is what a
 * timeout based baseline has to survive. */
void bl_world_channel(bl_world *w, uint32_t tick) {
  uint32_t threshold = (uint32_t)(w->cfg.loss * 65536.0);
  for (uint32_t d = 0; d < BL_NODES; d++) {
    uint64_t v = bl_draw(w, tick, d);
    bool wants = threshold > 0 && (uint32_t)(v & 0xFFFFu) < threshold;
    w->lost_now[d] = w->cfg.burst_guard ? (wants && !w->lost_prev[d]) : wants;
    w->lost_prev[d] = w->lost_now[d];
  }
}

static uint32_t bl_delay_of(const bl_world *w, uint32_t tick, uint32_t domain) {
  if (w->cfg.delay == 0) return 0;
  uint64_t v = bl_draw(w, tick, domain);
  return (uint32_t)((v >> 16) & 0xFFFFu) % (w->cfg.delay + 1);
}

static bool bl_dup_of(const bl_world *w, uint32_t tick, uint32_t domain) {
  uint64_t v = bl_draw(w, tick, domain);
  return (uint32_t)((v >> 32) & 0xFFFFu) < BL_DUP_THRESHOLD;
}

float bl_world_setpoint(const bl_world *w, uint32_t tick) {
  double seconds = (double)(tick > 0 ? tick - 1 : 0) * relais_params_step_s(&w->params);
  return relais_traj_setpoint(&w->traj, seconds);
}

bool bl_world_handover_at(const bl_world *w, uint32_t tick, int idx) {
  for (size_t i = 0; i < w->cfg.fault_count; i++) {
    const bl_fault *f = &w->cfg.faults[i];
    if (f->at == tick && f->kind == BL_F_HANDOVER && (int)f->node == idx) return true;
  }
  return false;
}

void bl_world_faults(bl_world *w, uint32_t tick) {
  for (size_t i = 0; i < w->cfg.fault_count; i++) {
    const bl_fault *f = &w->cfg.faults[i];
    if (f->at != tick) continue;
    bl_nfault *nf = &w->nf[f->node];
    switch (f->kind) {
      case BL_F_DEATH: nf->dead = true; break;
      case BL_F_FREEZE: nf->frozen_until = tick + f->arg; break;
      case BL_F_CUT_ACT:
        if (f->arg != 2) nf->cut_act = true;
        if (f->arg != 1) nf->deaf = true;
        break;
      case BL_F_CUT_PEER: nf->cut_peer = true; break;
      case BL_F_SLOW: nf->slow_every = f->arg > 1 ? f->arg : 3; break;
      case BL_F_RESTORE:
        nf->cut_act = false;
        nf->deaf = false;
        nf->cut_peer = false;
        nf->slow_every = 0;
        break;
      case BL_F_HANDOVER: nf->frozen_until = tick + f->arg; break;
      default: break;
    }
  }
}

static void bl_queue(bl_world *w, const relais_msg *m, uint32_t deliver_at) {
  if (w->qcount >= BL_QUEUE_MAX) return;
  bl_pending *p = &w->queue[w->qcount++];
  p->msg = *m;
  p->deliver_at = deliver_at;
  p->used = false;
}

size_t bl_world_collect(bl_world *w, uint32_t tick, relais_msg *out, size_t cap) {
  size_t n = 0;
  uint32_t keep = 0;
  for (uint32_t i = 0; i < w->qcount; i++) {
    bl_pending *p = &w->queue[i];
    if (p->used) continue;
    if (p->deliver_at <= tick) {
      p->used = true;
      if (n < cap) out[n++] = p->msg;
      continue;
    }
    if (keep != i) w->queue[keep] = *p;
    keep++;
  }
  w->qcount = keep;
  return n;
}

void bl_world_send(bl_world *w, int idx, const relais_msg *m, uint32_t tick, bool same_tick) {
  bl_nfault *nf = &w->nf[idx];
  if (nf->cut_act) return;
  if (w->lost_now[idx]) return;
  uint32_t at = same_tick ? tick : tick + 1 + bl_delay_of(w, tick, (uint32_t)idx);
  bl_queue(w, m, at);
  if (bl_dup_of(w, tick, (uint32_t)idx)) bl_queue(w, m, at); /* ROB-02 */
}

size_t bl_world_peer(bl_world *w, int idx, const bl_node_out *out, relais_msg *buf) {
  size_t n = 0;
  for (uint8_t i = 0; i < out->count; i++) {
    const relais_msg *m = &out->m[i];
    if (!relais_msg_to_peer(m->type)) continue;
    if (w->nf[idx].cut_peer || w->lost_now[idx]) continue;
    if (n < BL_OUT_MAX) buf[n++] = *m;
  }
  return n;
}

bl_world *bl_world_new(const bl_config *cfg) {
  bl_world *w = calloc(1, sizeof *w);
  if (w == NULL) return NULL;
  w->cfg = *cfg;
  w->impl = cfg->impl;
  w->twin = cfg->twin;
  relais_params_default(&w->params);
  relais_traj_default(&w->traj);
  w->rng = cfg->seed != 0 ? cfg->seed : 0x9e3779b97f4a7c15ull;
  bl_draws_init(w);
  relais_plant_init(&w->plant, 0.0f, w->traj.seed);
  w->impl->act_init(&w->act, &w->params, cfg->timeout);
  for (uint32_t i = 0; i < BL_NODES; i++)
    w->impl->node_init(&w->node[i], (int)i, &w->params, cfg->timeout);
  return w;
}

void bl_world_free(bl_world *w) {
  if (w == NULL) return;
  free(w->draws);
  free(w);
}
