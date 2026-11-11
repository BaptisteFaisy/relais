/* Relais — the actuator's arbiter (spec 4.3, 4.4, 5.2, 5.6, 5.7).
 *
 * The actuator is the only writer of the mandate. It revokes a mandate only on
 * its own evidence: p+1 consecutive ticks without a valid order from the holder
 * (MAN-04), or a CESSION from it. Two machines that no longer agree therefore
 * cannot both command: whatever they believe, only the order of the node the
 * actuator appointed, for the tick to come, is applied (MAN-02, I1, I3).
 *
 * One step per tick, in this order:
 *   1. scan the frames received during the previous period,
 *   2. count the miss,
 *   3. revoke when misses > p (auto-switch to a certified second, MAN-10),
 *   4. CESSION then DEMANDE,
 *   5. pick the one value applied at this tick (spec 4.4),
 *   6. certification, lamp, degraded timer,
 *   7. fill STATUT and the journal events.
 */
#include "relais/arbiter.h"

#include "relais/control.h"

#include <math.h>
#include <string.h>

#define SNAPSHOT_MAGIC 0x52415242u /* "RARB" */
#define SNAPSHOT_VERSION 1u
#define LIVENESS_TICKS 8u

typedef struct {
  const relais_msg *order;
  const relais_msg *shadow;
  const relais_msg *cession;
  const relais_msg *demande;
  bool order_valid;
} scan_t;

static void push_event(relais_arb_out *out, relais_event_kind kind, relais_node node,
                       relais_mandate mandate, relais_tick tick, uint32_t detail) {
  if (out->event_count >= RELAIS_EVENTS_MAX) return;
  relais_event *e = &out->events[out->event_count++];
  e->kind = kind;
  e->node = node;
  e->mandate = mandate;
  e->tick = tick;
  e->detail = detail;
}

static void reject(relais_arb *a, relais_arb_out *out, relais_tick tick, relais_reject why,
                   const relais_msg *m) {
  if ((size_t)why < RELAIS_REJECT_SLOTS) a->rejected[why]++;
  push_event(out, RELAIS_EV_ORDER_REJECTED, m->sender, m->mandate, tick, (uint32_t)why);
}

/* MAN-02: an order counts only if it comes from the holder, carries the current
 * mandate and targets the tick being applied. Anything else is dropped, counted
 * and journaled with its reason. */
static bool order_is_valid(const relais_arb *a, const relais_msg *m, relais_tick tick,
                           relais_reject *why) {
  if (RELAIS_MUT(RELAIS_M1_ACCEPT_ANY)) { /* mutant: anybody is the holder */
    if (!RELAIS_MUT(RELAIS_M2_IGNORE_TICK) && m->tick != tick) {
      *why = RELAIS_REJ_TICK_MISSED;
      return false;
    }
    return true;
  }
  if (m->sender != a->holder) {
    *why = RELAIS_REJ_NOT_HOLDER;
    return false;
  }
  if (m->mandate > a->mandate) {
    *why = RELAIS_REJ_MANDATE_FUTURE; /* SEC-04: nobody appoints itself */
    return false;
  }
  if (m->mandate != a->mandate) {
    *why = RELAIS_REJ_MANDATE_STALE; /* the returning machine, spec 4.6 */
    return false;
  }
  if (!RELAIS_MUT(RELAIS_M2_IGNORE_TICK) && m->tick != tick) {
    *why = RELAIS_REJ_TICK_MISSED;
    return false;
  }
  return true;
}

static void scan(relais_arb *a, relais_arb_out *out, relais_tick tick, const relais_msg *msgs,
                 size_t n, scan_t *s) {
  memset(s, 0, sizeof *s);
  for (size_t i = 0; i < n; i++) {
    const relais_msg *m = &msgs[i];
    if (relais_node_is_computer(m->sender)) a->last_heard[m->sender] = tick;
    switch (m->type) {
      case RELAIS_MSG_ORDRE: {
        relais_reject why = RELAIS_REJ_NONE;
        if (order_is_valid(a, m, tick, &why)) {
          s->order = m;
          s->order_valid = true;
        } else {
          reject(a, out, tick, why, m);
        }
        break;
      }
      case RELAIS_MSG_OMBRE:
        if (m->tick == tick) s->shadow = m;
        break;
      case RELAIS_MSG_CESSION:
        /* DET-05: the machine gives up in the same tick; the actuator records
         * it now and frees the mandate at the next tick (REL-06). */
        if (m->sender == a->holder && m->mandate == a->mandate) {
          s->cession = m;
          a->cession_pending = true;
          push_event(out, RELAIS_EV_CESSION, m->sender, a->mandate, tick, m->reason);
        }
        break;
      case RELAIS_MSG_DEMANDE:
        if (s->demande == NULL) s->demande = m;
        break;
      default:
        break; /* STATUT, ETAT and SUIVI never reach the actuator */
    }
  }
}

