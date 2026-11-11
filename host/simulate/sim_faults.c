/* Relais — the fault model of the simulator (spec 3.3, 7.4, 7.5).
 *
 * Faults are declared as "at_tick:kind:node[:arg[:value]]" on the command line
 * or generated at random for a campaign. They are applied at the start of the
 * tick, before the actuator decides, exactly like a fault that lands between
 * two periods.
 */
#include "sim.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  sim_fault_kind kind;
} fault_name_t;

static const fault_name_t FAULT_NAMES[] = {
    {"death", SIM_F_DEATH},     {"freeze", SIM_F_FREEZE},   {"thaw", SIM_F_THAW},
    {"cut-actuator", SIM_F_CUT_ACT}, {"cut-peer", SIM_F_CUT_PEER},
    {"restore", SIM_F_RESTORE}, {"slow", SIM_F_SLOW},       {"bias", SIM_F_BIAS},
    {"restart", SIM_F_RESTART}, {"crash", SIM_F_CRASH_PROC},
    {"update", SIM_F_UPDATE}, {"handover", SIM_F_HANDOVER},
    {"forge", SIM_F_FORGE}, {"forge-stale", SIM_F_FORGE_STALE},
};

/* REL-10: how long an update keeps a machine out of service when the fault
 * carries no explicit duration (the "installation" window, in ticks). */
#define SIM_UPDATE_INSTALL_TICKS 20u

static sim_fault_kind parse_kind(const char *name) {
  for (size_t i = 0; i < sizeof FAULT_NAMES / sizeof FAULT_NAMES[0]; i++)
    if (strcmp(FAULT_NAMES[i].name, name) == 0) return FAULT_NAMES[i].kind;
  return SIM_F_NONE;
}

bool sim_add_fault(sim_t *s, uint32_t at_tick, const char *kind, int node, uint32_t arg,
                   float value) {
  if (s == NULL || s->fault_count >= SIM_MAX_FAULTS) return false;
  sim_fault_kind k = parse_kind(kind);
  if (k == SIM_F_NONE) return false;
  if (node < 0 || node >= SIM_NODE_COUNT) return false;
  sim_fault_t *f = &s->faults[s->fault_count++];
  f->at_tick = at_tick;
  f->kind = k;
  f->node = node;
  f->arg = arg;
  f->value = value;
  return true;
}

/* "500:death:0" / "900:freeze:1:40" / "1200:bias:0:0:0.25" */
bool sim_parse_fault(sim_t *s, const char *spec) {
  char buf[128];
  if (spec == NULL || strlen(spec) >= sizeof buf) return false;
  strcpy(buf, spec);
  char *fields[5] = {NULL};
  int count = 0;
  for (char *save = NULL, *tok = strtok_r(buf, ":", &save); tok != NULL && count < 5;
       tok = strtok_r(NULL, ":", &save))
    fields[count++] = tok;
  if (count < 3) return false;
  uint32_t at = (uint32_t)strtoul(fields[0], NULL, 10);
  int node = atoi(fields[2]);
  uint32_t arg = count > 3 ? (uint32_t)strtoul(fields[3], NULL, 10) : 0;
  float value = count > 4 ? (float)strtod(fields[4], NULL) : 0.0f;
  return sim_add_fault(s, at, fields[1], node, arg, value);
}

static void cut_link(sim_node_t *nd, sim_fault_kind kind, uint32_t arg) {
  if (kind == SIM_F_CUT_ACT) {
    /* arg 0 = both directions, 1 = outgoing only, 2 = incoming only (F5). */
    if (arg != 2) nd->cut_act = true;
    if (arg != 1) nd->deaf = true;
    return;
  }
  nd->cut_peer = true; /* F4: the link between the two computers */
}

static void restore_links(sim_node_t *nd) {
  nd->cut_act = false;
  nd->cut_peer = false;
  nd->deaf = false;
  nd->slow_every = 0;
}

