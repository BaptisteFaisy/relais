/* Relais — writing the actuator's journal to a stream (spec 6.4, ACT-07).
 *
 * The formatting lives in the pure core (core/src/journal.c) so that the
 * verifier and the report read exactly what the actuator wrote. What is left
 * here is the only part that touches a stream, shared by the PC actuator, the
 * QNX one and the simulator: one implementation, three binaries.
 *
 * Order matters: the events of a tick are written BEFORE its record, which is
 * the order the verifier and the invariant checker expect.
 *
 * The writer holds the chain state (idea 34): every record it writes carries the
 * fingerprint of the record before it, so a journal that was edited, truncated
 * or padded afterwards is detected by relais-verify --check-chain.
 */
#ifndef RELAIS_JOURNAL_WRITER_H
#define RELAIS_JOURNAL_WRITER_H

#include <stdio.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "relais/journal.h"

/* One journal being written: its stream, the fingerprint of its last record and
 * the chain key (an enabled key, or the built-in public one when not enabled). */
typedef struct {
  FILE *f;
  relais_chain chain;
  relais_key key;
  size_t records; /* records written: the anchor names the fingerprint and this */
} relais_jw;

/* Attach a writer to an open stream. chain_key may be NULL for the default. */
void relais_jw_open(relais_jw *w, FILE *f, const relais_key *chain_key);

/* Fill a tick record from what the arbiter decided and what the rack did. */
void relais_jw_fill_tick(relais_jr_tick *r, const relais_arb_out *out, relais_tick tick,
                         const relais_plant *plant, float setpoint);
void relais_jw_write_header(relais_jw *w, const relais_params *params, const char *scenario,
                            uint32_t scenario_id);
void relais_jw_write_tick(relais_jw *w, const relais_arb_out *out, relais_tick tick,
                          const relais_plant *plant, float setpoint);
void relais_jw_write_events(relais_jw *w, const relais_arb_out *out, relais_tick tick);

/* The fingerprint of the last record written, as text: the anchor to record
 * OUTSIDE the journal. A hash chain proves that nothing inside the file was
 * edited, inserted or deleted in the middle, but a file whose tail was cut is
 * still a valid chain; only this anchor, kept apart, reveals the truncation. */
int relais_jw_anchor(const relais_jw *w, char *out, size_t cap);

#endif /* RELAIS_JOURNAL_WRITER_H */
