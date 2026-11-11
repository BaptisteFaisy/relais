/* Relais — the safety invariants of spec 7.2, checked once and used three
 * times: by the exhaustive explorer, by the seeded simulator and by the
 * offline verifier. Feeding them is incremental, so a violation is reported at
 * the exact tick where it appears, with the counter-example still in hand.
 *
 * What the journal cannot show is stated where it is checked: the tick label
 * of an applied order is guaranteed by the arbiter (MAN-02) and explored
 * directly by host/explore; here I3 is checked as "no order of a machine that
 * was not the published holder".
 */
#include "relais/invariants.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void relais_inv_init(relais_inv *v, const relais_params *p) {
  if (v == NULL) return;
  memset(v, 0, sizeof *v);
  if (p != NULL) v->params = *p;
  else relais_params_default(&v->params);
  v->gap_bound = (uint32_t)v->params.patience + 1u;
  v->deviation_bound = v->params.delta;
  v->last_holder = RELAIS_NODE_NONE;
  v->twin_enabled = false;
  v->single_fault = true;
}

void relais_inv_expect_certified(relais_inv *v, bool yes) {
  if (v != NULL) {
    v->expect_certified_second = yes;
    v->gap_bound = yes ? 0u : (uint32_t)v->params.patience + 1u;
  }
}

void relais_inv_fault_free(relais_inv *v, bool yes) {
  if (v != NULL) v->fault_free_run = yes;
}

void relais_inv_lossy_channel(relais_inv *v, bool yes) {
  if (v == NULL) return;
  v->lossy_channel = yes;
  if (yes) v->gap_bound = (uint32_t)v->params.patience + 1u;
}

void relais_inv_double_fault(relais_inv *v, relais_tick tick) {
  if (v == NULL || v->double_fault_at != 0) return;
  v->double_fault_at = tick;
  v->single_fault = false;
  /* Beyond the single-fault hypothesis the claim is the safe state, not the
   * continuity of the command (spec 3.3, F9, F10): the gap is no longer
   * bounded, only I8 (the fallback loop reached within N+1 ticks) still is. */
  v->gap_bound = UINT32_MAX;
}

void relais_inv_value_fault(relais_inv *v, bool yes, relais_tick at) {
  if (v == NULL) return;
  v->value_fault = yes;
  v->value_fault_at = at;
}

void relais_inv_violate(relais_inv *v, const char *id, relais_tick tick, const char *detail) {
  if (v == NULL) return;
  v->violations++;
  if (v->violations == 1)
    snprintf(v->first, sizeof v->first, "%s violated at tick %llu: %s", id,
             (unsigned long long)tick, detail != NULL ? detail : "");
}

/* I1: exactly one value per tick, from one of the four known sources. */
static void check_unicity(relais_inv *v, const relais_jr_tick *r) {
  if (r->source == RELAIS_SRC_NONE) {
    relais_inv_violate(v, "I1", r->tick, "no value applied at this tick");
    return;
  }
  if (r->source == RELAIS_SRC_HOLDER && r->holder == RELAIS_NODE_NONE)
    relais_inv_violate(v, "I1", r->tick, "order applied with no holder");
  if (r->source == RELAIS_SRC_SHADOW && !r->certified)
    relais_inv_violate(v, "I1", r->tick, "substitution by a second that is not certified");
}

/* I2: the mandate never decreases and never changes twice in one tick; two
 * machines never hold the same mandate number. */
static void check_mandate(relais_inv *v, const relais_jr_tick *r) {
  if (v->ticks > 1) {
    if (r->mandate < v->last_mandate)
      relais_inv_violate(v, "I2", r->tick, "mandate went backwards");
    if (r->holder != RELAIS_NODE_NONE && v->last_holder != RELAIS_NODE_NONE &&
        r->holder != v->last_holder && r->mandate == v->last_mandate)
      relais_inv_violate(v, "I2", r->tick, "two holders of the same mandate");
  }
  if (v->mandate_changes_this_tick > 1)
    relais_inv_violate(v, "I2", r->tick, "mandate changed twice in one tick");
}

/* I3: no order of a deposed machine is applied. */
static void check_no_returning(relais_inv *v, const relais_jr_tick *r) {
  if (v->ticks < 2 || r->source != RELAIS_SRC_HOLDER) return;
  if (r->holder == v->last_holder) return;
  if (v->mandate_changes_this_tick == 0)
    relais_inv_violate(v, "I3", r->tick,
                       "order applied from a machine that is not the published holder");
}

