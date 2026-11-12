/* Relais — the physics of the explored world (spec 7.3).
 *
 * One tick runs exactly like the seeded simulator does: the actuator collects
 * what arrived, decides the one value it applies, the rack moves, the STATUT
 * goes out, the holder computes first and hands its state to the second, then
 * the second computes. The difference is that nothing is drawn at random: the
 * explorer enumerates the choices instead.
 */
#include "world.h"

#include <string.h>

static double tick_seconds(const explore_world *w, uint32_t tick) {
  return (double)(tick > 0 ? tick - 1 : 0) * relais_params_period_s(&w->params);
}

static void queue(explore_world *w, explore_link link, const relais_msg *m,
                  uint32_t deliver_at) {
  for (int s = 0; s < EXPLORE_SLOTS; s++) {
    if (w->slot[link][s].busy) continue;
    w->slot[link][s].msg = *m;
    w->slot[link][s].deliver_at = deliver_at;
    w->slot[link][s].busy = true;
    return;
  }
}

static bool node_runs(const explore_world *w, int i) {
  return !w->dead[i] && w->frozen_until[i] < w->tick;
}

static void feed_invariants(explore_world *w, const relais_arb_out *out, float setpoint) {
  for (uint8_t e = 0; e < out->event_count; e++) {
    relais_jr_event ev;
    memset(&ev, 0, sizeof ev);
    ev.tick = w->tick;
    ev.kind = out->events[e].kind;
    ev.node = out->events[e].node;
    ev.mandate = out->events[e].mandate;
    ev.detail = out->events[e].detail;
    relais_inv_on_event(&w->inv, &ev);
  }
  relais_jr_tick rec;
  memset(&rec, 0, sizeof rec);
  rec.tick = w->tick;
  rec.mandate = out->mandate;
  rec.holder = out->holder;
  rec.source = out->source;
  rec.value = out->value;
  rec.pos = w->plant.pos;
  rec.vel = w->plant.vel;
  rec.setpoint = setpoint;
  rec.misses = out->misses;
  rec.lamp = out->lamp;
  rec.cause = out->cause;
  rec.certified = out->certified;
  rec.certified_node = out->certified_node;
  rec.stop_requested = out->stop_requested;
  relais_inv_on_tick(&w->inv, &rec, NULL); /* no twin here: I5 is level 2's job */
  relais_inv_end_tick(&w->inv, w->tick);
}

/* Deliver what the actuator received, decide, move the rack, publish. */
static void actuator_beat(explore_world *w) {
  relais_msg msgs[EXPLORE_MSG_MAX];
  size_t n = 0;
  const explore_link uplinks[2] = {LINK_A_ACT, LINK_B_ACT};
  for (int l = 0; l < 2; l++) {
    for (int s = 0; s < EXPLORE_SLOTS; s++) {
      explore_slot *sl = &w->slot[uplinks[l]][s];
      if (!sl->busy) continue;
      if (w->cut[uplinks[l]] || sl->deliver_at > w->tick) continue;
      if (n < EXPLORE_MSG_MAX) msgs[n++] = sl->msg;
      sl->busy = false;
    }
  }
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  meas.pos = w->plant.pos;
  meas.vel = w->plant.vel;
  meas.setpoint = relais_traj_setpoint(&w->traj, tick_seconds(w, w->tick + 1));
  relais_arb_out out;
  relais_arb_step(&w->arb, (relais_tick)w->tick, msgs, n, &meas, &out);
  w->source = out.source;
  float dt = (float)relais_params_period_s(&w->params);
  relais_plant_step(&w->plant, out.value,
                    relais_traj_disturbance(&w->traj, tick_seconds(w, w->tick)), dt);
  out.statut.meas.pos = w->plant.pos;
  out.statut.meas.vel = w->plant.vel;
  out.statut.meas.applied = out.value;
  feed_invariants(w, &out, meas.setpoint);

  /* The STATUT goes out on both downlinks; a cut link simply loses it. */
  if (!w->cut[LINK_ACT_A]) queue(w, LINK_ACT_A, &out.statut, w->tick);
  if (!w->cut[LINK_ACT_B]) queue(w, LINK_ACT_B, &out.statut, w->tick);
}