/* The mandate goes vacant, or straight to the certified second (MAN-10). */
static void release_mandate(relais_arb *a, relais_arb_out *out, relais_tick tick,
                            relais_event_kind kind) {
  /* DET-03: the event carries its own evidence, the miss count the actuator
   * counted itself, so a verifier can judge the decision without guessing. */
  uint32_t evidence = (uint32_t)a->misses;
  push_event(out, kind, a->holder, a->mandate, tick, evidence);
  if (a->certified && a->certified_node != RELAIS_NODE_NONE &&
      a->certified_node != a->holder) {
    a->holder = a->certified_node;
    a->mandate++; /* MAN-03: exactly one step per change of holder */
    a->misses = 0;
    a->handovers++;
    /* REL-05: the shadow this very node sent for this tick is still a fresh
     * value, so the certification is dropped only once the value is picked. */
    a->switch_pending = true;
    /* The evidence travels with the event: the verifier judges the decision on
     * what the actuator counted, not on what the tick record shows afterwards. */
    push_event(out, RELAIS_EV_MANDATE_AUTO_SWITCH, a->holder, a->mandate, tick, evidence);
    return;
  }
  a->holder = RELAIS_NODE_NONE;
  a->certified = false;
  a->certified_node = RELAIS_NODE_NONE;
  a->certify_streak = 0;
}

/* SEC-06: a flood of demandes must not delay the orders. */
static bool request_allowed(relais_arb *a, relais_tick tick) {
  uint16_t window = (uint16_t)(tick / (a->params.request_window ? a->params.request_window : 1));
  if (window != a->request_window_tick) {
    a->request_window_tick = window;
    a->request_seen = 0;
  }
  if (a->request_seen >= a->params.request_burst) return false;
  a->request_seen++;
  return true;
}

static void handle_demande(relais_arb *a, relais_arb_out *out, relais_tick tick, scan_t *s) {
  const relais_msg *m = s->demande;
  if (m == NULL) return;
  bool vacant = a->holder == RELAIS_NODE_NONE;
  /* Mutant M3 grants the mandate with no evidence of silence (I7 falls). */
  bool grant = RELAIS_MUT(RELAIS_M3_GRANT_WITHOUT_SILENCE) ? true : vacant;
  if (!grant || !request_allowed(a, tick)) {
    push_event(out, RELAIS_EV_REQUEST_DENIED, m->sender, a->mandate, tick,
               grant ? 1u : 0u);
    return;
  }
  a->holder = m->sender;
  a->mandate++;
  a->misses = 0;
  a->handovers++;
  a->certified = false;
  a->certified_node = RELAIS_NODE_NONE;
  a->certify_streak = 0;
  push_event(out, RELAIS_EV_MANDATE_GRANTED, a->holder, a->mandate, tick, m->reason);
  /* MAN-05: the demande may carry the first order, for this very tick. */
  if (m->has_order && m->tick == tick) {
    s->order = m;
    s->order_valid = true;
  }
}

