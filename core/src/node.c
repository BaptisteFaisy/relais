/* Relais — one computer's protocol state machine (spec 4.3, 4.6, 4.9, 5.2, 5.5).
 *
 * The node never invents a mandate: it adopts the one the actuator publishes
 * (MAN-01, SEC-04). An order names the tick it targets, so a machine waking up
 * from a freeze sends a stale order that the actuator drops and the machine
 * stands down as soon as it reads the next STATUT (spec 4.6, MAN-08, REL-07).
 *
 * A cession is sent to the actuator as a CESSION and to the peer as a SUIVI
 * carrying RELAIS_FLAG_CESSION, so death point P5 can send only one of them.
 */
#include "relais/node.h"

#include <stdio.h>
#include <string.h>

static void emit(relais_node_out *o, const relais_msg *m) {
  if (o->count < RELAIS_OUT_MSG_MAX) o->out[o->count++] = *m;
}

static bool die_here(const relais_node_sm *s, relais_death_point dp, relais_node_out *o) {
  if (s->death_point != dp) return false;
  if (s->death_at != 0 && s->now < s->death_at) return false;
  o->died = true;
  o->at = dp;
  return true;
}

static void make_order(const relais_node_sm *s, relais_tick target, float value,
                       relais_msg *m) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ORDRE;
  m->sender = s->self;
  m->tick = target;
  m->mandate = s->mandate;
  m->value = value;
  m->has_order = true;
  relais_testament_fill(&s->ctl, &s->last_meas, &s->params, m->testament, &m->testament_len);
}

/* REP-01: the state carried by ETAT is the one the holder had BEFORE computing
 * this tick's order. The second imports it, computes on the same STATUT, and
 * gets the same value bit for bit (CMD-03, DET-09). Exporting the state after
 * the computation would put the replica one tick ahead of its holder. */
static void make_etat(relais_tick tick, relais_mandate mandate, const uint8_t *state,
                      uint8_t state_len, float last_value, relais_node sender, relais_msg *m) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ETAT;
  m->sender = sender;
  m->tick = tick;
  m->mandate = mandate;
  m->last_value = last_value;
  m->ctl_state_len = state_len;
  if (state != NULL && state_len > 0) memcpy(m->ctl_state, state, state_len);
}

static void make_shadow(const relais_node_sm *s, relais_tick target, float value,
                        relais_msg *m) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_OMBRE;
  m->sender = s->self;
  m->tick = target;
  m->mandate = s->mandate;
  m->value = value;
  m->replica_age = s->replica_age;
  m->flags = (uint8_t)(s->ready ? RELAIS_FLAG_READY : 0);
}

/* MAN-05: a non-holder asks; the demande carries its first order so the
 * handover can be served in two periods instead of three (spec 4.5). */
static void make_demande(relais_node_sm *s, relais_tick target, float value, relais_msg *m) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_DEMANDE;
  m->sender = s->self;
  m->tick = target;
  m->seen_mandate = s->seen_mandate;
  m->reason = s->seen_holder == RELAIS_NODE_NONE ? 2u : 1u;
  m->value = value;
  m->has_order = true;
  m->flags = RELAIS_FLAG_HAS_ORDER;
  m->replica_age = s->replica_age;
  relais_testament_fill(&s->ctl, &s->last_meas, &s->params, m->testament, &m->testament_len);
  s->demandes_sent++;
}

static void make_suivi(const relais_node_sm *s, relais_tick tick, bool ceding, relais_msg *m) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_SUIVI;
  m->sender = s->self;
  m->tick = tick;
  m->mandate = s->seen_mandate;
  m->replica_age = s->replica_age;
  m->ready = s->ready;
  m->flags = (uint8_t)((s->ready ? RELAIS_FLAG_READY : 0) |
                       (ceding ? RELAIS_FLAG_CESSION : 0));
}

static void make_cession(const relais_node_sm *s, uint8_t reason, relais_msg *m) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_CESSION;
  m->sender = s->self;
  m->tick = s->now;
  m->mandate = s->mandate;
  m->reason = reason;
  m->flags = RELAIS_FLAG_CESSION;
}

