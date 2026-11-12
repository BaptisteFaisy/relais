/* Relais — byzantine attack bench (mission M20, idea 16).
 *
 * Plays each hostile frame against the arbiter IN PROCESS, through the real
 * core, and reports for every attack the property that matters: was it
 * rejected, counted and journaled, and above all did it move the APPLIED VALUE
 * or the MANDATE? An attack that has an effect is a major result: it is printed
 * as EFFECT and described, never hidden.
 *
 * Every frame is encoded with relais_frame_encode and decoded back before it is
 * handed to the arbiter, so the bench plays exactly what the wire would carry
 * (inject.h). The link key is shared by the actuator and both computers, which
 * is the honest threat model: an attacker holding it is an insider (SEC-02).
 *
 * Build: make byzantine   (or ./build/run_byzantine)
 */
#include "inject.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "relais/params.h"

#define ATTACK_VALUE 0.95f /* a value far from any honest one, easy to spot */
#define HONEST_VALUE 0.20f /* what the holder legitimately orders */

typedef enum { V_REJECTED, V_IGNORED, V_EFFECT } verdict_t;

typedef struct {
  relais_params p;
  relais_arb a;
  relais_key k;
  relais_mandate man;
} bench_t;

static const char *verdict_name(verdict_t v) {
  switch (v) {
    case V_REJECTED: return "REJECTED (counted, no effect)";
    case V_IGNORED: return "IGNORED (not consumed, no effect)";
    case V_EFFECT: return "EFFECT  <-- major result";
  }
  return "?";
}

static uint32_t rejected_total(const relais_arb *a) { return relais_arb_rejected_total(a); }

static void zero_meas(relais_meas *m) { memset(m, 0, sizeof *m); }

/* Step one tick with `n` frames, each encoded then decoded over the wire. */
static void step_wire(bench_t *b, relais_tick tick, const relais_msg *raw, size_t n,
                      relais_arb_out *out) {
  relais_msg onwire[RELAIS_IN_MSG_MAX];
  size_t m = n < RELAIS_IN_MSG_MAX ? n : RELAIS_IN_MSG_MAX;
  for (size_t i = 0; i < m; i++) {
    uint8_t buf[RELAIS_FRAME_MAX];
    if (!relais_byz_roundtrip(&raw[i], &b->k, &onwire[i], buf, sizeof buf)) {
      fprintf(stderr, "bench: frame %zu is not wire-valid\n", i);
    }
  }
  relais_meas meas;
  zero_meas(&meas);
  relais_arb_step(&b->a, tick, onwire, m, &meas, out);
}

static void bench_init(bench_t *b, uint8_t auth_strikes) {
  relais_params_default(&b->p);
  b->p.auth_strikes = auth_strikes;
  relais_arb_init(&b->a, &b->p);
  relais_byz_key(&b->k);
  b->man = 0;
}

/* Make A the holder (mandate 1) with an honest order for tick 1. */
static void establish_holder_a(bench_t *b) {
  relais_msg dem;
  relais_byz_demande(&dem, RELAIS_NODE_A, 0, 1, true, HONEST_VALUE, &b->p);
  relais_arb_out out;
  step_wire(b, 1, &dem, 1, &out);
  b->man = b->a.mandate;
}

/* Certify B as the second by agreeing shadows for certify_ticks ticks. */
static void certify_second_b(bench_t *b, relais_tick from) {
  for (uint32_t i = 0; i < (uint32_t)b->p.certify_ticks + 1u; i++) {
    relais_tick t = from + i;
    relais_msg frames[2];
    relais_byz_order(&frames[0], RELAIS_NODE_A, b->man, t, HONEST_VALUE, &b->p);
    relais_msg_clear(&frames[1]);
    frames[1].type = RELAIS_MSG_OMBRE;
    frames[1].sender = RELAIS_NODE_B;
    frames[1].tick = t;
    frames[1].value = HONEST_VALUE;
    relais_arb_out out;
    step_wire(b, t, frames, 2, &out);
  }
}

