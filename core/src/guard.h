/* Relais — internal guard on the arbiter input (M20).
 *
 * Shared by arbiter.c and guard.c only; not part of the public API. The public
 * entry points live in relais/arbiter.h.
 */
#ifndef RELAIS_GUARD_H
#define RELAIS_GUARD_H

#include "relais/arbiter.h"

/* The bit that stands for a node in the guard masks. */
uint8_t relais__guard_bit(relais_node n);

/* Why every frame of `n` must be dropped before any protocol rule is applied:
 * RELAIS_REJ_REVOKED, RELAIS_REJ_AUTH_BANNED, or RELAIS_REJ_NONE. */
relais_reject relais__guard_block(const relais_arb *a, relais_node n);

/* Journal, as events of this tick, the revocations and bans decided since the
 * previous one. arbiter.c calls it at the top of relais_arb_step; it pushes
 * through relais__arb_push_event so the event layout stays in one place. */
void relais__guard_flush(relais_arb *a, relais_arb_out *out, relais_tick tick);

/* Implemented by arbiter.c: the single event push, shared with the guard. */
void relais__arb_push_event(relais_arb_out *out, relais_event_kind kind, relais_node node,
                            relais_mandate mandate, relais_tick tick, uint32_t detail);

#endif /* RELAIS_GUARD_H */
