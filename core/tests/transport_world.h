/* Relais — one whole system driven over an arbitrary transport (mission M32).
 *
 * Two computers, one actuator, the rack model, the invariant checker of the
 * core: the tick loop of host/simulate/sim.c reduced to what a transport test
 * needs, and written ONCE. It runs in two modes, chosen by the caller:
 *
 *   direct   the frames are passed by function call, no medium at all;
 *   wired    every frame is encoded, pushed through the three transports that
 *            `wire` installed, received, authenticated and decoded.
 *
 * Both modes must produce the same decision at every tick. That comparison, and
 * not a comment, is the proof that the protocol does not depend on the medium.
 *
 * Included by core/tests/test_transport.c (memory transport) and by
 * host/transport/test_can.c (fragmented CAN). Not part of the library.
 */
#ifndef RELAIS_TESTS_TRANSPORT_WORLD_H
#define RELAIS_TESTS_TRANSPORT_WORLD_H

#include <stdio.h>
#include <string.h>

#include "relais/invariants.h"
#include "relais/transport.h"
#include "test_util.h"

/* ---------------------- one whole system, with or without a real medium ---- */

#define SCEN_TICKS 200u
#define SCEN_DEATH 120u /* computer A dies: the mandate must go to B */

typedef struct {
  float value;
  uint8_t source;
  uint8_t holder;
  uint8_t misses;
  uint8_t lamp;
  uint8_t cause;
  uint8_t event_count;
  uint8_t kinds[RELAIS_EVENTS_MAX];
  relais_mandate mandate;
} trace_tick;

typedef struct {
  relais_params params;
  relais_traj traj;
  relais_plant plant;
  relais_arb arb;
  relais_node_sm sm[2];
  relais_key key;
  relais_inv inv;
  relais_transport tr[3]; /* 0 actuator, 1 computer A, 2 computer B */
  bool via;      /* true: every frame crosses the transport installed by wire */
  bool dead[2];
  uint8_t scratch[RELAIS_FRAME_MAX];
  uint8_t rx[RELAIS_FRAME_MAX];
  relais_msg act_in[RELAIS_IN_MSG_MAX];
  size_t act_n;
  relais_msg node_in[2][RELAIS_IN_MSG_MAX];
  size_t node_n[2];
  trace_tick trace[SCEN_TICKS];
  uint32_t frames;     /* frames that crossed a medium */
  uint32_t rejected;   /* frames the medium delivered but the core refused */
  uint32_t switches;   /* mandate_auto_switch events seen */
} world_t;

/* Who installs the three endpoints. Each test file brings its own medium, so
 * the same scenario runs over the memory transport and over fragmented CAN
 * without a single duplicated line. */
typedef void (*world_wire_fn)(world_t *w);

static void w_queue_act(world_t *w, const relais_msg *m) {
  if (w->act_n < RELAIS_IN_MSG_MAX) w->act_in[w->act_n++] = *m;
}

static void w_queue_node(world_t *w, int i, const relais_msg *m) {
  if (w->node_n[i] < RELAIS_IN_MSG_MAX) w->node_in[i][w->node_n[i]++] = *m;
}

/* The actuator publishes STATUT on both of its links, one per computer. */
static void w_publish(world_t *w, const relais_msg *statut) {
  if (!w->via) {
    w_queue_node(w, 0, statut);
    w_queue_node(w, 1, statut);
    return;
  }
  size_t len = relais_frame_encode(statut, &w->key, w->scratch, sizeof w->scratch);
  if (len == 0) return;
  for (int l = 0; l < RELAIS_LINK_COUNT; l++)
    if (relais_tr_send_raw(&w->tr[0], (relais_link)l, w->scratch, len) == RELAIS_TR_OK)
      w->frames++;
}

/* One output frame of a computer: spec 4.9 decides where it goes. */
static void w_route(world_t *w, int from, const relais_msg *m) {
  if (w->via) {
    if (relais_tr_send_msg(&w->tr[1 + (size_t)from], &w->key, m, w->scratch,
                           sizeof w->scratch) == RELAIS_TR_OK)
      w->frames++;
    return;
  }
  if (relais_msg_to_peer(m->type)) w_queue_node(w, from == 0 ? 1 : 0, m);
  else w_queue_act(w, m);
}

/* Drain one inbox into decoded messages: the direct mode copies, the wired mode
 * receives, authenticates and decodes. Same result, same order. */
static size_t w_gather(world_t *w, relais_transport *t, relais_msg *inbox, size_t *n_in,
                       relais_msg *msgs, size_t cap) {
  if (!w->via) {
    size_t n = *n_in < cap ? *n_in : cap;
    for (size_t i = 0; i < n; i++) msgs[i] = inbox[i];
    *n_in = 0;
    return n;
  }
  size_t n = 0;
  while (n < cap) {
    relais_msg m;
    relais_link link = RELAIS_LINK_ACTUATOR;
    relais_reject why = RELAIS_REJ_NONE;
    if (relais_tr_recv_msg(t, &w->key, &link, &m, &why, w->rx, sizeof w->rx) != RELAIS_TR_OK)
      break;
    msgs[n++] = m;
  }
  return n;
}

