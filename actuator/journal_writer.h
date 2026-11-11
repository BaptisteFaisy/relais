/* Relais — writing the actuator's journal to a stream (spec 6.4, ACT-07).
 *
 * The formatting lives in the pure core (core/src/journal.c) so that the
 * verifier and the report read exactly what the actuator wrote. What is left
 * here is the only part that touches a stream, shared by the PC actuator and by
 * the QNX one: one implementation, two binaries.
 *
 * Order matters: the events of a tick are written BEFORE its record, which is
 * the order the verifier and the invariant checker expect.
 */
#ifndef RELAIS_JOURNAL_WRITER_H
#define RELAIS_JOURNAL_WRITER_H

#include <stdio.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "relais/journal.h"

/* Fill a tick record from what the arbiter decided and what the rack did. */
void relais_jw_fill_tick(relais_jr_tick *r, const relais_arb_out *out, relais_tick tick,
                         const relais_plant *plant, float setpoint);
void relais_jw_write_header(FILE *f, const relais_params *params, const char *scenario,
                            uint32_t scenario_id);
void relais_jw_write_tick(FILE *f, const relais_arb_out *out, relais_tick tick,
                          const relais_plant *plant, float setpoint);
void relais_jw_write_events(FILE *f, const relais_arb_out *out, relais_tick tick);

#endif /* RELAIS_JOURNAL_WRITER_H */
