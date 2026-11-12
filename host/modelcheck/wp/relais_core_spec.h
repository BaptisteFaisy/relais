/* Relais — the ACSL contracts of the public core API (mission M36).
 *
 * This file is a "specification module": it declares the functions of the
 * core with their contracts, and the harness (host/modelcheck/wp/wp_*.c) and
 * the sources of core are analysed together with it. The core is NOT
 * modified: the contracts live here, outside the core.
 *
 * The contracts state the properties the bounded exhaustion of
 * host/modelcheck/ verified on every case; WP tries to prove them for ALL
 * the inputs, which is the difference between the two halves of the proof.
 *
 * Requirements cited: MAN-02, MAN-03, SEC-01, SEC-03, SEC-04, ACT-01,
 * ACT-05, CMD-06, ROB-09, I1, I2, I3.
 */
#ifndef RELAIS_MC_CORE_SPEC_H
#define RELAIS_MC_CORE_SPEC_H

#include "relais/arbiter.h"
#include "relais/check.h"
#include "relais/control.h"
#include "relais/frame.h"

/* SEC-01, ROB-09: the parser reads only the bytes it is given, and a frame
 * is decoded only if its header is legal. */
bool relais_frame_decode(const uint8_t *buf, size_t len, const relais_key *k, relais_msg *out,
                         relais_reject *why)
    /*@ requires \valid_read((const uint8_t *)buf + 0 .. len);
        requires \valid_read(k);
        requires \valid(out);
        assigns *out;
        ensures \result ==> out->type >= RELAIS_MSG_STATUT && out->type <= RELAIS_MSG_SUIVI;
        ensures \result ==> out->sender == RELAIS_NODE_A || out->sender == RELAIS_NODE_B ||
                            out->sender == RELAIS_NODE_C || out->sender == RELAIS_NODE_ACTUATOR;
    */;

/* SEC-02: the integrity check is the one of the key (CRC-32C at level S,
 * keyed SipHash at level C). */
bool relais_check_verify(const relais_key *k, const void *frame, size_t len, uint64_t got)
    /*@ requires \valid_read(k);
        requires \valid_read((const uint8_t *)frame + 0 .. len);
        assigns \nothing;
    */;

/* I1 (ACT-01), I2 (MAN-03), ACT-05: one tick applies exactly one bounded
 * value, from one of the four published sources, and the mandate moves by at
 * most one step and never backwards. */
void relais_arb_step(relais_arb *a, relais_tick tick, const relais_msg *msgs, size_t n,
                     const relais_meas *meas, relais_arb_out *out)
    /*@ requires \valid(a);
        requires \valid_read(msgs + 0 .. n);
        requires \valid_read(meas);
        requires \valid(out);
        assigns *a, *out;
        ensures a->mandate >= \old(a->mandate);
        ensures a->mandate - \old(a->mandate) <= 1;
        ensures out->applied;
        ensures out->source >= RELAIS_SRC_HOLDER && out->source <= RELAIS_SRC_FALLBACK;
        ensures out->holder == a->holder;
        ensures out->mandate == a->mandate;
        ensures out->value <= a->params.max_amplitude;
        ensures out->value >= -a->params.max_amplitude;
    */;

/* CMD-06, ACT-05: the law returns a bounded command, and the change per tick
 * stays inside the rate bound. */
float relais_ctl_step(relais_ctl_state *s, const relais_meas *m, const relais_params *p)
    /*@ requires \valid(s);
        requires \valid_read(m);
        requires \valid_read(p);
        assigns *s;
        ensures \result <= p->max_amplitude;
        ensures \result >= -p->max_amplitude;
        ensures \result - \old(s->last_value) <= p->max_slew;
        ensures \old(s->last_value) - \result <= p->max_slew;
    */;

/* ACT-05: the clamp is total and bounded. */
float relais_clamp_value(float v, const relais_params *p)
    /*@ requires \valid_read(p);
        assigns \nothing;
        ensures \result <= p->max_amplitude;
        ensures \result >= -p->max_amplitude;
    */;

/* ACT-02, I6: the will is played only inside its horizon. */
bool relais_testament_at(const float *testament, uint8_t len, relais_tick base, relais_tick tick,
                         float *out)
    /*@ requires \valid_read(testament + 0 .. len);
        requires \valid(out);
        assigns *out;
        ensures \result ==> base != RELAIS_TICK_NONE && tick >= base &&
                            tick - base < (relais_tick)len;
        ensures \result ==> *out == testament[tick - base];
    */;

#endif /* RELAIS_MC_CORE_SPEC_H */