/* One period of control: the law is a pure function of the state and of the
 * measurements published by the actuator, so both machines compute the same
 * bits (CMD-03). The bench may add a bias to inject a value fault (INJ-10). */
static float compute(relais_node_sm *s, const relais_meas *meas) {
  s->last_meas = *meas;
  float value = s->law.step != NULL ? s->law.step(s->law.ctx, meas, &s->params)
                                    : relais_ctl_step(&s->ctl, meas, &s->params);
  if (s->bias != 0.0f) value += s->bias;
  return relais_clamp_value(value, &s->params);
}

static void stand_down(relais_node_sm *s, relais_node_out *o) {
  if (s->role == RELAIS_ROLE_HOLDER) {
    s->deposed = true;
    o->deposed = true;
  }
  s->role = RELAIS_ROLE_SECOND;
  s->mandate = 0;
  s->ready = false;
  s->replica_age = 255;
}

/* Holder branch: compute, order, replicate. */
static void as_holder(relais_node_sm *s, const relais_msg *statut, relais_node_out *o) {
  relais_tick target = statut->tick + 1;
  if (die_here(s, RELAIS_DP_P1, o)) return; /* STATUT received, before computing */
  uint8_t snapshot[RELAIS_CTL_STATE_MAX];
  uint8_t snapshot_len = s->law.export_state != NULL
                             ? s->law.export_state(s->law.ctx, snapshot, sizeof snapshot)
                             : relais_ctl_export(&s->ctl, snapshot, sizeof snapshot);
  float previous = s->ctl.last_value;
  float value = compute(s, &statut->meas);
  if (die_here(s, RELAIS_DP_P2, o)) return; /* computed, before any send */
  if (s->deposed) {
    s->deposed = false; /* first tick as the holder again (REL-08) */
    if (die_here(s, RELAIS_DP_P7, o)) return;
  }
  relais_msg order;
  make_order(s, target, value, &order);
  emit(o, &order);
  s->orders_sent++;
  if (die_here(s, RELAIS_DP_P3, o)) return; /* ORDRE sent, ETAT not yet */
  relais_msg etat;
  make_etat(statut->tick, s->mandate, snapshot, snapshot_len, previous, s->self, &etat);
  emit(o, &etat);
  if (die_here(s, RELAIS_DP_P4, o)) return; /* ETAT on one link only */
  relais_msg suivi;
  make_suivi(s, statut->tick, false, &suivi);
  emit(o, &suivi);
}

/* Second branch: follow the holder, shadow it, ask when it goes quiet. */
static void as_second(relais_node_sm *s, const relais_msg *statut, relais_node_out *o,
                      bool missed_statut, bool was_unroled) {
  relais_tick target = statut->tick + 1;
  bool stale_replica = s->last_etat != RELAIS_TICK_NONE && statut->tick > s->last_etat;
  if (stale_replica) s->replica_age++;
  /* DET-06: the state link is declared lost after two ticks without an ETAT.
   * REP-04 rebuilds a state only when the replica is really unusable: the
   * second skipped ticks, or it never got a state at all. A replica that is
   * merely one tick old is exact, because both machines run the same law on
   * the same measurements (CMD-03) — replacing it would create the very bump
   * the replication exists to avoid. */
  if (stale_replica && (missed_statut || (was_unroled && s->last_etat == RELAIS_TICK_NONE))) {
    s->link_lost = true;
    s->link_lost_ticks++;
    relais_ctl_reconstruct(&s->ctl, &statut->meas, &s->params);
  } else if (!stale_replica) {
    s->link_lost_ticks = 0;
  } else {
    s->link_lost_ticks++;
    s->link_lost = s->link_lost_ticks >= 2;
  }
  if (die_here(s, RELAIS_DP_P9, o)) return; /* reintegration, before certification */
  float value = compute(s, &statut->meas);
  relais_msg shadow;
  make_shadow(s, target, value, &shadow);
  emit(o, &shadow);
  s->shadows_sent++;

  bool vacant = statut->holder == RELAIS_NODE_NONE;
  bool suspect = vacant || statut->misses > 0; /* DET-02: a suspicion only */
  /* Mutant M7: the second commands on a mere suspicion, without waiting for
   * the grant. The arbiter must absorb it (defence in depth, spec 7.6). */
  if (RELAIS_MUT(RELAIS_M7_ORDER_ON_SUSPICION) && suspect) {
    relais_msg stolen;
    make_order(s, target, value, &stolen);
    emit(o, &stolen);
    s->orders_sent++;
  }
  if (suspect && !die_here(s, RELAIS_DP_P6, o)) {
    relais_msg demande;
    make_demande(s, target, value, &demande);
    emit(o, &demande);
    o->requested = true;
  }
  relais_msg suivi;
  make_suivi(s, statut->tick, false, &suivi);
  emit(o, &suivi);
}

