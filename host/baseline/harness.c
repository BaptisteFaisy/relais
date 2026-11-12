/* Relais — the tick loop and the measurement of the comparison (M43).
 *
 * The world (world.c) holds the computers, the links, the plant and the
 * faults; this file drives one tick and turns the run into the metrics of
 * docs/COMPARISON-MEASURED.md. The three implementations share it, so no
 * implementation can be flattered by a private bench.
 */
#include "world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BL_EPSILON 0.005f /* the agreement band of the project (spec 6.3) */

/* The trace prints one line per tick: it is how a scenario is replayed tick
 * by tick in the documentation. */
static bool bl_trace_on = false;

void bl_trace_enable(bool on) { bl_trace_on = on; }

/* One computer of this tick. The dead, the frozen and the slow ones are
 * handled here, so an implementation only sees a computer that is alive and
 * that heard the actuator (or that did not, which it must survive). */
static void bl_run_node(bl_world *w, int idx, uint32_t tick, const bl_status *st,
                        const relais_msg *peer, size_t n_peer, bl_node_out *out) {
  bl_nfault *nf = &w->nf[idx];
  memset(out, 0, sizeof *out);
  if (nf->dead) return;         /* F1 */
  if (nf->frozen_until >= tick) return; /* F2 */
  bool slow = nf->slow_every > 0 && (tick % nf->slow_every) == 0; /* F3 */
  bl_status ns = *st;
  ns.heard = !nf->deaf && !w->lost_now[idx] && !slow;
  w->impl->node_step(&w->node[idx], idx, &ns, peer, n_peer, out);
}

/* The frames a computer emits: the ones for the peer go to the peer buffer,
 * the others are queued for the actuator. */
static size_t bl_emit(bl_world *w, int idx, uint32_t tick, const bl_node_out *out,
                      relais_msg *peer_buf, bool same_tick) {
  size_t n_peer = bl_world_peer(w, idx, out, peer_buf);
  for (uint8_t i = 0; i < out->count; i++) {
    const relais_msg *m = &out->m[i];
    if (relais_msg_to_peer(m->type)) continue;
    bl_world_send(w, idx, m, tick, same_tick);
  }
  return n_peer;
}

/* A local step-down (DET-05): the frames it emits must reach the actuator
 * during the very tick they are emitted (REL-06). */
static void bl_handover(bl_world *w, uint32_t tick, relais_msg *pre, size_t *n_pre,
                        int *pre_to) {
  for (uint32_t i = 0; i < BL_NODES; i++) {
    if (!bl_world_handover_at(w, tick, (int)i)) continue;
    bl_node_out out;
    memset(&out, 0, sizeof out);
    w->impl->node_fault(&w->node[i], (int)i, 1u, &out);
    *pre_to = (int)(1u - i);
    *n_pre = bl_emit(w, (int)i, tick, &out, pre, true);
    break;
  }
}

/* What the actuator publishes to the two computers. */
static void bl_status_of(const bl_world *w, uint32_t tick, const bl_act_out *aout,
                         bl_status *st) {
  st->tick = tick;
  st->commander = aout->commander;
  st->term = aout->term;
  st->expiry = aout->expiry;
  st->misses = aout->misses;
  st->certified = aout->certified;
  st->certified_node = aout->certified_node;
  st->heard = true;
  st->meas.pos = w->plant.pos;
  st->meas.vel = w->plant.vel;
  st->meas.applied = aout->value;
  st->meas.setpoint = bl_world_setpoint(w, tick);
}

/* The two computers of this tick. The commander computes first, the other
 * one follows on its state: the same rule as the seeded simulator, for the
 * three implementations alike. */
static void bl_nodes(bl_world *w, uint32_t tick, const bl_status *st, uint8_t commander,
                      const relais_msg *pre, size_t n_pre, int pre_to, bl_tick *res) {
  int order[2] = {0, 1};
  if (commander == 1u) {
    order[0] = 1;
    order[1] = 0;
  }
  relais_msg from_first[BL_OUT_MAX];
  size_t n_from_first = 0;
  relais_msg peer_out[BL_OUT_MAX];
  for (int k = 0; k < 2; k++) {
    int idx = order[k];
    relais_msg input[BL_OUT_MAX * 2];
    size_t n_input = 0;
    if (k == 1)
      for (size_t i = 0; i < n_from_first && n_input < BL_OUT_MAX * 2; i++)
        input[n_input++] = from_first[i];
    if (pre_to == idx)
      for (size_t i = 0; i < n_pre && n_input < BL_OUT_MAX * 2; i++) input[n_input++] = pre[i];
    bl_node_out out;
    bl_run_node(w, idx, tick, st, input, n_input, &out);
    res->commands[idx] = out.commands;
    res->command[idx] = out.command;
    size_t n = bl_emit(w, idx, tick, &out, peer_out, false);
    if (k == 0) {
      for (size_t i = 0; i < n && i < BL_OUT_MAX; i++) from_first[i] = peer_out[i];
      n_from_first = n;
    }
  }
}

