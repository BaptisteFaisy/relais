/* Relais — byzantine frame injector (mission M20).
 *
 * Builds the frames a hostile or faulty peer could put on the wire, encodes
 * them with the real protocol encoder (relais_frame_encode, SEC-02) and hands
 * them back decoded, so the bench plays exactly what the wire would carry and
 * not an in-memory shortcut. Every builder fills a relais_msg; the bench then
 * calls relais_byz_roundtrip to prove the frame is wire-valid before stepping
 * the arbiter with it.
 *
 * The injector is host code: it may allocate nothing either, it only fills
 * caller memory, like the core.
 */
#ifndef RELAIS_BYZ_INJECT_H
#define RELAIS_BYZ_INJECT_H

#include "relais/frame.h"
#include "relais/params.h"

/* The bench link key: the shared secret the actuator and both computers use at
 * level C (SEC-02). An attacker that holds it is an insider. */
void relais_byz_key(relais_key *k);

/* Encode `m` then decode it back into `out`: true when the frame is a valid one
 * for the protocol. `buf`/`cap` is the scratch the encoder writes into. This is
 * the single place the injector touches the wire format. */
bool relais_byz_roundtrip(const relais_msg *m, const relais_key *k, relais_msg *out,
                          uint8_t *buf, size_t cap);

/* --- the attacks. Each fills a message; the bench picks the hostile values. --- */

/* ORDRE of `from` for `tick` under `mandate`, carrying `value` and a flat will.
 * A future mandate (SEC-04) or a wrong tick (SEC-03) is just a hostile argument
 * here: the same builder plays the honest and the byzantine order. */
void relais_byz_order(relais_msg *m, relais_node from, relais_mandate mandate, relais_tick tick,
                      float value, const relais_params *p);

/* STATUT usurping the actuator. `from` is whoever emits it: a computer that
 * pretends to be the actuator, or the actuator identity forged by a computer.
 * The arbiter never consumes a STATUT; this probes that boundary. */
void relais_byz_fake_statut(relais_msg *m, relais_node from, relais_tick tick,
                            relais_mandate mandate, relais_node holder);

/* CESSION attributed to `claimed` (the holder being usurped), under `mandate`.
 * The sender field lies: the frame is emitted by another machine. */
void relais_byz_cession(relais_msg *m, relais_node claimed, relais_mandate mandate,
                        uint8_t reason);

/* ETAT authenticated with the (stolen) link key but carrying a corrupt state.
 * The arbiter never consumes an ETAT either; the second machine does. */
void relais_byz_etat(relais_msg *m, relais_node from, relais_tick tick, float last_value);

/* DEMANDE of `from`, optionally carrying its first order (MAN-05). A burst is
 * the same builder called many times within one window (SEC-06). */
void relais_byz_demande(relais_msg *m, relais_node from, relais_mandate seen, relais_tick tick,
                        bool with_order, float value, const relais_params *p);

#endif /* RELAIS_BYZ_INJECT_H */