void relais_node_init(relais_node_sm *s, const relais_params *p, relais_node self) {
  if (s == NULL || p == NULL) return;
  memset(s, 0, sizeof *s);
  s->params = *p;
  s->self = self;
  s->role = RELAIS_ROLE_NONE;
  s->seen_holder = RELAIS_NODE_NONE;
  s->last_statut = RELAIS_TICK_NONE;
  s->last_etat = RELAIS_TICK_NONE;
  s->replica_age = 255;
  relais_ctl_init(&s->ctl, p);
}

void relais_node_clear_out(relais_node_out *o) {
  if (o == NULL) return;
  memset(o, 0, sizeof *o);
}

void relais_node_on_status(relais_node_sm *s, const relais_msg *statut, relais_node_out *o) {
  if (s == NULL || statut == NULL || o == NULL) return;
  if (statut->type != RELAIS_MSG_STATUT) return;
  relais_node_clear_out(o);
  s->now = statut->tick;
  relais_tick previous_statut = s->last_statut;
  bool missed_statut =
      previous_statut != RELAIS_TICK_NONE && statut->tick > previous_statut + 1;
  bool was_unroled = s->role == RELAIS_ROLE_NONE;
  s->last_statut = statut->tick;
  s->bail_ticks = 0;
  s->without_actuator = false;
  s->seen_mandate = statut->mandate;
  s->seen_holder = statut->holder;
  s->ready = statut->certified && statut->certified_node == s->self;

  if (statut->holder == s->self) {
    /* MAN-01: the mandate is read, never claimed. */
    s->mandate = statut->mandate;
    s->role = RELAIS_ROLE_HOLDER;
    as_holder(s, statut, o);
    return;
  }
  if (s->role == RELAIS_ROLE_HOLDER) {
    /* MAN-08 / REL-07: deposed, stop ordering before the next tick. */
    stand_down(s, o);
    if (die_here(s, RELAIS_DP_P8, o)) return;
  }
  if (s->role != RELAIS_ROLE_HOLDER) s->role = RELAIS_ROLE_SECOND;
  as_second(s, statut, o, missed_statut, was_unroled);
}

void relais_node_on_etat(relais_node_sm *s, const relais_msg *etat, relais_node_out *o) {
  if (s == NULL || etat == NULL) return;
  if (etat->type != RELAIS_MSG_ETAT || etat->sender == s->self) return;
  /* REP-02: a state older than the last one received is dropped. */
  if (s->last_etat != RELAIS_TICK_NONE && etat->tick <= s->last_etat) {
    s->stale_dropped++;
    return;
  }
  if (!RELAIS_MUT(RELAIS_M5_NO_STATE_IMPORT)) {
    bool imported = s->law.import_state != NULL
                        ? s->law.import_state(s->law.ctx, etat->ctl_state,
                                              etat->ctl_state_len)
                        : relais_ctl_import(&s->ctl, etat->ctl_state, etat->ctl_state_len);
    if (!imported) return;
  }
  s->last_etat = etat->tick;
  s->replica_age = 0;
  s->link_lost = false;
  s->link_lost_ticks = 0;
  s->now = etat->tick > s->now ? etat->tick : s->now;
  if (o != NULL) relais_node_clear_out(o);
}