/* One tick of the world: the faults, the channel, the actuator, the plant,
 * then the two computers. */
static void bl_step(bl_world *w, uint32_t tick, bl_tick *res) {
  memset(res, 0, sizeof *res);
  bl_world_faults(w, tick);
  bl_world_channel(w, tick);
  relais_msg pre[BL_OUT_MAX];
  size_t n_pre = 0;
  int pre_to = -1;
  bl_handover(w, tick, pre, &n_pre, &pre_to);

  relais_msg msgs[BL_MSGS_MAX];
  size_t n = bl_world_collect(w, tick, msgs, BL_MSGS_MAX);
  bl_meas meas;
  meas.pos = w->plant.pos;
  meas.vel = w->plant.vel;
  meas.applied = 0.0f;
  meas.setpoint = bl_world_setpoint(w, tick);
  bl_act_out aout;
  memset(&aout, 0, sizeof aout);
  w->impl->act_step(&w->act, tick, msgs, n, &meas, &aout);

  float dt = (float)relais_params_step_s(&w->params);
  float disturbance = relais_traj_disturbance(&w->traj, (double)tick * (double)dt);
  relais_plant_step(&w->plant, aout.value, disturbance, dt);

  bl_status st;
  bl_status_of(w, tick, &aout, &st);
  bl_nodes(w, tick, &st, aout.commander, pre, n_pre, pre_to, res);

  if (bl_trace_on && !w->twin)
    printf("t=%u cmd=%u fresh=%d node=%u val=%.4f c0=%d c1=%d miss=%u\n", tick,
           (unsigned)aout.commander, aout.fresh ? 1 : 0, (unsigned)aout.fresh_node,
           (double)aout.value, res->commands[0] ? 1 : 0, res->commands[1] ? 1 : 0,
           (unsigned)aout.misses);
  res->value = aout.value;
  res->fresh = aout.fresh;
  res->fresh_node = aout.fresh_node;
  res->commander = aout.commander;
  res->pos = w->plant.pos;
  res->setpoint = bl_world_setpoint(w, tick);
  w->commander = aout.commander;
}

/* --- the measurement ----------------------------------------------------- */

typedef struct {
  bl_metrics *m;
  uint32_t start;
  uint32_t end;
  uint8_t fault_commander;
  float prev_value;
  bool have_prev;
  bool jump_pending;
  float value_before;
} bl_measure;

static void bl_measure_init(bl_measure *s, bl_metrics *m, uint32_t start, uint32_t end) {
  memset(s, 0, sizeof *s);
  s->m = m;
  s->start = start;
  s->end = end;
  s->fault_commander = BL_NONE;
}

/* The metrics of one tick of the window. */
static void bl_measure_tick(bl_measure *s, const bl_tick *a, const bl_tick *b, uint32_t tick) {
  bl_metrics *m = s->m;
  if (!m->handover_done && a->commander != BL_NONE && a->commander != s->fault_commander) {
    m->handover_done = true;
    m->handover_ticks = tick - s->start;
    s->jump_pending = true;
    s->value_before = s->prev_value;
  }
  if (s->jump_pending) {
    float jump = fabsf(a->value - s->value_before);
    if (jump > m->handover_jump) m->handover_jump = jump;
    s->jump_pending = false;
  }
  if (!m->fresh_done && a->fresh && a->fresh_node != s->fault_commander) {
    m->fresh_done = true;
    m->fresh_ticks = tick - s->start;
  }
  if (!a->fresh) m->stale_ticks++;
  if (a->commands[0] && a->commands[1]) {
    m->double_cmds++;
    if (fabsf(a->command[0] - a->command[1]) > BL_EPSILON) m->split_cmds++;
  }
  float gap = fabsf(a->pos - b->pos);
  if (gap > m->twin_gap_max) m->twin_gap_max = gap;
  float err = fabsf(a->pos - a->setpoint);
  if (err > m->track_err_max) m->track_err_max = err;
  if (s->have_prev) {
    float jump = fabsf(a->value - s->prev_value);
    if (jump > m->cmd_jump_max) m->cmd_jump_max = jump;
  }
  s->prev_value = a->value;
  s->have_prev = true;
}