/* Spec 4.4: exactly one value per tick, in this order. */
static void pick_value(relais_arb *a, relais_arb_out *out, relais_tick tick, const scan_t *s,
                       const relais_meas *meas) {
  if (s->order_valid && s->order != NULL) {
    out->value = relais_clamp_value(s->order->value, &a->params);
    out->source = RELAIS_SRC_HOLDER;
    a->applied_by_source[RELAIS_SRC_HOLDER]++;
    /* ACT-02: the will of the last order received, covering the next N ticks. */
    a->testament_len = s->order->testament_len > RELAIS_TESTAMENT_MAX
                           ? RELAIS_TESTAMENT_MAX
                           : s->order->testament_len;
    for (uint8_t i = 0; i < a->testament_len; i++) a->testament[i] = s->order->testament[i];
    /* ACT-02: this order covers `tick`; the will it carries starts at tick+1,
     * because the holder filled it from the state it had just computed with. */
    a->testament_base = tick + 1;
    if (a->in_fallback) {
      a->in_fallback = false;
      push_event(out, RELAIS_EV_FALLBACK_EXIT, a->holder, a->mandate, tick, 0);
    }
    return;
  }
  if (a->certified && s->shadow != NULL && s->shadow->sender == a->certified_node) {
    out->value = relais_clamp_value(s->shadow->value, &a->params);
    out->source = RELAIS_SRC_SHADOW; /* REL-05: substitution, no stale tick */
    a->applied_by_source[RELAIS_SRC_SHADOW]++;
    return;
  }
  float planned = 0.0f;
  bool within = RELAIS_MUT(RELAIS_M4_TESTAMENT_FOREVER)
                    ? (a->testament_len > 0 && tick >= a->testament_base)
                    : relais_testament_at(a->testament, a->testament_len, a->testament_base,
                                          tick, &planned);
  if (within) {
    if (RELAIS_MUT(RELAIS_M4_TESTAMENT_FOREVER)) {
      size_t idx = (size_t)((tick - a->testament_base) % (a->testament_len ? a->testament_len : 1));
      planned = a->testament[idx];
    }
    out->value = relais_clamp_value(planned, &a->params);
    out->source = RELAIS_SRC_TESTAMENT;
    a->applied_by_source[RELAIS_SRC_TESTAMENT]++;
    return;
  }
  /* ACT-03: the will is exhausted, the local damped loop takes over. */
  out->value = relais_clamp_value(relais_fallback_step(&a->fb_state, meas, &a->params),
                                  &a->params);
  out->source = RELAIS_SRC_FALLBACK;
  a->applied_by_source[RELAIS_SRC_FALLBACK]++;
  if (!a->in_fallback) {
    a->in_fallback = true;
    push_event(out, RELAIS_EV_FALLBACK_ENTER, RELAIS_NODE_NONE, a->mandate, tick, 0);
  }
}

/* REP-05 and DET-09: certification by agreement, disagreement alert. */
static void certify(relais_arb *a, relais_arb_out *out, relais_tick tick, const scan_t *s) {
  if (a->holder == RELAIS_NODE_NONE) {
    a->certify_streak = 0;
    return;
  }
  relais_node second = a->holder == RELAIS_NODE_A ? RELAIS_NODE_B : RELAIS_NODE_A;
  if (s->shadow == NULL || s->shadow->sender != second) {
    a->certify_streak = 0;
    if (a->certified && ++a->shadow_misses > a->params.patience) {
      a->certified = false;
      a->certified_node = RELAIS_NODE_NONE;
      a->shadow_misses = 0;
      push_event(out, RELAIS_EV_DECERTIFIED, second, a->mandate, tick, 0);
    }
    return;
  }
  a->shadow_misses = 0;
  if (s->order_valid && s->order != NULL) {
    float gap = fabsf(s->order->value - s->shadow->value);
    a->last_order = s->order->value;
    a->last_shadow = s->shadow->value;
    if (gap <= a->params.epsilon) {
      a->disagree_streak = 0;
      if (a->certify_streak < 255) a->certify_streak++;
    } else {
      a->certify_streak = 0;
      if (a->certified) {
        a->certified = false;
        a->certified_node = RELAIS_NODE_NONE;
        push_event(out, RELAIS_EV_DECERTIFIED, second, a->mandate, tick, 0);
      }
      if (++a->disagree_streak >= 3 && !a->disagreement) {
        a->disagreement = true; /* DET-09: three ticks above epsilon */
        push_event(out, RELAIS_EV_DISAGREEMENT, second, a->mandate, tick,
                   (uint32_t)(gap * 1000.0f));
      }
      return;
    }
  }
  if (!a->certified && a->certify_streak >= a->params.certify_ticks) {
    a->certified = true;
    a->certified_node = second;
    push_event(out, RELAIS_EV_CERTIFIED, second, a->mandate, tick, a->certify_streak);
  }
}

static void liveness(relais_arb *a, relais_arb_out *out, relais_tick tick) {
  for (int i = RELAIS_NODE_A; i <= RELAIS_NODE_C; i++) {
    relais_node node = (relais_node)i;
    bool seen = a->last_heard[i] != 0 &&
                (tick - a->last_heard[i]) <= (relais_tick)LIVENESS_TICKS;
    if (seen && !a->present[i]) {
      a->present[i] = true;
      /* DEG-03 / I9: a machine coming BACK, not one showing up for the first
       * time while the system arms itself. */
      if (a->ever_seen[i]) push_event(out, RELAIS_EV_NODE_BACK, node, a->mandate, tick, 0);
    } else if (!seen && a->present[i]) {
      a->present[i] = false;
    }
    if (seen) a->ever_seen[i] = true;
  }
}

