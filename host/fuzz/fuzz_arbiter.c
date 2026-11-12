/* Relais — fuzz target for the arbiter (MAN-02, MAN-04, MAN-10, DET-03, I1).
 *
 * Here the input is not a frame to parse but a SCRIPT (see fuzz_script.h): the
 * bytes are read as a sequence of event choices, each step building the
 * messages the arbiter is fed at the next tick. The logic is fuzzed, not only
 * the parser: the messages are aligned with the current state (sender, mandate,
 * tick) half the time, so the orders are really accepted and the deep branches
 * (handover, certification, disagreement, fallback, testament, rate limit) are
 * reached.
 *
 * The snapshot/restore pair (MAN-11) is fuzzed too: the actuator restores its
 * state from a file at start-up, so that file is an input an attacker could
 * try to write.
 */
#include "relais/arbiter.h"

#include <math.h>

#include "fuzz_script.h"
#include "fuzz_util.h"

#define ARB_MAX_MESSAGES ((size_t)RELAIS_IN_MSG_MAX)

/* Build one message out of its record. The fields stay inside the domain a
 * decoded frame would have (a decoded frame is always well formed), the values
 * are free. */
static void build_msg(relais_msg *m, const uint8_t *rec, relais_tick tick, const relais_arb *a) {
  memset(m, 0, sizeof *m);
  m->type = (relais_msg_type)(1u + (uint32_t)(rec[0] % 7u));
  m->sender = (relais_node)(1u + (uint32_t)(rec[1] % 4u));
  m->flags = rec[2];
  m->value = fuzz_script_f32(rec + 4);
  m->testament_len = (uint8_t)(rec[8] % (RELAIS_TESTAMENT_MAX + 1u));
  m->testament[0] = fuzz_script_f32(rec + 16);
  m->testament[1] = fuzz_script_f32(rec + 20);
  m->replica_age = rec[9];
  m->reason = rec[10];
  m->has_order = rec[11] != 0u;
  m->seen_mandate = fuzz_script_u32(rec + 12);
  if ((rec[3] & 1u) != 0u) {
    /* Aligned with the arbiter: the message looks like the one the holder
     * would really send, so it is accepted and the logic is exercised. */
    m->tick = tick;
    m->mandate = a->mandate;
    if (a->holder != RELAIS_NODE_NONE) {
      m->sender = a->holder;
      if (m->type == RELAIS_MSG_OMBRE)
        m->sender = a->holder == RELAIS_NODE_A ? RELAIS_NODE_B : RELAIS_NODE_A;
    }
  } else {
    m->tick = (relais_tick)fuzz_script_u32(rec + 12);
    m->mandate = fuzz_script_u32(rec + 16) & 0xffffu;
  }
  if (m->type == RELAIS_MSG_ETAT) {
    m->ctl_state_len = (uint8_t)(rec[9] % 16u);
    for (uint8_t i = 0; i < m->ctl_state_len; i++) m->ctl_state[i] = rec[(i % 8u) + 16u];
  }
}

/* What the arbiter promises after every tick (spec 4.4, MAN-07, ACT-05). */
static void check_step(const relais_arb *a, const relais_arb_out *out, const relais_params *p,
                       relais_mandate last_mandate) {
  fuzz_require(out->applied); /* exactly one value per tick */
  fuzz_require(isfinite(out->value));
  fuzz_require(fabsf(out->value) <= p->max_amplitude); /* ACT-05 */
  fuzz_require(out->event_count <= RELAIS_EVENTS_MAX);
  fuzz_require(out->mandate == a->mandate && out->holder == a->holder);
  fuzz_require(a->mandate >= last_mandate); /* the mandate never goes back */
  fuzz_require(out->statut.mandate == out->mandate);
  fuzz_require(out->statut.holder == out->holder);
  fuzz_require(out->statut.meas.applied == out->value);
  /* MAN-07: the STATUT announces the state the machines must act on from now,
   * while out->certified is the certification that served this tick's choice
   * (arbiter.c keeps both on purpose), so only the STATUT is compared here. */
  fuzz_require(out->statut.certified == a->certified);
  fuzz_require(out->statut.certified_node == a->certified_node);
}

/* The state file of the actuator (MAN-11): a snapshot must restore, a corrupted
 * one must be refused without touching the arbiter. */
static void fuzz_snapshot(const uint8_t *data, size_t size) {
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);
  uint8_t snap[64];
  size_t n = relais_arb_snapshot(&a, snap, sizeof snap);
  fuzz_require(n > 0u);

  relais_arb restored;
  relais_arb_init(&restored, &p);
  fuzz_require(relais_arb_restore(&restored, snap, n));
  fuzz_require(restored.mandate == a.mandate && restored.holder == a.holder);

  memcpy(snap, data, size < n ? size : n);
  relais_arb_init(&restored, &p);
  (void)relais_arb_restore(&restored, snap, size < n ? size : n);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);

  size_t at = 0;
  relais_tick tick = 0;
  relais_mandate last_mandate = a.mandate;

  while (at < size) {
    size_t nmsg = (size_t)(data[at] % (ARB_MAX_MESSAGES + 1u));
    at++;
    if (at + nmsg * FUZZ_SCRIPT_REC + FUZZ_SCRIPT_MEAS > size) break;
    relais_msg msgs[RELAIS_IN_MSG_MAX];
    for (size_t i = 0; i < nmsg; i++) {
      build_msg(&msgs[i], data + at + i * FUZZ_SCRIPT_REC, tick + 1u, &a);
    }
    at += nmsg * FUZZ_SCRIPT_REC;
    relais_meas meas;
    memset(&meas, 0, sizeof meas);
    meas.pos = fuzz_script_f32(data + at);
    meas.vel = fuzz_script_f32(data + at + 4u);
    meas.applied = fuzz_script_f32(data + at + 8u);
    meas.setpoint = fuzz_script_f32(data + at + 12u);
    at += FUZZ_SCRIPT_MEAS;
    tick++;
    relais_arb_out out;
    relais_arb_step(&a, tick, msgs, nmsg, &meas, &out);
    check_step(&a, &out, &p, last_mandate);
    last_mandate = a.mandate;
  }

  fuzz_snapshot(data, size);
  return 0;
}