/* DEG-03: a machine comes back through relais_node_init, exactly like a reboot:
 * empty control state, no role, no mandate. It therefore returns as an unarmed
 * second and must be RECERTIFIED by the actuator before it can hold a mandate
 * again (DEG-07, spec 4.6). `clear_links` is what tells a restart of a machine
 * whose links were cut from the wake-up of a machine coming out of an update. */
static void reboot_node(sim_t *s, sim_node_t *nd, bool clear_links) {
  nd->dead = false;
  nd->frozen_until = 0;
  nd->reinit_at = 0;
  if (clear_links) restore_links(nd);
  relais_node_init(&nd->sm, &s->params, nd->sm.self);
}

/* REL-09: the machine cedes the mandate in the same tick, through the very
 * call a maintenance tool makes (relais_node_on_handover). The cession reaches
 * the actuator now, so the handover takes effect at the next tick instead of
 * waiting for the p+1 ticks of silence proof (MAN-04). A machine that is not
 * the holder has nothing to cede: the call emits nothing. */
static void cede_now(sim_t *s, sim_node_t *nd, sim_node_t *peer, uint32_t tick) {
  relais_node_out out;
  relais_node_on_handover(&nd->sm, &out);
  sim_deliver_outputs(s, nd, peer, &out, tick > 0 ? tick - 1 : 0, tick);
}

/* REL-10: one machine goes down for an update, cleanly.
 *
 * The cession comes FIRST, and that is the whole point. A machine that cedes
 * hands the actuator a piece of evidence it can act on at the very next tick
 * (REL-06), so the certified second is appointed in one tick and the shadow it
 * already sent for that tick is a fresh value (REL-05): the handover costs ONE
 * tick. Without the cession the actuator could only wait for its own proof of
 * silence, p+1 ticks without a valid order (MAN-04); the command would still be
 * served, but the handover would depend on a timeout instead of on an
 * intention. An update is a PLANNED absence, so it must not pay for a silence
 * detector.
 *
 * A machine that is only the second has no mandate to cede:
 * relais_node_on_handover emits nothing and the machine simply goes out of
 * service, which costs a yellow lamp (DEG-01) and nothing else.
 */
static void start_update(sim_t *s, sim_node_t *nd, sim_node_t *peer, const sim_fault_t *f,
                         uint32_t tick) {
  cede_now(s, nd, peer, tick);
  nd->frozen_until = tick + (f->arg > 0 ? f->arg : SIM_UPDATE_INSTALL_TICKS);
  nd->reinit_at = nd->frozen_until + 1; /* installation window, then the reboot */
}

/* The reboot of an updated machine belongs to the fault model, so it fires even
 * when no other fault is declared at that tick. */
static void pending_reboots(sim_t *s, uint32_t tick) {
  for (int i = 0; i < SIM_NODE_COUNT; i++) {
    sim_node_t *nd = &s->nodes[i];
    if (nd->reinit_at != 0 && tick >= nd->reinit_at) reboot_node(s, nd, false);
  }
}

/* Returns the number of faults active at the end of this tick: more than one
 * means the single-fault hypothesis is over (spec 3.3). */
