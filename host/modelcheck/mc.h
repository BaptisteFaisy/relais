/* Relais — bounded exhaustive checking of the C core itself (mission M36).
 *
 * WHY THIS EXISTS. host/explore/ and docs/tla/ prove properties of a MODEL of
 * the protocol. This harness proves properties of the C: it drives the real
 * core functions (relais_frame_decode, relais_arb_step, relais_ctl_step) over a
 * FINITE, PUBLISHED input space and checks the safety properties on EVERY
 * point of it. No sampling, no randomness: every case below is executed.
 *
 * WHAT THIS IS NOT. A general proof. Everything outside the published bounds is
 * declared NOT COVERED (docs/MODELCHECK.md), never assumed covered. The
 * unbounded obligations are discharged by the CBMC harness
 * (host/modelcheck/cbmc/) and the Frama-C/WP annotations
 * (host/modelcheck/wp/); `make modelcheck` runs them when the tools are present
 * and says exactly what is missing when they are not.
 *
 * Requirements cited: MAN-02, MAN-03, MAN-04, MAN-05, DET-01, SEC-01, SEC-02,
 * SEC-03, SEC-04, ACT-01, ACT-02, ACT-05, CMD-03, CMD-06, ROB-09, I1, I3, I6.
 */
#ifndef RELAIS_MC_H
#define RELAIS_MC_H

#include <stdbool.h>
#include <stdint.h>

#include "test_util.h" /* shared fixtures and T_CHECK/T_MSG: no second harness */

/* ---------------------------------------------------------------- bounds --
 * Every number here is a PUBLISHED bound: the exhaustion is complete inside it
 * and says nothing outside it (docs/MODELCHECK.md, section "Bounds"). */

/* Space A — the wire, relais_frame_decode (ROB-09, SEC-01, SEC-02). */
#define MC_A_TYPES        7u    /* RELAIS_MSG_STATUT .. RELAIS_MSG_SUIVI */
#define MC_A_PAYLEN_MAX   RELAIS_PAYLOAD_MAX /* 480: every declared length */
#define MC_A_BYTE_VALUES  256u  /* every value of a corrupted byte */
#define MC_A_EXTEND_MAX   8u    /* trailing bytes appended to a valid frame */
#define MC_A_KEY_MODES    2u    /* level S (CRC-32C) and level C (SipHash) */
#define MC_A_ETAT_STATE   38u   /* ctl_state_len of a real exported state */
#define MC_A_FIELD_VALUES 256u  /* every value of type/sender/version/flags */
#define MC_A_TICKS        6u    /* tick extremes, incl. 0 and UINT64_MAX */
#define MC_A_MANDATES     4u    /* mandate extremes, incl. 0 and UINT32_MAX */
/* NOT COVERED by space A: corruptions of two bytes or more at once, frames
 * longer than RELAIS_FRAME_MAX, and payloads above MC_A_PAYLEN_MAX. */

/* Space B — the arbiter, relais_arb_step (MAN-02..05, DET-01, ACT-01, I1, I3). */
#define MC_B_STATES       13u   /* arbiter states, each built by a fixed prefix */
#define MC_B_TICK_OFFSETS 7u    /* step tick = state tick + offset, -1 .. +64 */
#define MC_B_TYPES        7u    /* frame type of the single frame under test */
#define MC_B_SENDERS      4u    /* NONE, A, B, ACTUATOR */
#define MC_B_FRAME_TICKS  3u    /* frame tick label: tick-1, tick, tick+1 */
#define MC_B_MANDATES     5u    /* M-1, M, M+1, 0, UINT32_MAX (reused/future) */
#define MC_B_VALUES       9u    /* order value, incl. out-of-range and NaN */
#define MC_B_PAIR_VOCAB   16u   /* vocabulary of the two-frame cross product */
#define MC_B_PAIR_TICKS   5u    /* tick offsets used by that cross product */
#define MC_B_SEQ_VOCAB    12u   /* vocabulary of the depth-3 sequences */
#define MC_B_SEQ_DEPTH    3u    /* ticks chained per sequence */
/* NOT COVERED by space B: more than two frames per tick beyond the sequence
 * vocabulary, sequences longer than MC_B_SEQ_DEPTH, and non-default parameter
 * sets (patience, certify_ticks, epsilon) other than the three published. */