static void w_record(world_t *w, relais_tick tick, const relais_arb_out *out) {
  trace_tick *t = &w->trace[tick - 1];
  t->value = out->value;
  t->source = (uint8_t)out->source;
  t->holder = (uint8_t)out->holder;
  t->misses = out->misses;
  t->lamp = (uint8_t)out->lamp;
  t->cause = (uint8_t)out->cause;
  t->mandate = out->mandate;
  t->event_count = out->event_count;
  for (uint8_t e = 0; e < out->event_count; e++) {
    t->kinds[e] = (uint8_t)out->events[e].kind;
    relais_jr_event ev;
    memset(&ev, 0, sizeof ev);
    ev.tick = tick;
    ev.kind = out->events[e].kind;
    ev.node = out->events[e].node;
    ev.mandate = out->events[e].mandate;
    ev.detail = out->events[e].detail;
    if (ev.kind == RELAIS_EV_MANDATE_AUTO_SWITCH) w->switches++;
    relais_inv_on_event(&w->inv, &ev);
  }
  relais_jr_tick rec;
  memset(&rec, 0, sizeof rec);
  rec.tick = tick;
  rec.mandate = out->mandate;
  rec.holder = out->holder;
  rec.source = out->source;
  rec.value = out->value;
  rec.pos = w->plant.pos;
  rec.vel = w->plant.vel;
  rec.setpoint = out->statut.meas.setpoint;
  rec.misses = out->misses;
  rec.lamp = out->lamp;
  rec.cause = out->cause;
  rec.certified = out->certified;
  rec.certified_node = out->certified_node;
  rec.stop_requested = out->stop_requested;
  relais_inv_on_tick(&w->inv, &rec, NULL);
  relais_inv_end_tick(&w->inv, tick);
}

static void w_node_beat(world_t *w, int i, relais_tick tick) {
  if (w->dead[i]) return; /* F1: the machine is gone, nothing is sent */
  relais_msg in[RELAIS_IN_MSG_MAX];
  size_t n = w_gather(w, &w->tr[1 + (size_t)i], w->node_in[i], &w->node_n[i], in,
                      RELAIS_IN_MSG_MAX);
  relais_node_out o, ignored;
  relais_node_clear_out(&o);
  for (size_t k = 0; k < n; k++) {
    if (in[k].type == RELAIS_MSG_STATUT) {
      relais_node_on_status(&w->sm[i], &in[k], &o);
    } else {
      /* ETAT and SUIVI carry no output of their own (REP-01, REP-02). */
      relais_node_clear_out(&ignored);
      if (in[k].type == RELAIS_MSG_ETAT) relais_node_on_etat(&w->sm[i], &in[k], &ignored);
      else if (in[k].type == RELAIS_MSG_SUIVI) relais_node_on_suivi(&w->sm[i], &in[k], &ignored);
    }
  }
  if (n == 0) relais_node_on_silence(&w->sm[i], tick, &o); /* MAN-09 */
  for (uint8_t k = 0; k < o.count; k++) w_route(w, i, &o.out[k]);
}

static void w_tick(world_t *w, relais_tick tick) {
  relais_msg msgs[RELAIS_IN_MSG_MAX];
  size_t n = w_gather(w, &w->tr[0], w->act_in, &w->act_n, msgs, RELAIS_IN_MSG_MAX);
  const double dt = relais_params_step_s(&w->params);
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  meas.pos = w->plant.pos;
  meas.vel = w->plant.vel;
  meas.setpoint = relais_traj_setpoint(&w->traj, (double)(tick - 1) * dt);
  relais_arb_out out;
  relais_arb_step(&w->arb, tick, msgs, n, &meas, &out);
  relais_plant_step(&w->plant, out.value, relais_traj_disturbance(&w->traj, (double)tick * dt),
                    (float)dt);
  out.statut.meas.pos = w->plant.pos;
  out.statut.meas.vel = w->plant.vel;
  w_record(w, tick, &out);
  w_publish(w, &out.statut);
  w_node_beat(w, 0, tick);
  w_node_beat(w, 1, tick);
}

/* Level C (complete): two computers, the shadow of the certified second, one
 * death at SCEN_DEATH. Identical in every mode; only the medium differs. Pass
 * wire = NULL for the direct mode, where no frame crosses any transport. */
static void run_scenario(world_t *w, world_wire_fn wire) {
  memset(w, 0, sizeof *w);
  relais_params_default(&w->params);
  relais_traj_default(&w->traj);
  w->via = wire != NULL;
  relais_key_from_hex(&w->key, "0f1e2d3c4b5a69788796a5b4c3d2e1f0"); /* SEC-02 on */
  relais_arb_init(&w->arb, &w->params);
  relais_plant_init(&w->plant, 0.0f, w->traj.seed);
  relais_node_init(&w->sm[0], &w->params, RELAIS_NODE_A);
  relais_node_init(&w->sm[1], &w->params, RELAIS_NODE_B);
  relais_inv_init(&w->inv, &w->params);
  relais_inv_expect_certified(&w->inv, true);
  relais_inv_fault_free(&w->inv, false); /* a death is injected: I7 is out */
  if (wire != NULL) wire(w);
  for (relais_tick tick = 1; tick <= SCEN_TICKS; tick++) {
    if (tick == SCEN_DEATH) w->dead[0] = true;
    w_tick(w, tick);
  }
}

static uint32_t count_source(const world_t *w, relais_source s) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < SCEN_TICKS; i++)
    if (w->trace[i].source == (uint8_t)s) n++;
  return n;
}

static bool tick_identical(const trace_tick *a, const trace_tick *b) {
  return a->value == b->value && a->source == b->source && a->holder == b->holder &&
         a->misses == b->misses && a->lamp == b->lamp && a->cause == b->cause &&
         a->mandate == b->mandate && a->event_count == b->event_count &&
         memcmp(a->kinds, b->kinds, sizeof a->kinds) == 0;
}

#endif /* RELAIS_TESTS_TRANSPORT_WORLD_H */