uint32_t sim_apply_faults(sim_t *s, uint32_t tick) {
  pending_reboots(s, tick); /* REL-10: an updated machine wakes up on its own */
  for (uint32_t i = 0; i < s->fault_count; i++) {
    const sim_fault_t *f = &s->faults[i];
    if (f->at_tick != tick) continue;
    sim_node_t *nd = &s->nodes[f->node];
    sim_node_t *peer = &s->nodes[f->node == 0 ? 1 : 0];
    s->faults_applied++;
    switch (f->kind) {
      case SIM_F_DEATH: nd->dead = true; break;
      case SIM_F_FREEZE: nd->frozen_until = tick + f->arg; break;
      case SIM_F_THAW: nd->frozen_until = 0; break;
      case SIM_F_CUT_ACT:
      case SIM_F_CUT_PEER: cut_link(nd, f->kind, f->arg); break;
      case SIM_F_RESTORE:
        restore_links(nd);
        restore_links(peer);
        break;
      case SIM_F_SLOW: nd->slow_every = f->arg > 1 ? f->arg : 3; break;
      case SIM_F_BIAS: relais_node_set_bias(&nd->sm, f->value); break;
      case SIM_F_RESTART: reboot_node(s, nd, true); break; /* DEG-03, ROB-06 */
      case SIM_F_UPDATE: start_update(s, nd, peer, f, tick); break;
      case SIM_F_HANDOVER: cede_now(s, nd, peer, tick); break;
      case SIM_F_CRASH_PROC: {
        /* DET-05: a critical process dies, the machine cedes in the same tick.
         * QNX-06: the supervisor restarts it, which takes a few ticks during
         * which the machine cannot compute (ROB-07). */
        relais_node_out out;
        relais_node_on_local_fault(&nd->sm, 1, &out);
        sim_deliver_outputs(s, nd, peer, &out, tick > 0 ? tick - 1 : 0, tick);
        nd->frozen_until = tick + (f->arg > 0 ? f->arg : 10);
        break;
      }
      case SIM_F_FORGE: {
        /* SEC-04: a machine that is not the holder forges an order carrying the
         * current mandate and the current tick. The nominal arbiter drops it;
         * mutant M1 applies it, and the invariants see it at once. */
        relais_msg forged;
        relais_msg_clear(&forged);
        forged.type = RELAIS_MSG_ORDRE;
        forged.sender = f->node == 0 ? RELAIS_NODE_A : RELAIS_NODE_B;
        forged.tick = (relais_tick)tick;
        forged.mandate = s->arb.mandate;
        forged.value = f->value != 0.0f ? f->value : -0.9f;
        forged.has_order = true;
        forged.testament_len = 0;
        sim_deliver_outputs(s, nd, peer, &(relais_node_out){.count = 1, .out = {forged}},
                            tick > 0 ? tick - 1 : 0, tick);
        break;
      }
      case SIM_F_FORGE_STALE: {
        /* ACT-09: an order delayed past its tick is dropped even when it comes
         * from the holder. Mutant M2 ignores the label and applies it. */
        relais_node who = s->arb.holder;
        if (!relais_node_is_computer(who)) who = RELAIS_NODE_A;
        relais_msg stale;
        relais_msg_clear(&stale);
        stale.type = RELAIS_MSG_ORDRE;
        stale.sender = who;
        stale.tick = (relais_tick)(tick > 3 ? tick - 3 : 1);
        stale.mandate = s->arb.mandate;
        stale.value = f->value != 0.0f ? f->value : -0.9f;
        stale.has_order = true;
        int idx = who == RELAIS_NODE_B ? 1 : 0;
        sim_deliver_outputs(s, &s->nodes[idx], &s->nodes[idx == 0 ? 1 : 0],
                            &(relais_node_out){.count = 1, .out = {stale}},
                            tick > 0 ? tick - 1 : 0, tick);
        break;
      }
      case SIM_F_NONE: break;
    }
  }
  uint32_t active = 0;
  for (int i = 0; i < SIM_NODE_COUNT; i++) {
    const sim_node_t *nd = &s->nodes[i];
    if (nd->dead) active++;
    else if (nd->frozen_until >= tick) active++;
    else if (nd->cut_act || nd->cut_peer || nd->deaf) active++;
  }
  return active;
}

/* A campaign draws single faults at random instants (spec 7.5). */
void sim_seed_random_faults(sim_t *s, uint32_t count, uint32_t first_tick, uint32_t spread) {
  static const char *kinds[] = {"death", "freeze", "cut-actuator", "cut-peer", "slow"};
  for (uint32_t i = 0; i < count; i++) {
    uint32_t at = first_tick + (uint32_t)(sim_rand(s) % (spread > 0 ? spread : 1000));
    const char *kind = kinds[sim_rand(s) % 5];
    int node = (int)(sim_rand(s) % SIM_NODE_COUNT);
    uint32_t arg = strcmp(kind, "freeze") == 0 ? (uint32_t)(1 + sim_rand(s) % 20) : 3;
    sim_add_fault(s, at, kind, node, arg, 0.0f);
  }
}