/* DEG-01, DEG-02 and I10: the lamp follows the cause and stays lit while it lasts. */
static void update_lamp(relais_arb *a, relais_arb_out *out, relais_tick tick) {
  relais_node second = a->holder == RELAIS_NODE_A ? RELAIS_NODE_B : RELAIS_NODE_A;
  bool peer_present = a->present[second];
  relais_lamp lamp = RELAIS_LAMP_OFF;
  relais_cause cause = RELAIS_CAUSE_NONE;

  if (out->source == RELAIS_SRC_FALLBACK) {
    lamp = RELAIS_LAMP_RED;
    cause = RELAIS_CAUSE_FALLBACK;
  } else if (a->disagreement) {
    lamp = RELAIS_LAMP_RED;
    cause = RELAIS_CAUSE_DISAGREEMENT;
  } else if (out->source != RELAIS_SRC_HOLDER) {
    /* DEG-01: the holder is not delivering, the redundancy is lost even while
     * the substitution keeps the command fresh. */
    lamp = RELAIS_LAMP_YELLOW;
    cause = RELAIS_CAUSE_NODE_MISSING;
  } else if (a->holder == RELAIS_NODE_NONE || !peer_present) {
    lamp = RELAIS_LAMP_YELLOW;
    cause = a->holder == RELAIS_NODE_NONE ? RELAIS_CAUSE_NODE_MISSING
                                          : RELAIS_CAUSE_LINK_LOST;
  } else if (!a->certified) {
    lamp = RELAIS_LAMP_YELLOW;
    cause = RELAIS_CAUSE_SECOND_UNCERTIFIED;
  }
  /* Mutant M8 puts the lamp out when a link comes back, second uncertified. */
  if (RELAIS_MUT(RELAIS_M8_LAMP_EARLY_OFF) && out->source == RELAIS_SRC_HOLDER) {
    lamp = RELAIS_LAMP_OFF;
    cause = RELAIS_CAUSE_NONE;
  }
  if (a->stop_requested) {
    lamp = RELAIS_LAMP_RED;
    cause = RELAIS_CAUSE_STOP_REQUESTED;
  }
  if (lamp != a->lamp) {
    push_event(out, RELAIS_EV_LAMP_CHANGE, a->holder, a->mandate, tick, (uint32_t)lamp);
    a->lamp = lamp;
    a->cause = cause;
  } else {
    a->cause = cause;
  }
  /* DEG-05: degraded mode is limited in time. */
  if (lamp == RELAIS_LAMP_OFF) {
    a->degraded_ticks = 0;
  } else if (a->degraded_ticks < a->params.degraded_ticks) {
    a->degraded_ticks++;
    if (a->degraded_ticks >= a->params.degraded_ticks && !a->stop_requested) {
      a->stop_requested = true;
      push_event(out, RELAIS_EV_STOP_REQUESTED, a->holder, a->mandate, tick,
                 a->degraded_ticks);
    }
  }
}

void relais_arb_init(relais_arb *a, const relais_params *p) {
  if (a == NULL || p == NULL) return;
  memset(a, 0, sizeof *a);
  a->params = *p;
  a->mandate = 0; /* MAN-06: the mandate starts vacant, the first grant makes it 1 */
  a->holder = RELAIS_NODE_NONE;
  a->certified_node = RELAIS_NODE_NONE;
  a->testament_base = RELAIS_TICK_NONE;
  a->lamp = RELAIS_LAMP_OFF;
  a->cause = RELAIS_CAUSE_NONE;
  relais_fallback_init(&a->fb_state, NULL);
}