/* I4: the gap between two fresh orders stays inside the bound. */
static void check_gap(relais_inv *v, const relais_jr_tick *r, bool fresh) {
  if (fresh) {
    v->gap = 0;
    v->last_fresh_tick = r->tick;
    return;
  }
  if (v->last_fresh_tick == 0) return; /* start-up, before any holder */
  v->gap++;
  if (v->gap > v->max_gap) v->max_gap = v->gap;
  /* The bounds of I4 are claimed for a channel inside the timing hypothesis
   * 2*delta + C + J <= T, with no burst of losses longer than p (ROB-03, spec
   * 7.3 item 5). On a disturbed channel the gap is still measured and
   * published; only the safety invariants stay enforced. */
  if (v->lossy_channel) return;
  if (v->expect_certified_second && v->single_fault)
    relais_inv_violate(v, "I4", r->tick, "a tick without a fresh order, second certified");
  else if (v->gap > v->gap_bound)
    relais_inv_violate(v, "I4", r->tick, "gap between fresh orders above p+1");
}

/* I5: continuity against the twin without fault, while a machine commands. */
static void check_continuity(relais_inv *v, const relais_jr_tick *r, const float *twin) {
  /* The twin is a machine already in service: before the first fresh order the
   * two runs are still arming the mandate, and a lost frame there is not a
   * continuity defect. */
  if (twin == NULL || v->last_fresh_tick == 0 || r->source == RELAIS_SRC_FALLBACK) return;
  v->twin_enabled = true;
  /* Start-up grace: while the mandate is still being armed, a lost frame sends
   * the twin and the run down different paths, which is not a continuity
   * defect. Once in service, every tick is compared. */
  if (v->service_ticks < 10) return;
  float deviation = fabsf(r->value - *twin);
  if (deviation > v->max_value_deviation) v->max_value_deviation = deviation;
  /* REP-06 and I5 are level C requirements: at level S the handover is served
   * by the will, whose extrapolation stays inside a few percent. The deviation
   * is measured either way, it is only enforced when a second is certified. */
  /* I5 is claimed inside the timing hypothesis, like I4. */
  if (v->expect_certified_second && v->single_fault && !v->value_fault &&
      !v->lossy_channel && deviation > v->deviation_bound)
    relais_inv_violate(v, "I5", r->tick, "applied value too far from the twin");
}

/* I6 and I8: the will is never played past its horizon, and the fallback loop
 * takes over at most N+1 ticks after the last fresh order. */
static void check_horizon(relais_inv *v, const relais_jr_tick *r) {
  if (r->source == RELAIS_SRC_TESTAMENT) {
    v->testament_run++;
    if (v->testament_run > (uint32_t)v->params.testament_len)
      relais_inv_violate(v, "I6", r->tick, "will played past its horizon");
  } else {
    v->testament_run = 0;
  }
  if (r->source == RELAIS_SRC_FALLBACK) {
    if (!v->in_fallback) {
      /* I8 is about reaching the safe state, not about staying in it: with no
       * machine left, the fallback loop IS the safe state (F9, F10). */
      v->in_fallback = true;
      v->fallback_since = r->tick;
      v->fallback_ticks = 0;
      if (v->last_fresh_tick != 0 &&
          (r->tick - v->last_fresh_tick) > (relais_tick)v->params.testament_len + 1)
        relais_inv_violate(v, "I8", r->tick, "fallback reached too late after the last order");
    } else {
      v->fallback_ticks++;
    }
    if (v->fallback_ticks > v->max_fallback_run) v->max_fallback_run = v->fallback_ticks;
  } else {
    v->in_fallback = false;
    v->fallback_ticks = 0;
  }
}

/* I10: every loss of redundancy shows on the lamp within two ticks and stays
 * lit while the cause lasts. */
static void check_signalling(relais_inv *v, const relais_jr_tick *r) {
  bool degraded = r->source != RELAIS_SRC_HOLDER || r->holder == RELAIS_NODE_NONE ||
                  (v->expect_certified_second && !r->certified);
  if (!degraded) {
    v->lamp_delay = 0;
    v->last_lamp = r->lamp;
    return;
  }
  if (r->lamp != RELAIS_LAMP_OFF) {
    v->lamp_delay = 0;
    v->last_lamp = r->lamp;
    return;
  }
  v->lamp_delay++;
  if (v->lamp_delay > v->max_lamp_delay) v->max_lamp_delay = v->lamp_delay;
  if (v->lamp_delay > 2)
    relais_inv_violate(v, "I10", r->tick, "redundancy lost and the lamp stayed off");
}