static uint32_t bl_fault_tick(const bl_config *cfg) {
  uint32_t best = BL_TICK_NONE;
  for (size_t i = 0; i < cfg->fault_count; i++)
    if (cfg->faults[i].at < best) best = cfg->faults[i].at;
  return best;
}

void bl_run(const bl_config *cfg, bl_metrics *out) {
  memset(out, 0, sizeof *out);
  out->handover_ticks = BL_TICK_NONE;
  out->fresh_ticks = BL_TICK_NONE;
  bl_config twin_cfg = *cfg;
  twin_cfg.loss = 0.0;
  twin_cfg.delay = 0;
  twin_cfg.fault_count = 0;
  twin_cfg.twin = true;
  bl_world *w = bl_world_new(cfg);
  bl_world *twin = bl_world_new(&twin_cfg);
  if (w == NULL || twin == NULL) {
    bl_world_free(w);
    bl_world_free(twin);
    return;
  }
  uint32_t first_fault = bl_fault_tick(cfg);
  uint32_t window = cfg->window > 0 ? cfg->window : 200u;
  uint32_t start = cfg->measure_at != 0 ? cfg->measure_at
                                        : (first_fault == BL_TICK_NONE ? 1u : first_fault);
  bl_measure s;
  bl_measure_init(&s, out, start, start + window);

  for (uint32_t tick = 1; tick <= cfg->ticks; tick++) {
    if (tick == start) s.fault_commander = w->commander;
    bl_tick a;
    bl_tick b;
    bl_step(w, tick, &a);
    bl_step(twin, tick, &b);
    if (tick >= start && tick < s.end) bl_measure_tick(&s, &a, &b, tick);
    else {
      s.prev_value = a.value;
      s.have_prev = true;
    }
  }
  if (w->impl->act_counters != NULL) w->impl->act_counters(&w->act, out->counters, 2);
  bl_world_free(w);
  bl_world_free(twin);
}

/* --- the fault syntax ---------------------------------------------------- */

const char *bl_fault_kind_name(bl_fault_kind kind) {
  switch (kind) {
    case BL_F_DEATH: return "death";
    case BL_F_FREEZE: return "freeze";
    case BL_F_CUT_ACT: return "cut-actuator";
    case BL_F_CUT_PEER: return "cut-peer";
    case BL_F_SLOW: return "slow";
    case BL_F_RESTORE: return "restore";
    case BL_F_HANDOVER: return "handover";
    default: return "none";
  }
}

static bool bl_kind_from_name(const char *name, bl_fault_kind *out) {
  static const struct {
    const char *name;
    bl_fault_kind kind;
  } kinds[] = {
      {"death", BL_F_DEATH},
      {"freeze", BL_F_FREEZE},
      {"cut-actuator", BL_F_CUT_ACT},
      {"cut-peer", BL_F_CUT_PEER},
      {"slow", BL_F_SLOW},
      {"restore", BL_F_RESTORE},
      {"handover", BL_F_HANDOVER},
  };
  for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
    if (strcmp(name, kinds[i].name) == 0) {
      *out = kinds[i].kind;
      return true;
    }
  }
  return false;
}

bool bl_fault_parse(bl_config *cfg, const char *spec) {
  char buf[96];
  if (spec == NULL || strlen(spec) >= sizeof buf) return false;
  strcpy(buf, spec);
  char *fields[4] = {NULL, NULL, NULL, NULL};
  int count = 0;
  for (char *save = NULL, *tok = strtok_r(buf, ":", &save); tok != NULL && count < 4;
       tok = strtok_r(NULL, ":", &save))
    fields[count++] = tok;
  if (count < 3 || cfg->fault_count >= BL_MAX_FAULTS) return false;
  bl_fault_kind kind;
  if (!bl_kind_from_name(fields[1], &kind)) return false;
  bl_fault *f = &cfg->faults[cfg->fault_count++];
  f->at = (uint32_t)strtoul(fields[0], NULL, 10);
  f->kind = kind;
  f->node = (uint8_t)strtoul(fields[2], NULL, 10);
  f->arg = count > 3 ? (uint32_t)strtoul(fields[3], NULL, 10) : 0;
  return f->node < BL_NODES;
}