void relais_arb_step(relais_arb *a, relais_tick tick, const relais_msg *msgs, size_t n,
                     const relais_meas *meas, relais_arb_out *out) {
  if (a == NULL || out == NULL) return;
  memset(out, 0, sizeof *out);
  relais_meas local;
  if (meas == NULL) {
    memset(&local, 0, sizeof local);
    meas = &local;
  }
  a->ticks++;
  /* REL-06: a cession recorded at the previous tick frees the mandate here, so
   * the order already received for that tick was applied under a known holder
   * and the new one can be granted in the same tick. */
  if (a->cession_pending) {
    a->cession_pending = false;
    release_mandate(a, out, tick, RELAIS_EV_MANDATE_REVOKED);
  }
  scan_t s;
  scan(a, out, tick, msgs, n, &s);
  (void)s.cession;

  /* 2. the miss is counted first: it is the actuator's own evidence (DET-01). */
  a->misses = s.order_valid ? 0 : (uint8_t)(a->misses < 255 ? a->misses + 1 : 255);

  /* 3. revoke on p+1 consecutive misses (MAN-04). */
  if (a->holder != RELAIS_NODE_NONE && a->misses > a->params.patience)
    release_mandate(a, out, tick, RELAIS_EV_MANDATE_REVOKED);

  /* 4. a DEMANDE can now be granted. */
  handle_demande(a, out, tick, &s);

  /* 5. the one value of this tick. */
  pick_value(a, out, tick, &s, meas);
  /* What the journal reports is the certification that served the choice; what
   * the STATUT announces is the state the machines must act on from now. */
  bool certified_when_picked = a->certified;
  relais_node certified_node_when_picked = a->certified_node;
  if (a->switch_pending) {
    /* The new holder has no certified second any more: the other machine must
     * be certified again before it can take over (DEG-07). */
    a->switch_pending = false;
    a->certified = false;
    a->certified_node = RELAIS_NODE_NONE;
    a->certify_streak = 0;
  }
  certify(a, out, tick, &s);
  liveness(a, out, tick);
  update_lamp(a, out, tick);

  /* 7. what both computers are told (MAN-07). */
  out->applied = true;
  out->mandate = a->mandate;
  out->holder = a->holder;
  out->misses = a->misses;
  out->certified = certified_when_picked;
  out->certified_node = certified_node_when_picked;
  out->lamp = a->lamp;
  out->cause = a->cause;
  out->stop_requested = a->stop_requested;
  out->statut.type = RELAIS_MSG_STATUT;
  out->statut.sender = RELAIS_NODE_ACTUATOR;
  out->statut.tick = tick;
  out->statut.mandate = a->mandate;
  out->statut.holder = a->holder;
  out->statut.source = out->source;
  out->statut.misses = a->misses;
  out->statut.certified = a->certified;
  out->statut.certified_node = a->certified_node;
  out->statut.lamp = a->lamp;
  out->statut.cause = a->cause;
  out->statut.stop_requested = a->stop_requested;
  out->statut.meas = *meas;
  out->statut.meas.applied = out->value;
}

size_t relais_arb_snapshot(const relais_arb *a, uint8_t *buf, size_t cap) {
  const size_t need = 16;
  if (a == NULL || buf == NULL || cap < need) return 0;
  memset(buf, 0, need);
  buf[0] = (uint8_t)(SNAPSHOT_MAGIC & 0xffu);
  buf[1] = (uint8_t)((SNAPSHOT_MAGIC >> 8) & 0xffu);
  buf[2] = (uint8_t)((SNAPSHOT_MAGIC >> 16) & 0xffu);
  buf[3] = (uint8_t)((SNAPSHOT_MAGIC >> 24) & 0xffu);
  buf[4] = SNAPSHOT_VERSION;
  buf[5] = (uint8_t)a->holder;
  buf[6] = (uint8_t)(a->certified ? 1 : 0);
  buf[7] = (uint8_t)a->certified_node;
  for (int i = 0; i < 4; i++)
    buf[8 + (size_t)i] = (uint8_t)((a->mandate >> (i * 8)) & 0xffu);
  for (int i = 0; i < 4; i++)
    buf[12 + (size_t)i] = (uint8_t)((a->handovers >> (i * 8)) & 0xffu);
  return need;
}

bool relais_arb_restore(relais_arb *a, const uint8_t *buf, size_t len) {
  if (a == NULL || buf == NULL || len < 16) return false;
  uint32_t magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
                   ((uint32_t)buf[3] << 24);
  if (magic != SNAPSHOT_MAGIC || buf[4] != SNAPSHOT_VERSION) return false;
  /* MAN-11: the mandate number survives a restart. Mutant M6 loses it. */
  relais_mandate kept = (relais_mandate)buf[8] | ((relais_mandate)buf[9] << 8) |
                        ((relais_mandate)buf[10] << 16) | ((relais_mandate)buf[11] << 24);
  a->mandate = RELAIS_MUT(RELAIS_M6_MANDATE_RESET) ? 0 : kept;
  a->holder = RELAIS_MUT(RELAIS_M6_MANDATE_RESET) ? RELAIS_NODE_NONE : (relais_node)buf[5];
  a->certified = buf[6] != 0 && !RELAIS_MUT(RELAIS_M6_MANDATE_RESET);
  a->certified_node = a->certified ? (relais_node)buf[7] : RELAIS_NODE_NONE;
  return true;
}

bool relais_arb_vacant(const relais_arb *a) {
  return a == NULL || a->holder == RELAIS_NODE_NONE;
}

void relais_arb_force_link(relais_arb *a, relais_node n, bool heard) {
  if (a == NULL || !relais_node_is_computer(n)) return;
  a->present[n] = heard;
}

uint32_t relais_arb_rejected_total(const relais_arb *a) {
  if (a == NULL) return 0;
  uint32_t total = 0;
  for (size_t i = 0; i < RELAIS_REJECT_SLOTS; i++) total += a->rejected[i];
  return total;
}