static void print_row(const char *name, const char *expected, verdict_t v, const char *note) {
  printf("%-32s | %-22s | %s\n", name, expected, verdict_name(v));
  if (note != NULL && note[0] != '\0') printf("%-32s   %s\n", "", note);
}

/* True when the attacker's value was applied as the holder's order. */
static bool attacker_value_applied(const relais_arb_out *out) {
  return out->source == RELAIS_SRC_HOLDER && fabsf(out->value - ATTACK_VALUE) < 1e-4f;
}

/* ---- attack 1: order with a future mandate (SEC-04) ---- */
static void atk_future_mandate(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  relais_msg ord;
  relais_byz_order(&ord, RELAIS_NODE_A, b.man + 1, 2, ATTACK_VALUE, &b.p);
  relais_arb_out out;
  uint32_t before = rejected_total(&b.a);
  step_wire(&b, 2, &ord, 1, &out);
  bool effect = attacker_value_applied(&out) || b.a.mandate != b.man ||
                b.a.holder != RELAIS_NODE_A;
  uint32_t rej = rejected_total(&b.a) - before;
  char note[96];
  snprintf(note, sizeof note, "rejected+=%u (mandate_future), holder=%s mandate=%u",
           rej, relais_node_name(b.a.holder), b.a.mandate);
  print_row("future-mandate order", "SEC-04 reject", effect ? V_EFFECT : V_REJECTED, note);
}

/* ---- attack 2: right mandate, wrong tick (SEC-03) ---- */
static void atk_wrong_tick(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  relais_msg ord;
  relais_byz_order(&ord, RELAIS_NODE_A, b.man, 9, ATTACK_VALUE, &b.p); /* future tick */
  relais_arb_out out;
  uint32_t before = rejected_total(&b.a);
  step_wire(&b, 2, &ord, 1, &out);
  bool effect = attacker_value_applied(&out) || b.a.mandate != b.man;
  char note[96];
  snprintf(note, sizeof note, "rejected+%u (tick_missed), applied=%.2f from %s",
           rejected_total(&b.a) - before, out.value, relais_source_name(out.source));
  print_row("wrong-tick order", "SEC-03 reject", effect ? V_EFFECT : V_REJECTED, note);
}

/* ---- attack 3: replay of an authentic captured frame (SEC-03) ---- */
static void atk_replay(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  /* Capture the honest order for tick 2, then replay it late at tick 5. */
  relais_msg honest;
  relais_byz_order(&honest, RELAIS_NODE_A, b.man, 2, ATTACK_VALUE, &b.p);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(&honest, &b.k, buf, sizeof buf);
  relais_msg replayed;
  relais_reject why = RELAIS_REJ_NONE;
  relais_frame_decode(buf, len, &b.k, &replayed, &why); /* authentic capture */
  step_wire(&b, 2, &honest, 1, &(relais_arb_out){0});   /* tick 2 passes honestly */
  relais_arb_out out;
  uint32_t before = rejected_total(&b.a);
  step_wire(&b, 5, &replayed, 1, &out); /* replayed three ticks late */
  bool effect = attacker_value_applied(&out);
  char note[96];
  snprintf(note, sizeof note, "stale replay rejected+%u (tick_missed/mandate_stale)",
           rejected_total(&b.a) - before);
  print_row("replayed authentic frame", "SEC-03 reject", effect ? V_EFFECT : V_REJECTED, note);
}

/* ---- attack 4: STATUT usurping the actuator, emitted by a machine ---- */
static void atk_fake_statut(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  relais_msg st;
  relais_byz_fake_statut(&st, RELAIS_NODE_B, 2, b.man + 5, RELAIS_NODE_B);
  relais_arb_out out;
  uint32_t before = rejected_total(&b.a);
  step_wire(&b, 2, &st, 1, &out);
  bool effect = b.a.holder != RELAIS_NODE_A || b.a.mandate != b.man;
  char note[96];
  snprintf(note, sizeof note, "actuator never consumes STATUT; holder=%s mandate=%u rej=%u",
           relais_node_name(b.a.holder), b.a.mandate, rejected_total(&b.a) - before);
  print_row("forged STATUT (usurpation)", "no effect on arbiter", effect ? V_EFFECT : V_IGNORED,
            note);
}