/* One computer reads its STATUT and puts its frames in flight. */
static void computer_beat(explore_world *w, int i, explore_link down, explore_link up,
                          explore_link peer_up) {
  relais_node_out out;
  relais_node_clear_out(&out);
  if (!node_runs(w, i)) return;
  relais_msg statut;
  bool got = false;
  for (int s = 0; s < EXPLORE_SLOTS; s++) {
    explore_slot *sl = &w->slot[down][s];
    if (!sl->busy || sl->deliver_at > w->tick) continue;
    statut = sl->msg;
    got = true;
    sl->busy = false;
    break;
  }
  if (!got) {
    relais_node_on_silence(&w->node[i], (relais_tick)w->tick, &out);
  } else {
    relais_node_on_status(&w->node[i], &statut, &out);
  }
  for (uint8_t k = 0; k < out.count; k++) {
    const relais_msg *m = &out.out[k];
    if (m->type == RELAIS_MSG_ETAT || m->type == RELAIS_MSG_SUIVI) {
      if (w->cut[peer_up]) continue;
      int other = i == 0 ? 1 : 0;
      relais_node_out ignored;
      if (!node_runs(w, other)) continue;
      if (m->type == RELAIS_MSG_ETAT) relais_node_on_etat(&w->node[other], m, &ignored);
      else relais_node_on_suivi(&w->node[other], m, &ignored);
      continue;
    }
    if (!w->complete && m->type == RELAIS_MSG_OMBRE) continue; /* level S has no shadow */
    if (w->cut[up]) continue;
    queue(w, up, m, w->tick + 1);
  }
}

static void world_tick(explore_world *w) {
  w->tick++;
  w->pending_mutations = 0;
  w->channel_link = -1;
  w->burst_this_tick = 0;
  actuator_beat(w);
  /* The holder computes first so that its state reaches the second inside the
   * same tick (spec 4.2): that is what makes the shadow equal to the order. */
  int first = w->arb.holder == RELAIS_NODE_B ? 1 : 0;
  int second = first == 0 ? 1 : 0;
  computer_beat(w, first, first == 0 ? LINK_ACT_A : LINK_ACT_B,
                first == 0 ? LINK_A_ACT : LINK_B_ACT, first == 0 ? LINK_A_B : LINK_B_A);
  computer_beat(w, second, second == 0 ? LINK_ACT_A : LINK_ACT_B,
                second == 0 ? LINK_A_ACT : LINK_B_ACT, second == 0 ? LINK_A_B : LINK_B_A);
}

void explore_world_init(explore_world *w, const relais_params *p, const relais_traj *t,
                        bool complete, bool expect_certified) {
  memset(w, 0, sizeof *w);
  w->params = *p;
  w->traj = *t;
  w->complete = complete;
  relais_arb_init(&w->arb, p);
  relais_plant_init(&w->plant, 0.0f, t->seed);
  relais_node_init(&w->node[0], p, RELAIS_NODE_A);
  relais_node_init(&w->node[1], p, RELAIS_NODE_B);
  w->channel_link = -1;
  relais_inv_init(&w->inv, p);
  relais_inv_expect_certified(&w->inv, expect_certified);
  relais_inv_fault_free(&w->inv, true);
}

static void drop_first(explore_world *w, int link) {
  for (int s = 0; s < EXPLORE_SLOTS; s++)
    if (w->slot[link][s].busy) { w->slot[link][s].busy = false; return; }
}

static void delay_first(explore_world *w, int link) {
  for (int s = 0; s < EXPLORE_SLOTS; s++)
    if (w->slot[link][s].busy) { w->slot[link][s].deliver_at++; return; }
}

static void duplicate_first(explore_world *w, int link) {
  explore_slot *src = NULL;
  for (int s = 0; s < EXPLORE_SLOTS; s++)
    if (w->slot[link][s].busy) { src = &w->slot[link][s]; break; }
  if (src == NULL) return;
  for (int s = 0; s < EXPLORE_SLOTS; s++) {
    if (w->slot[link][s].busy) continue;
    w->slot[link][s] = *src;
    return;
  }
}