/* Space C — the control law, relais_ctl_step and friends (CMD-03, CMD-06). */
#define MC_C_CLAMP_VALUES 14u   /* incl. NaN and the two infinities */
#define MC_C_AMPLITUDES   3u    /* max_amplitude settings */
#define MC_C_SLEWS        3u    /* max_slew settings */
#define MC_C_MEAS_GRID    9u    /* grid points per measurement field */
#define MC_C_PARAMS       3u    /* published parameter sets */
#define MC_C_RUN_TICKS    2000u /* determinism run length (CMD-03) */
#define MC_C_TST_LEN      9u    /* testament length 0..8 */
#define MC_C_TST_BASES    5u    /* testament bases, incl. RELAIS_TICK_NONE */
#define MC_C_TST_TICKS    15u   /* ticks probed against each base */
/* NOT COVERED by space C: measurement values outside MC_C_CLAMP_VALUES, and
 * parameter sets outside the three published ones. */

/* ------------------------------------------------------------- reporting -- */

typedef struct {
  const char *name;  /* space name, as printed */
  uint64_t cases;    /* points of the input space executed */
  uint64_t rejected; /* decoder rejections observed (space A) */
  uint64_t accepted; /* decoder acceptances observed (space A) */
  double seconds;    /* wall time of the space */
} mc_space;

extern mc_space mc_sp_a; /* wire    */
extern mc_space mc_sp_b; /* arbiter */
extern mc_space mc_sp_c; /* control */

void mc_space_wire(mc_space *sp);    /* space A */
void mc_space_arbiter(mc_space *sp); /* space B */
void mc_space_control(mc_space *sp); /* space C */
void mc_report(const mc_space *sp);  /* one line: cases, time, bounds */
uint64_t mc_total_cases(void);
double mc_total_seconds(void);

/* The arbiter states of space B: built once, reused by every enumeration. */
typedef struct {
  relais_arb arb;
  relais_tick next; /* the tick the next step must use */
} mc_state;

void mc_arb_build_state(unsigned which, mc_state *st);
void mc_arb_pairs(mc_space *sp, const mc_state *states);
void mc_arb_sequences(mc_space *sp, const mc_state *states);
void mc_arb_corrupted(mc_space *sp, const mc_state *states);
/* Run one real step on a copy of `state` and check every property. The evolved
 * arbiter is written to `evolved` (may be NULL). */
void mc_run_case(mc_space *sp, const relais_arb *state, const relais_msg *msgs, size_t n,
                 relais_tick tick, bool in_contract, relais_arb *evolved,
                 relais_arb_out *outcome);

/* Observations, not failures: they locate the boundary of the documented input
 * contract of the arbiter (docs/MODELCHECK.md, "Contract boundary"). */
extern uint64_t mc_obs_holder_not_computer;
extern uint64_t mc_obs_demande_from_actuator;
extern uint64_t mc_obs_contract_violation;
extern uint64_t mc_obs_slew_nonfinite;

/* --------------------------------------------------------------- helpers -- */

double mc_now(void); /* monotonic seconds */
/* Level S: CRC-32C trailer. Level C: keyed SipHash trailer (SEC-02). */
void mc_key(relais_key *k, bool level_c);
/* A deterministic, valid message of every type: same fields on every run, so
 * the enumeration is reproducible and the counts are stable. */
void mc_fill_msg(relais_msg *m, relais_msg_type t);
/* Encode mc_fill_msg(t) with key k; returns the on-wire size (0 on failure). */
size_t mc_encode(relais_msg_type t, const relais_key *k, uint8_t *buf, size_t cap);
/* The three published parameter sets of space B and C. */
void mc_params_set(relais_params *p, unsigned which);
/* Describe a case for a failure message; every T_MSG uses it. */
void mc_desc(char *buf, size_t cap, const char *what, uint64_t a, uint64_t b);
/* The case being checked, so a failure names its exact input. */
extern char mc_case[224];
void mc_case_set(const char *fmt, ...);
/* buf = "<property> | <case>" for T_MSG. */
void mc_fail(char *buf, size_t cap, const char *prop);

#endif /* RELAIS_MC_H */