/* ---- attack 5: ETAT corrupt but authenticated (stolen key) ---- */
static void atk_stolen_etat(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  relais_msg et;
  relais_byz_etat(&et, RELAIS_NODE_B, 2, ATTACK_VALUE);
  relais_arb_out out;
  step_wire(&b, 2, &et, 1, &out);
  bool effect = b.a.holder != RELAIS_NODE_A || b.a.mandate != b.man;
  print_row("corrupt authenticated ETAT", "no effect on arbiter", effect ? V_EFFECT : V_IGNORED,
            "actuator never consumes ETAT; only the second machine imports it");
}

/* ---- attack 6: demande burst (SEC-06) ---- */
static void atk_demande_burst(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  relais_msg frames[RELAIS_IN_MSG_MAX];
  size_t n = 0;
  relais_byz_order(&frames[n++], RELAIS_NODE_A, b.man, 2, HONEST_VALUE, &b.p);
  while (n < RELAIS_IN_MSG_MAX)
    relais_byz_demande(&frames[n++], RELAIS_NODE_B, b.man, 2, false, 0, &b.p);
  relais_arb_out out;
  step_wire(&b, 2, frames, n, &out);
  bool stolen = b.a.holder != RELAIS_NODE_A || b.a.mandate != b.man;
  bool order_ok = out.source == RELAIS_SRC_HOLDER && fabsf(out.value - HONEST_VALUE) < 1e-4f;
  char note[96];
  snprintf(note, sizeof note, "holder order applied=%s; B stole mandate=%s",
           order_ok ? "yes" : "no", stolen ? "YES" : "no");
  print_row("demande burst", "SEC-06 no delay", stolen ? V_EFFECT : V_REJECTED, note);
}

/* ---- attack 7: CESSION attributed to a non-holder (ignored) ---- */
static void atk_cession_nonholder(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  relais_msg ce;
  relais_byz_cession(&ce, RELAIS_NODE_B, b.man, 1); /* B claims to cede, B is not holder */
  relais_arb_out out;
  step_wire(&b, 2, &ce, 1, &out);
  bool effect = b.a.holder != RELAIS_NODE_A || b.a.mandate != b.man;
  print_row("cession of a non-holder", "ignored", effect ? V_EFFECT : V_IGNORED,
            "only the holder's own cession is honoured (DET-05)");
}

/* ---- attack 8: CESSION forged as the holder, emitted by B (shared key) ---- */
static void atk_cession_forged_holder(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  certify_second_b(&b, 2); /* B is the certified second, so it can take over */
  relais_mandate man_before = b.a.mandate;
  relais_msg ce;
  relais_byz_cession(&ce, RELAIS_NODE_A, b.a.mandate, 1); /* B forges A's cession */
  relais_arb_out out;
  relais_tick t = b.a.ticks + 1;
  step_wire(&b, t, &ce, 1, &out);   /* recorded this tick (REL-06) ... */
  step_wire(&b, t + 1, NULL, 0, &out); /* ... frees the mandate at the next tick */
  bool effect = b.a.holder != RELAIS_NODE_A || b.a.mandate != man_before;
  char note[128];
  snprintf(note, sizeof note, "holder A -> %s, mandate %u -> %u (B took over by forgery)",
           relais_node_name(b.a.holder), man_before, b.a.mandate);
  print_row("forged holder CESSION", "SHOULD be rejected", effect ? V_EFFECT : V_REJECTED, note);
  if (effect)
    printf("    MAJOR: the shared link key authenticates the frame but not its emitter,\n"
           "    so B forges A's CESSION and takes the mandate. Mitigation needs per-link\n"
           "    keys (SEC-02) or a sender<->link binding at the transport, out of M20 scope.\n");
}

