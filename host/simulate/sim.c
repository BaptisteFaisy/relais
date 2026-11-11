/* Relais — the tick pipeline of the seeded simulator (spec 7.4).
 *
 * The same core is used three times over (spec 4.10): here it drives two
 * machines and one actuator in a discrete-event loop where a seed fixes every
 * draw (frame delays, losses, fault instants). The twin without fault runs in
 * parallel and gives the reference the continuity invariant I5 is measured on.
 */
#include "sim.h"

#include <string.h>

static float setpoint_at(const sim_t *s, uint32_t tick) {
  double seconds = (double)(tick > 0 ? tick - 1 : 0) * relais_params_step_s(&s->params);
  return relais_traj_setpoint(&s->traj, seconds);
}

uint64_t sim_rand(sim_t *s) {
  uint64_t x = s->rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  s->rng = x;
  return x * 0x2545f4914f6cdd1dull;
}

bool sim_chance(sim_t *s, double probability) {
  if (probability <= 0.0) return false;
  if (probability >= 1.0) return true;
  double draw = (double)(sim_rand(s) >> 11) / 9007199254740992.0;
  return draw < probability;
}

/* ROB-03: the loss of frames of one link, without a burst longer than p, must
 * not change the holder. The model therefore never loses two frames in a row
 * on the same link when p is 1. */
static bool sim_lose(sim_t *s, int link) {
  if (!sim_chance(s, s->loss)) return false;
  if (s->lost_last[link]) return false;
  s->lost_last[link] = true;
  return true;
}

void sim_init(sim_t *s, const relais_params *p, const relais_traj *t, uint64_t seed,
              bool complete) {
  memset(s, 0, sizeof *s);
  s->params = *p;
  s->traj = *t;
  s->complete = complete;
  s->rng = seed != 0 ? seed : 0x9e3779b97f4a7c15ull;
  relais_arb_init(&s->arb, p);
  relais_plant_init(&s->plant, 0.0f, t->seed);
  relais_node_init(&s->nodes[0].sm, p, RELAIS_NODE_A);
  relais_node_init(&s->nodes[1].sm, p, RELAIS_NODE_B);
  s->nodes[0].frozen_until = 0;
  s->nodes[1].frozen_until = 0;
}

static size_t collect(sim_t *s, uint32_t tick, relais_msg *msgs, size_t cap) {
  size_t n = 0;
  size_t keep = 0;
  for (size_t i = 0; i < s->pending_count; i++) {
    sim_pending_t *p = &s->to_act[i];
    if (p->used) continue;
    if (p->deliver_at <= tick) {
      p->used = true;
      if (n < cap) msgs[n++] = p->msg;
      continue;
    }
    if (p->deliver_at <= tick + 4 && keep < SIM_MAX_PENDING) s->to_act[keep++] = *p;
  }
  s->pending_count = (uint32_t)keep;
  return n;
}

static void queue_to_actuator(sim_t *s, const relais_msg *m, uint32_t deliver_at) {
  if (s->pending_count >= SIM_MAX_PENDING) return;
  sim_pending_t *p = &s->to_act[s->pending_count++];
  p->msg = *m;
  p->deliver_at = deliver_at + (s->max_delay > 0 ? (uint32_t)(sim_rand(s) % (s->max_delay + 1)) : 0);
  p->used = false;
}

static bool node_can_receive(const sim_node_t *nd, uint32_t tick) {
  return !nd->dead && nd->frozen_until < tick;
}

/* ETAT and SUIVI travel between the two computers inside the same tick: the
 * holder computes first, the second then shadows it on a fresh state. */
void sim_deliver_outputs(sim_t *s, sim_node_t *from, sim_node_t *peer,
                         const relais_node_out *out, uint32_t tick, uint32_t deliver_at) {
  for (uint8_t i = 0; i < out->count; i++) {
    const relais_msg *m = &out->out[i];
    bool to_peer = (m->type == RELAIS_MSG_ETAT || m->type == RELAIS_MSG_SUIVI);
    /* Level S has no shadow: the handover then goes through the will, which is
     * what the three-period bound of REL-01 is about. */
    if (!s->complete && m->type == RELAIS_MSG_OMBRE) continue;
    if (to_peer) {
      if (from->cut_peer || !node_can_receive(peer, tick)) continue;
      if (sim_lose(s, from == &s->nodes[0] ? 0 : 1)) continue;
      relais_node_out ignored;
      if (m->type == RELAIS_MSG_ETAT) relais_node_on_etat(&peer->sm, m, &ignored);
      else relais_node_on_suivi(&peer->sm, m, &ignored);
      continue;
    }
    if (from->cut_act) continue;
    if (sim_lose(s, from == &s->nodes[0] ? 0 : 1)) continue;
    if (sim_chance(s, 0.02)) queue_to_actuator(s, m, deliver_at); /* duplication, ROB-02 */
    queue_to_actuator(s, m, deliver_at);
  }
}

