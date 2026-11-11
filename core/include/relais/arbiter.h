/* Relais — the actuator's arbiter (spec 4.3, 4.4, 4.5, 5.2, 5.6).
 *
 * The actuator is the only one that writes the mandate number and the holder.
 * It revokes a mandate only on its own evidence: p+1 consecutive ticks without
 * a valid order from the holder, or a CESSION from it. It never trusts a claim.
 *
 * One call per tick, in this order:
 *   1. look for a valid order of the holder for this tick,
 *   2. count the miss,
 *   3. revoke when misses > p (auto-switch to a certified second, MAN-10),
 *   4. process CESSION then DEMANDE frames,
 *   5. pick the one value applied at this tick (spec 4.4),
 *   6. update certification, lamp, degraded timer,
 *   7. fill the STATUT to publish and the journal events.
 */
#ifndef RELAIS_ARBITER_H
#define RELAIS_ARBITER_H

#include "relais/frame.h"
#include "relais/mutant.h"
#include "relais/params.h"

typedef struct {
  relais_params params;

  /* Mandate, the only copy that counts. */
  relais_mandate mandate;
  relais_node holder;
  uint8_t misses;              /* consecutive ticks without a valid holder order */

  /* Certification of the second (REP-05). */
  bool switch_pending;   /* a certified second became holder during this tick */
  bool certified;
  relais_node certified_node;
  uint8_t certify_streak;
  uint8_t shadow_misses;
  float last_order;
  float last_shadow;
  bool have_order;
  bool have_shadow;
  uint8_t disagree_streak;
  bool disagreement;

  /* Testament of the last order received. */
  float testament[RELAIS_TESTAMENT_MAX];
  uint8_t testament_len;
  relais_tick testament_base; /* first tick the will covers */

  /* Fallback law state. */
  float fb_state;
  bool in_fallback;

  /* Liveness of the two computers, as the actuator sees it. */
  relais_tick last_heard[5];  /* indexed by relais_node */
  bool present[5];
  bool ever_seen[5];          /* a first sighting is not a reintegration */

  /* Signalling and degraded mode. */
  relais_lamp lamp;
  relais_cause cause;
  uint32_t degraded_ticks;
  bool stop_requested;

  /* REL-06: a cession takes effect at the next tick, so the order already
   * received for this tick is still applied under a known holder. */
  bool cession_pending;

  /* SEC-06 rate limit on demandes. */
  uint16_t request_window_tick;
  uint8_t request_seen;

  /* Counters, published by STATUT and the journal. */
  uint32_t rejected[RELAIS_REJECT_SLOTS];
  uint32_t forged;
  uint32_t applied_by_source[5];
  uint32_t ticks;
  uint32_t handovers;
} relais_arb;

typedef struct {
  bool applied;             /* a value was applied at this tick: always true */
  float value;              /* the bounded value applied */
  relais_source source;
  relais_mandate mandate;
  relais_node holder;
  uint8_t misses;
  bool certified;
  relais_node certified_node;
  relais_lamp lamp;
  relais_cause cause;
  bool stop_requested;
  relais_msg statut;        /* ready to send to both computers */
  relais_event events[RELAIS_EVENTS_MAX];
  uint8_t event_count;
} relais_arb_out;

void relais_arb_init(relais_arb *a, const relais_params *p);

/* One tick. msgs[0..n-1] are the frames received during the previous period,
 * already decoded and authenticated. meas carries the plant position and
 * velocity measured at the end of the previous tick and the setpoint for the
 * next one. */
void relais_arb_step(relais_arb *a, relais_tick tick, const relais_msg *msgs,
                     size_t n, const relais_meas *meas, relais_arb_out *out);

/* MAN-11: the mandate number survives a restart of the actuator. */
size_t relais_arb_snapshot(const relais_arb *a, uint8_t *buf, size_t cap);
bool relais_arb_restore(relais_arb *a, const uint8_t *buf, size_t len);

/* Maintenance and bench helpers. */
bool relais_arb_vacant(const relais_arb *a);
void relais_arb_force_link(relais_arb *a, relais_node n, bool heard);
uint32_t relais_arb_rejected_total(const relais_arb *a);

#endif /* RELAIS_ARBITER_H */