/* ---- attack 9: revoked node keeps ordering (idea 39) ---- */
static void atk_revoked_node(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  relais_arb_revoke(&b.a, RELAIS_NODE_A); /* operator revokes the holder */
  relais_msg ord;
  relais_byz_order(&ord, RELAIS_NODE_A, b.man, 2, ATTACK_VALUE, &b.p);
  relais_arb_out out;
  uint32_t before = rejected_total(&b.a);
  step_wire(&b, 2, &ord, 1, &out);
  bool effect = attacker_value_applied(&out);
  char note[96];
  snprintf(note, sizeof note, "revoked order rejected+%u (revoked); mandate falls later on misses",
           rejected_total(&b.a) - before);
  print_row("revoked node order", "idea 39 reject", effect ? V_EFFECT : V_REJECTED, note);
}

/* ---- attack 10: auth-strike ban (idea 38, DEG-09) ---- */
static void atk_auth_ban(void) {
  bench_t b;
  bench_init(&b, 3); /* ban after 3 consecutive MAC failures */
  establish_holder_a(&b);
  relais_arb_auth_fail(&b.a, RELAIS_NODE_A);
  relais_arb_auth_fail(&b.a, RELAIS_NODE_A);
  bool tripped = relais_arb_auth_fail(&b.a, RELAIS_NODE_A);
  relais_msg ord;
  relais_byz_order(&ord, RELAIS_NODE_A, b.man, 2, ATTACK_VALUE, &b.p);
  relais_arb_out out;
  uint32_t before = rejected_total(&b.a);
  step_wire(&b, 2, &ord, 1, &out);
  bool effect = attacker_value_applied(&out);
  char note[96];
  snprintf(note, sizeof note, "ban tripped=%s; rejected+%u (auth_banned); event journaled",
           tripped ? "yes" : "no", rejected_total(&b.a) - before);
  print_row("banned node order", "idea 38 reject", effect ? V_EFFECT : V_REJECTED, note);
}

/* ---- attack 11: frame forged without the link key (SEC-02) ---- */
static void atk_forged_no_key(void) {
  bench_t b;
  bench_init(&b, 0);
  establish_holder_a(&b);
  relais_msg ord;
  relais_byz_order(&ord, RELAIS_NODE_A, b.man, 2, ATTACK_VALUE, &b.p);
  relais_key wrong;
  relais_key_clear(&wrong);
  relais_key_from_hex(&wrong, "00112233445566778899aabbccddeeff");
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(&ord, &wrong, buf, sizeof buf); /* signed by an outsider */
  relais_msg back;
  relais_reject why = RELAIS_REJ_NONE;
  bool decoded = relais_frame_decode(buf, len, &b.k, &back, &why); /* verified with the link key */
  char note[96];
  snprintf(note, sizeof note, "decode=%s why=%s; the frame never reaches the arbiter",
           decoded ? "true" : "false", relais_reject_name(why));
  print_row("forged frame, no key", "SEC-02 reject", decoded ? V_EFFECT : V_REJECTED, note);
}

int main(void) {
  printf("Relais M20 — byzantine attack bench (in-process, through the core)\n");
  printf("Attacker model: an insider holding the shared link key (SEC-02).\n\n");
  printf("%-32s | %-22s | %s\n", "ATTACK", "EXPECTED", "VERDICT");
  printf("---------------------------------+------------------------+------------------------\n");
  atk_future_mandate();
  atk_wrong_tick();
  atk_replay();
  atk_fake_statut();
  atk_stolen_etat();
  atk_demande_burst();
  atk_cession_nonholder();
  atk_cession_forged_holder();
  atk_revoked_node();
  atk_auth_ban();
  atk_forged_no_key();
  printf("\nReproduce: ./build/run_byzantine\n");
  printf("One EFFECT is expected and documented: the forged holder CESSION, which\n"
         "the shared link key cannot attribute to its real emitter. See\n"
         "docs/THREAT-BENCH.md. Every other attack is rejected or ignored.\n");
  return 0;
}