void relais_node_on_suivi(relais_node_sm *s, const relais_msg *suivi, relais_node_out *o) {
  if (s == NULL || suivi == NULL || suivi->type != RELAIS_MSG_SUIVI) return;
  if (suivi->flags & RELAIS_FLAG_CESSION) {
    /* The holder is ceding: suspicion raised, but the actuator still decides
     * (REL-04). Nothing is sent here, the next STATUT tells the truth. */
    s->link_lost_ticks = 0;
  }
  if (o != NULL) relais_node_clear_out(o);
}

void relais_node_on_silence(relais_node_sm *s, relais_tick now, relais_node_out *o) {
  if (s == NULL) return;
  s->now = now;
  s->bail_ticks++;
  if (o != NULL) relais_node_clear_out(o);
  if (s->bail_ticks < s->params.bail_ticks) return;
  /* MAN-09 / DET-07: no tempo means no mandate. Safety does not depend on this
   * rule (MAN-02 is enough); it is here for the diagnosis. */
  s->without_actuator = true;
  if (s->role == RELAIS_ROLE_HOLDER) {
    s->role = RELAIS_ROLE_SECOND;
    s->mandate = 0;
    s->deposed = true;
    if (o != NULL) o->deposed = true;
  }
}

void relais_node_on_local_fault(relais_node_sm *s, uint8_t reason, relais_node_out *o) {
  if (s == NULL || o == NULL) return;
  relais_node_clear_out(o);
  if (s->role != RELAIS_ROLE_HOLDER) return;
  /* DET-05: cede in the same tick, no silence timeout. */
  relais_msg cession;
  make_cession(s, reason, &cession);
  emit(o, &cession);
  s->cessions_sent++;
  bool peer_copy = s->death_point != RELAIS_DP_P5;
  if (peer_copy) {
    relais_msg suivi;
    make_suivi(s, s->now, true, &suivi);
    emit(o, &suivi);
  }
  s->role = RELAIS_ROLE_SECOND;
  s->mandate = 0;
  if (die_here(s, RELAIS_DP_P5, o)) return;
}

void relais_node_on_handover(relais_node_sm *s, relais_node_out *o) {
  relais_node_on_local_fault(s, 2 /* maintenance, REL-09 */, o);
}

void relais_node_set_bias(relais_node_sm *s, float bias) {
  if (s != NULL) s->bias = bias;
}

void relais_node_set_law(relais_node_sm *s, const relais_law *law) {
  if (s == NULL) return;
  if (law == NULL) {
    memset(&s->law, 0, sizeof s->law);
    return;
  }
  s->law = *law;
  if (s->law.init != NULL) s->law.init(s->law.ctx, &s->params);
}

void relais_node_arm_death(relais_node_sm *s, relais_death_point dp, relais_tick at) {
  if (s == NULL) return;
  s->death_point = dp;
  s->death_at = at;
}

int relais_node_status_text(const relais_node_sm *s, char *buf, size_t cap) {
  if (s == NULL || buf == NULL || cap == 0) return -1;
  return snprintf(buf, cap,
                  "node=%s role=%s mandate=%u seen_holder=%s replica_age=%u bail=%u "
                  "link=%s without_actuator=%s orders=%u shadows=%u demandes=%u "
                  "cessions=%u stale=%u",
                  relais_node_name(s->self),
                  s->role == RELAIS_ROLE_HOLDER ? "holder"
                                                : (s->role == RELAIS_ROLE_SECOND ? "second"
                                                                                 : "none"),
                  (unsigned)s->mandate, relais_node_name(s->seen_holder),
                  (unsigned)s->replica_age, (unsigned)s->bail_ticks,
                  s->link_lost ? "lost" : "ok", s->without_actuator ? "yes" : "no",
                  (unsigned)s->orders_sent, (unsigned)s->shadows_sent,
                  (unsigned)s->demandes_sent, (unsigned)s->cessions_sent,
                  (unsigned)s->stale_dropped);
}