void relais_inv_on_tick(relais_inv *v, const relais_jr_tick *r, const float *twin_value) {
  if (v == NULL || r == NULL) return;
  v->ticks++;
  /* I11: progress, one tick after the other, nothing skipped. */
  if (r->tick != (relais_tick)v->ticks)
    relais_inv_violate(v, "I11", r->tick, "tick numbering is not contiguous");
  check_unicity(v, r);
  check_mandate(v, r);
  check_no_returning(v, r);

  bool fresh = r->source == RELAIS_SRC_HOLDER || r->source == RELAIS_SRC_SHADOW;
  check_gap(v, r, fresh);
  check_continuity(v, r, twin_value);
  check_horizon(v, r);
  /* I9: while a machine comes back, the holder still serves every tick. */
  if (v->reintegration_ticks > 0) {
    v->reintegration_ticks--;
    /* I9: a machine joining must not disturb the command. At level S the will
     * may still serve a tick during a handover (that is I4's bound); the
     * fallback loop may not, at any level. */
    bool bad = !v->lossy_channel &&
               (r->source == RELAIS_SRC_FALLBACK || (v->expect_certified_second && !fresh));
    if (bad)
      relais_inv_violate(v, "I9", r->tick, "a tick not served by the holder while reintegrating");
  }
  if (r->misses > v->max_misses) v->max_misses = r->misses;
  if (fresh && v->service_ticks < 1000000) v->service_ticks++;
  check_signalling(v, r);
  /* DET-09: a machine that computes wrong must be unmasked, not just tolerated. */
  if (v->value_fault && v->expect_certified_second && r->tick >= v->value_fault_at &&
      !v->disagreement_seen &&
      !v->detection_reported && r->tick > v->value_fault_at + 3) {
    v->detection_reported = true;
    relais_inv_violate(v, "DET-09", r->tick, "a wrong value was never reported as a disagreement");
  }

  v->last_mandate = r->mandate;
  v->last_holder = r->holder;
  v->last_lamp = r->lamp;
}

void relais_inv_on_event(relais_inv *v, const relais_jr_event *e) {
  if (v == NULL || e == NULL) return;
  switch (e->kind) {
    case RELAIS_EV_CESSION:
      v->cession_since_change = true;
      break;
    case RELAIS_EV_MANDATE_REVOKED:
      /* MAN-04 / DET-03: the actuator revokes only on its own evidence, and the
       * event carries it: the miss count it counted itself, or a cession. */
      if (!v->cession_since_change && e->detail <= (uint32_t)v->params.patience)
        relais_inv_violate(v, "DET-03", e->tick, "mandate revoked with no evidence");
      if (v->fault_free_run)
        relais_inv_violate(v, "I7", e->tick, "mandate revoked with no fault injected");
      v->mandate_vacant = true;
      break;
    case RELAIS_EV_MANDATE_GRANTED:
      v->mandate_changes_this_tick++;
      /* I7: with no injected fault the holder must never change. */
      if (v->fault_free_run && v->last_holder != RELAIS_NODE_NONE)
        relais_inv_violate(v, "I7", e->tick, "handover with no fault injected");
      /* MAN-05: a mandate is granted only when it is vacant. */
      if (v->last_holder != RELAIS_NODE_NONE && !v->mandate_vacant)
        relais_inv_violate(v, "DET-03", e->tick,
                           "mandate granted while a holder was in place");
      v->mandate_vacant = false;
      v->cession_since_change = false;
      break;
    case RELAIS_EV_MANDATE_AUTO_SWITCH:
      v->mandate_changes_this_tick++;
      if (!v->cession_since_change && e->detail <= (uint32_t)v->params.patience)
        relais_inv_violate(v, "DET-03", e->tick, "auto switch with no evidence of silence");
      if (v->fault_free_run && v->last_holder != RELAIS_NODE_NONE)
        relais_inv_violate(v, "I7", e->tick, "handover with no fault injected");
      v->mandate_vacant = false;
      v->cession_since_change = false;
      break;
    case RELAIS_EV_NODE_BACK:
      v->reintegration_ticks = (uint32_t)v->params.certify_ticks + 1u;
      break;
    case RELAIS_EV_DISAGREEMENT:
      v->disagreement_seen = true; /* DET-09 */
      break;
    default:
      break;
  }
}

void relais_inv_end_tick(relais_inv *v, relais_tick tick) {
  if (v == NULL) return;
  (void)tick;
  v->mandate_changes_this_tick = 0;
}

bool relais_inv_ok(const relais_inv *v) { return v != NULL && v->violations == 0; }

void relais_inv_report(const relais_inv *v, char *buf, size_t cap) {
  if (v == NULL || buf == NULL || cap == 0) return;
  int n = snprintf(buf, cap,
                   "ticks=%u violations=%u max_gap=%u (bound %u) "
                   "max_deviation=%.6g (bound %.6g) fallback_run=%u lamp_delay=%u level=%s",
                   (unsigned)v->ticks, (unsigned)v->violations, (unsigned)v->max_gap,
                   (unsigned)v->gap_bound, (double)v->max_value_deviation,
                   (double)v->deviation_bound, (unsigned)v->max_fallback_run,
                   (unsigned)v->max_lamp_delay,
                   v->expect_certified_second ? "complet" : "socle");
  if (v->violations > 0 && (size_t)n + 2 < cap)
    snprintf(buf + n, cap - (size_t)n, "\nfirst: %s", v->first);
}
