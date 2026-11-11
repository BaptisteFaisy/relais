/* Relais — the invariants of spec 7.2, implemented once and used by the
 * exhaustive explorer, the seeded simulator and relais-verify.
 *
 * The checker is incremental: feed it every tick record and every event, in
 * order, and ask for the verdict at the end. It never allocates.
 */
#ifndef RELAIS_INVARIANTS_H
#define RELAIS_INVARIANTS_H

#include "relais/journal.h"
#include "relais/params.h"

#define RELAIS_INV_MSG_MAX 192

typedef struct {
  relais_params params;
  bool expect_certified_second; /* level C: a certified second must leave no gap */
  bool fault_free_run;          /* I7: no fault was injected in this run */
  bool single_fault;            /* I4 is only claimed under the single-fault rule */
  bool lossy_channel;           /* I4's zero-gap bound assumes a channel that delivers */
  bool value_fault;             /* F8: a machine emits a wrong value on purpose */
  relais_tick value_fault_at;   /* tick at which that fault was injected */
  bool disagreement_seen;       /* DET-09: the actuator must notice it */
  bool detection_reported;
  relais_tick double_fault_at;  /* tick where the single-fault hypothesis broke */

  /* DET-03 / MAN-04: what evidence the actuator had before it changed hands */
  uint8_t max_misses;
  bool cession_since_change;
  bool mandate_vacant;         /* MAN-05: a grant is only legal when vacant */
  uint32_t service_ticks;  /* ticks since the first fresh order (start-up grace) */

  /* I2, I3 */
  relais_mandate last_mandate;
  relais_node last_holder;
  uint32_t mandate_changes_this_tick;

  /* I4 */
  uint32_t gap;      /* consecutive ticks without a fresh order */
  uint32_t max_gap;
  uint32_t gap_bound;

  /* I5: the twin without fault is the reference */
  bool twin_enabled;
  float max_value_deviation;
  float max_pos_deviation;
  float deviation_bound;

  /* I6, I8 */
  uint32_t testament_run; /* consecutive ticks served by the will */
  relais_tick last_fresh_tick;
  relais_tick fallback_since;
  bool in_fallback;
  uint32_t fallback_ticks;
  uint32_t max_fallback_run;

  /* I9: ticks left in a reintegration window */
  uint32_t reintegration_ticks;

  /* I10 */
  relais_lamp last_lamp;
  uint32_t lamp_delay;
  uint32_t max_lamp_delay;

  /* Result */
  uint32_t violations;
  uint32_t ticks;
  char first[RELAIS_INV_MSG_MAX];  /* first violation, with its tick */
  char summary[RELAIS_INV_MSG_MAX];
} relais_inv;

void relais_inv_init(relais_inv *v, const relais_params *p);
void relais_inv_expect_certified(relais_inv *v, bool yes);
void relais_inv_fault_free(relais_inv *v, bool yes);
/* The channel loses or delays frames: the zero-gap bound of level C no longer
 * applies, the will covers the gap instead (ROB-02, ROB-03). */
void relais_inv_lossy_channel(relais_inv *v, bool yes);
/* A second fault landed before redundancy was back: from this tick on, I4 and
 * I5 are measured but not enforced; I8 (safe state) still is (spec 3.3). */
void relais_inv_double_fault(relais_inv *v, relais_tick tick);
/* A machine emits a wrong value (F8, INJ-10): continuity is not claimed any
 * more, detection is (DET-09). */
void relais_inv_value_fault(relais_inv *v, bool yes, relais_tick at);
/* Feed one twin value for the same tick, or NULL when there is no twin. */
void relais_inv_on_tick(relais_inv *v, const relais_jr_tick *r, const float *twin_value);
void relais_inv_on_event(relais_inv *v, const relais_jr_event *e);
/* Call once per tick, after its events, to close the tick window. */
void relais_inv_end_tick(relais_inv *v, relais_tick tick);
bool relais_inv_ok(const relais_inv *v);
void relais_inv_report(const relais_inv *v, char *buf, size_t cap);
void relais_inv_violate(relais_inv *v, const char *id, relais_tick tick, const char *detail);

#endif /* RELAIS_INVARIANTS_H */