/* A frame that is dropped or delayed is a channel condition (spec 3.3, F6):
 * it is tolerated inside the parameter bounds, but it is not "no fault at all",
 * so I7 stops applying and the gap bound relaxes to p+1 -- exactly what the
 * seeded simulator does. DET-03 keeps guarding against a wrongful handover:
 * the mandate still needs p+1 misses of the actuator's own counting. */
static void note_channel_fault(explore_world *w) {
  relais_inv_lossy_channel(&w->inv, true);
  relais_inv_fault_free(&w->inv, false);
}

void explore_world_apply(explore_world *w, const explore_event *ev) {
  switch (ev->kind) {
    case EX_TICK: world_tick(w); return;
    case EX_DROP:
      drop_first(w, ev->link);
      note_channel_fault(w);
      w->channel_link = (int8_t)ev->link;
      w->burst_this_tick++;
      break;
    case EX_DELAY:
      delay_first(w, ev->link);
      note_channel_fault(w);
      w->channel_link = (int8_t)ev->link;
      w->burst_this_tick++;
      break;
    case EX_DUP: duplicate_first(w, ev->link); break;
    case EX_KILL:
      w->dead[ev->node] = true;
      relais_inv_fault_free(&w->inv, false); /* a fault was injected: I7 is out */
      break;
    case EX_FREEZE1:
      w->frozen_until[ev->node] = w->tick + 1;
      relais_inv_fault_free(&w->inv, false);
      break;
    case EX_FREEZE3:
      w->frozen_until[ev->node] = w->tick + 3;
      relais_inv_fault_free(&w->inv, false);
      break;
    case EX_THAW: w->frozen_until[ev->node] = 0; break;
    case EX_RESTART:
      w->dead[ev->node] = false;
      w->frozen_until[ev->node] = 0;
      relais_node_init(&w->node[ev->node], &w->params, w->node[ev->node].self);
      break;
    case EX_CUT:
      w->cut[ev->link] = true;
      w->channel_link = (int8_t)ev->link;
      w->burst_this_tick++;
      relais_inv_fault_free(&w->inv, false);
      note_channel_fault(w);
      break;
    case EX_RESTORE: w->cut[ev->link] = false; break;
    default: return;
  }
  if (w->pending_mutations < 255) w->pending_mutations++;
}

uint32_t explore_active_faults(const explore_world *w) {
  uint32_t n = 0;
  for (int i = 0; i < EXPLORE_NODES; i++) {
    if (w->dead[i] || w->frozen_until[i] > w->tick) n++;
  }
  for (int l = 0; l < EXPLORE_LINKS; l++)
    if (w->cut[l]) n++;
  return n;
}

int explore_world_enumerate(const explore_world *w, explore_event *out, int cap,
                            int fault_budget) {
  int n = 0;
  explore_event *e;
#define PUSH(kind_, link_, node_)                    \
  do {                                               \
    if (n >= cap) return n;                          \
    e = &out[n++];                                   \
    e->kind = (kind_);                               \
    e->link = (link_);                               \
    e->node = (node_);                               \
  } while (0)

  /* The tick is enumerated LAST on purpose: the depth-first search then dives
   * into the paths where a fault is injected before the next tick beats, which
   * is where the interesting interleavings live. The pure-tick path is still
   * explored, one level later. */
  if (w->pending_mutations >= EXPLORE_MUT_MAX) {
    PUSH(EX_TICK, -1, -1);
    return n;
  }

  /* One fault at a time, machine OR channel (spec 3.3): a persistent machine
   * fault occupies the whole budget, and so does a loss inflicted in this
   * window. Killing A while drowning B's uplink is two faults, and no bound is
   * claimed against two. */
  uint32_t machine_faults = explore_active_faults(w);
  bool channel_busy = w->burst_this_tick > 0;
  bool faults_allowed = (int)machine_faults + (channel_busy ? 1 : 0) < fault_budget;
  for (int l = 0; l < EXPLORE_LINKS; l++) {
    bool busy = false;
    for (int s = 0; s < EXPLORE_SLOTS; s++) if (w->slot[l][s].busy) busy = true;
    /* One disrupted link at a time: the single-fault hypothesis covers the
     * channel too, otherwise two cut cables are indistinguishable from a
     * double fault and no bound can be claimed (spec 3.3, 7.3 item 5). */
    bool this_channel = w->channel_link < 0 || (int)w->channel_link == l;
    if (busy) {
      /* ROB-03: no burst longer than p. A link that flaps cut/restore/cut
       * inside one period is exactly such a burst, and no bound can be claimed
       * against it, so the adversary is held to the same rule as the channel
       * model of the specification. Restoring is always allowed: it heals. */
      bool burst_ok = w->burst_this_tick < w->params.patience &&
                      (faults_allowed || channel_busy);
      if (this_channel && burst_ok) {
        PUSH(EX_DROP, l, -1);
        PUSH(EX_DELAY, l, -1);
      }
      PUSH(EX_DUP, l, -1);
    }
    if (w->cut[l]) PUSH(EX_RESTORE, l, -1);
    else if (faults_allowed && this_channel && w->burst_this_tick < w->params.patience)
      PUSH(EX_CUT, l, -1);
  }
  for (int i = 0; i < EXPLORE_NODES; i++) {
    if (w->dead[i]) {
      if (faults_allowed) PUSH(EX_RESTART, -1, i);
      continue;
    }
    if (w->frozen_until[i] > w->tick) {
      PUSH(EX_THAW, -1, i);
      continue;
    }
    if (!faults_allowed) continue;
    PUSH(EX_KILL, -1, i);
    PUSH(EX_FREEZE1, -1, i);
    PUSH(EX_FREEZE3, -1, i);
  }
  PUSH(EX_TICK, -1, -1);
#undef PUSH
  return n;
}