static void run_node(sim_t *s, int idx, const relais_msg *statut, relais_node_out *out,
                     uint32_t tick) {
  sim_node_t *nd = &s->nodes[idx];
  relais_node_clear_out(out);
  if (nd->dead) return;                       /* F1: nothing happens any more */
  if (nd->frozen_until >= tick) return;       /* F2: the machine is frozen */
  if (nd->deaf || sim_lose(s, idx) ||
      (nd->slow_every > 0 && (tick % nd->slow_every) == 0)) {
    relais_node_on_silence(&nd->sm, (relais_tick)tick, out); /* MAN-09 bail */
    return;
  }
  relais_node_on_status(&nd->sm, statut, out);
  nd->tick_count++;
}

void sim_run_tick(sim_t *s, uint32_t tick, relais_arb_out *out) {
  relais_msg msgs[RELAIS_IN_MSG_MAX];
  size_t n = collect(s, tick, msgs, RELAIS_IN_MSG_MAX);
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  meas.pos = s->plant.pos;
  meas.vel = s->plant.vel;
  meas.setpoint = setpoint_at(s, tick);
  relais_arb_step(&s->arb, (relais_tick)tick, msgs, n, &meas, out);
  float dt = (float)relais_params_step_s(&s->params);
  relais_plant_step(&s->plant, out->value,
                    relais_traj_disturbance(&s->traj, (double)tick * (double)dt), dt);
  out->statut.meas.pos = s->plant.pos;
  out->statut.meas.vel = s->plant.vel;
  out->statut.meas.applied = out->value;

  /* The holder computes first: its ETAT reaches the second inside the same
   * tick, so the shadow is computed on a fresh state (spec 4.2, 4.8). */
  int holder = out->statut.holder == RELAIS_NODE_B ? 1 : 0;
  int other = holder == 0 ? 1 : 0;
  relais_node_out outs[2];
  run_node(s, holder, &out->statut, &outs[holder], tick);
  sim_deliver_outputs(s, &s->nodes[holder], &s->nodes[other], &outs[holder], tick, tick + 1);
  run_node(s, other, &out->statut, &outs[other], tick);
  sim_deliver_outputs(s, &s->nodes[other], &s->nodes[holder], &outs[other], tick, tick + 1);
}

void sim_write_header(FILE *f, const sim_t *s, const char *scenario, uint32_t id) {
  if (f == NULL) return;
  relais_jr_header h;
  memset(&h, 0, sizeof h);
  h.params = s->params;
  h.mutant = relais_mutant_id();
  h.scenario_id = id;
  snprintf(h.scenario, sizeof h.scenario, "%s", scenario != NULL ? scenario : "simulate");
  char line[RELAIS_JOURNAL_LINE_MAX];
  if (relais_journal_format_header(&h, line, sizeof line) > 0) fprintf(f, "%s\n", line);
}

void sim_write_tick(FILE *f, const relais_arb_out *out, uint32_t tick, const relais_plant *pl,
                    float setpoint) {
  if (f == NULL) return;
  relais_jr_tick r;
  memset(&r, 0, sizeof r);
  r.tick = tick;
  r.mandate = out->mandate;
  r.holder = out->holder;
  r.source = out->source;
  r.value = out->value;
  r.pos = pl->pos;
  r.vel = pl->vel;
  r.setpoint = setpoint;
  r.misses = out->misses;
  r.lamp = out->lamp;
  r.cause = out->cause;
  r.certified = out->certified;
  r.certified_node = out->certified_node;
  r.stop_requested = out->stop_requested;
  char line[RELAIS_JOURNAL_LINE_MAX];
  if (relais_journal_format_tick(&r, line, sizeof line) > 0) fprintf(f, "%s\n", line);
}

void sim_write_events(FILE *f, const relais_arb_out *out, uint32_t tick) {
  if (f == NULL) return;
  for (uint8_t i = 0; i < out->event_count; i++) {
    relais_jr_event e;
    memset(&e, 0, sizeof e);
    e.tick = tick;
    e.kind = out->events[i].kind;
    e.node = out->events[i].node;
    e.mandate = out->events[i].mandate;
    e.detail = out->events[i].detail;
    snprintf(e.text, sizeof e.text, "%s", relais_event_name(e.kind));
    char line[RELAIS_JOURNAL_LINE_MAX];
    if (relais_journal_format_event(&e, line, sizeof line) > 0) fprintf(f, "%s\n", line);
  }
}