uint64_t explore_world_hash(const explore_world *w) {
  const uint64_t prime = 0x100000001b3ull;
  uint64_t h = 0xcbf29ce484222325ull;
  const unsigned char *p;
  size_t len;
#define ABSORB(ptr, size)              \
  p = (const unsigned char *)(ptr);    \
  len = (size);                        \
  for (size_t i = 0; i < len; i++) {   \
    h ^= p[i];                         \
    h *= prime;                        \
  }
  ABSORB(&w->tick, sizeof w->tick);
  ABSORB(&w->arb, sizeof w->arb);
  ABSORB(&w->node, sizeof w->node);
  ABSORB(&w->plant, sizeof w->plant);
  ABSORB(&w->slot, sizeof w->slot);
  ABSORB(&w->dead, sizeof w->dead);
  ABSORB(&w->frozen_until, sizeof w->frozen_until);
  ABSORB(&w->cut, sizeof w->cut);
  ABSORB(&w->pending_mutations, sizeof w->pending_mutations);
  ABSORB(&w->channel_link, sizeof w->channel_link);
  ABSORB(&w->burst_this_tick, sizeof w->burst_this_tick);
#undef ABSORB
  return h;
}

bool explore_world_ok(const explore_world *w) { return relais_inv_ok(&w->inv); }

const char *explore_kind_name(explore_kind k) {
  static const char *names[EX_KIND_COUNT] = {"tick", "drop", "delay", "duplicate", "kill",
                                             "freeze1", "freeze3", "thaw", "restart",
                                             "cut", "restore"};
  return k < EX_KIND_COUNT ? names[k] : "?";
}

const char *explore_link_name(explore_link l) {
  static const char *names[EXPLORE_LINKS] = {"A->actuator", "B->actuator", "actuator->A",
                                             "actuator->B", "A->B", "B->A"};
  return l < EXPLORE_LINKS ? names[l] : "?";
}

/* One half-link, as the scenario format of CONTRACT.md names it. Shared by the
 * explorer's counter-example dump and by the reducer's scenario writer, so the
 * two never disagree on a link name (no duplication). */
void explore_link_scenario(int link, const char **name, const char **direction) {
  static const char *names[EXPLORE_LINKS] = {"A-actuator", "B-actuator", "A-actuator",
                                             "B-actuator", "A-B", "A-B"};
  static const char *dirs[EXPLORE_LINKS] = {"forward", "forward", "reverse",
                                            "reverse", "forward", "reverse"};
  if (link < 0 || link >= EXPLORE_LINKS) {
    *name = "A-B";
    *direction = "both";
    return;
  }
  *name = names[link];
  *direction = dirs[link];
}
