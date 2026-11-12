/* Relais — unit tests for the byzantine frontiers (mission M20, ideas 16/38/39).
 *
 * Two new core mechanisms are proved here, plus the frame-level properties of
 * the arbiter under hostile input:
 *   - the revocation list (idea 39, MAN-11): a revoked sender sees every frame
 *     rejected and counted, and the revocation survives a restart;
 *   - the safe degraded mode on the authenticator (idea 38, DEG-09, SEC-02):
 *     after N consecutive MAC failures a sender is banned and the event is
 *     journaled; auth_strikes == 0 (the default) disables it entirely;
 *   - the arbiter rejects a future-mandate order (SEC-04), a wrong-tick or
 *     replayed order (SEC-03), ignores a STATUT or an ETAT it never consumes,
 *     rate-limits a demande burst (SEC-06), and never lets a forged frame move
 *     the applied value or the mandate.
 *
 * The default behaviour (empty revocation list, auth_strikes == 0) is checked
 * first as a non-regression: nothing changes unless the bench asks for it.
 */
#include "test_util.h"

#include <string.h>

/* A local bench key so this suite stays independent of host/byzantine. */
void relais_byz_key_local(relais_key *k);

/* --- small helpers local to this suite (no duplication with the fixtures) --- */

static void zero_meas(relais_meas *m) { memset(m, 0, sizeof *m); }

static bool has_event(const relais_arb_out *out, relais_event_kind kind) {
  for (uint8_t i = 0; i < out->event_count; i++)
    if (out->events[i].kind == kind) return true;
  return false;
}

/* Encode then decode one message over the wire: proves the frame is valid for
 * the protocol before it is handed to the arbiter (SEC-01, SEC-02). */
static bool wire(const relais_msg *m, const relais_key *k, relais_msg *out) {
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(m, k, buf, sizeof buf);
  if (len == 0) return false;
  relais_reject why = RELAIS_REJ_NONE;
  return relais_frame_decode(buf, len, k, out, &why);
}

/* Make A the holder with a valid order for `tick`; returns the mandate (1). */
static relais_mandate establish_holder_a(relais_arb *a, const relais_params *p, relais_tick tick) {
  relais_msg dem, onwire;
  t_demande(&dem, RELAIS_NODE_A, 0, tick, true, 0.10f, p);
  T_CHECK(wire(&dem, NULL, &onwire));
  relais_meas meas;
  zero_meas(&meas);
  relais_arb_out out;
  relais_arb_step(a, tick, &onwire, 1, &meas, &out);
  T_EQ_U(a->holder, RELAIS_NODE_A);
  return a->mandate;
}

/* One tick feeding `n` wire-valid frames; returns the applied source. */
static relais_source step_frames(relais_arb *a, relais_tick tick, const relais_msg *raw, size_t n,
                                 const relais_key *k, relais_arb_out *out) {
  relais_msg onwire[RELAIS_IN_MSG_MAX];
  for (size_t i = 0; i < n; i++) T_CHECK(wire(&raw[i], k, &onwire[i]));
  relais_meas meas;
  zero_meas(&meas);
  relais_arb_step(a, tick, onwire, n, &meas, out);
  return out->source;
}

/* ------------------------------------------------------- non-regression ---- */

static void test_default_unchanged(void) {
  t_section("M20 non-regression: empty list + auth_strikes 0 = unchanged");
  relais_params p;
  relais_params_default(&p);
  T_EQ_U(p.auth_strikes, 0); /* the mechanism is off by default */

  relais_arb a;
  relais_arb_init(&a, &p);
  for (int i = RELAIS_NODE_A; i <= RELAIS_NODE_C; i++) {
    T_CHECK(!relais_arb_is_revoked(&a, (relais_node)i));
    T_CHECK(!relais_arb_is_banned(&a, (relais_node)i));
  }
  /* With the mechanism off, a hundred reported failures ban nobody. */
  for (int r = 0; r < 100; r++) T_CHECK(!relais_arb_auth_fail(&a, RELAIS_NODE_A));
  T_CHECK(!relais_arb_is_banned(&a, RELAIS_NODE_A));

  /* A holder's valid order is still applied exactly as before. */
  relais_mandate man = establish_holder_a(&a, &p, 1);
  relais_msg ord;
  t_order(&ord, RELAIS_NODE_A, man, 2, 0.20f, &p);
  relais_arb_out out;
  relais_source src = step_frames(&a, 2, &ord, 1, NULL, &out);
  T_EQ_U(src, RELAIS_SRC_HOLDER);
  T_NEAR(out.value, 0.20f, 1e-6);
  T_EQ_U(relais_arb_rejected_total(&a), 0);
}

/* ------------------------------------------------------------ revocation ---- */

static void test_revoke_rejects_and_counts(void) {
  t_section("idea 39: a revoked sender is rejected, counted, no effect");
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_mandate man = establish_holder_a(&a, &p, 1);

  relais_arb_revoke(&a, RELAIS_NODE_A);
  T_CHECK(relais_arb_is_revoked(&a, RELAIS_NODE_A));
  T_CHECK(!relais_arb_is_revoked(&a, RELAIS_NODE_B));

  /* A's still-valid order for this tick is now refused by the guard. */
  relais_msg ord;
  t_order(&ord, RELAIS_NODE_A, man, 2, 0.99f, &p);
  relais_arb_out out;
  relais_source src = step_frames(&a, 2, &ord, 1, NULL, &out);
  T_CHECK(src != RELAIS_SRC_HOLDER);      /* the revoked order is not applied */
  T_CHECK(out.value < 0.98f);             /* its value never reaches the rack */
  T_EQ_U(a.rejected[RELAIS_REJ_REVOKED], 1);
  T_CHECK(has_event(&out, RELAIS_EV_ORDER_REJECTED));
  T_CHECK(has_event(&out, RELAIS_EV_NODE_REVOKED)); /* journaled (DET-03 style) */
  /* The mandate itself is not touched by the revocation on this very tick; it
   * only falls later, on the actuator's own p+1 miss evidence (MAN-04). */
  T_EQ_U(a.mandate, man);
}

static void test_revoke_survives_restart(void) {
  t_section("idea 39 / MAN-11: revocation survives a restart");
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);
  establish_holder_a(&a, &p, 1);
  relais_arb_revoke(&a, RELAIS_NODE_A);

  uint8_t snap[64];
  size_t len = relais_arb_snapshot(&a, snap, sizeof snap);
  T_CHECK(len > 0);
  relais_arb b;
  relais_arb_init(&b, &p);
  T_CHECK(!relais_arb_is_revoked(&b, RELAIS_NODE_A));
  T_CHECK(relais_arb_restore(&b, snap, len));
  T_CHECK(relais_arb_is_revoked(&b, RELAIS_NODE_A)); /* still revoked (MAN-11) */
}

static void test_revoke_blocks_certification(void) {
  t_section("idea 39: a revoked second never gets certified");
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_mandate man = establish_holder_a(&a, &p, 1);
  relais_arb_revoke(&a, RELAIS_NODE_B);

  /* Holder orders and B shadows agree for certify_ticks ticks, but B is
   * revoked, so its shadow is dropped and B is never certified. */
  for (uint32_t t = 2; t <= (uint32_t)(2 + p.certify_ticks + 1); t++) {
    relais_msg frames[2];
    t_order(&frames[0], RELAIS_NODE_A, man, (relais_tick)t, 0.30f, &p);
    t_shadow(&frames[1], RELAIS_NODE_B, (relais_tick)t, 0.30f, 0);
    relais_arb_out out;
    step_frames(&a, (relais_tick)t, frames, 2, NULL, &out);
  }
  T_CHECK(!a.certified);
  T_CHECK(a.rejected[RELAIS_REJ_REVOKED] >= 1);
}

/* ------------------------------------------------- safe degraded (strikes) -- */

static void test_strikes_ban_after_n(void) {
  t_section("idea 38 / DEG-09: ban after N consecutive auth failures");
  relais_params p;
  relais_params_default(&p);
  p.auth_strikes = 3;
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_mandate man = establish_holder_a(&a, &p, 1);

  T_CHECK(!relais_arb_auth_fail(&a, RELAIS_NODE_A)); /* strike 1 */
  T_CHECK(!relais_arb_auth_fail(&a, RELAIS_NODE_A)); /* strike 2: not yet */
  T_CHECK(!relais_arb_is_banned(&a, RELAIS_NODE_A));
  T_CHECK(relais_arb_auth_fail(&a, RELAIS_NODE_A));  /* strike 3: banned */
  T_CHECK(relais_arb_is_banned(&a, RELAIS_NODE_A));

  /* The banned holder's valid order is refused and counted. */
  relais_msg ord;
  t_order(&ord, RELAIS_NODE_A, man, 2, 0.77f, &p);
  relais_arb_out out;
  relais_source src = step_frames(&a, 2, &ord, 1, NULL, &out);
  T_CHECK(src != RELAIS_SRC_HOLDER);
  T_CHECK(out.value < 0.76f);
  T_EQ_U(a.rejected[RELAIS_REJ_AUTH_BANNED], 1);
  T_CHECK(has_event(&out, RELAIS_EV_AUTH_BANNED)); /* journaled */
}

static void test_strikes_success_resets(void) {
  t_section("idea 38: a success clears the streak, not the ban");
  relais_params p;
  relais_params_default(&p);
  p.auth_strikes = 3;
  relais_arb a;
  relais_arb_init(&a, &p);
  T_CHECK(!relais_arb_auth_fail(&a, RELAIS_NODE_B));
  T_CHECK(!relais_arb_auth_fail(&a, RELAIS_NODE_B));
  relais_arb_auth_ok(&a, RELAIS_NODE_B);            /* streak reset */
  T_CHECK(!relais_arb_auth_fail(&a, RELAIS_NODE_B));
  T_CHECK(!relais_arb_auth_fail(&a, RELAIS_NODE_B));
  T_CHECK(!relais_arb_is_banned(&a, RELAIS_NODE_B)); /* only 2 in a row again */
  T_CHECK(relais_arb_auth_fail(&a, RELAIS_NODE_B));  /* 3rd in a row: banned */
  T_CHECK(relais_arb_is_banned(&a, RELAIS_NODE_B));
}

static void test_ban_survives_restart(void) {
  t_section("idea 38 / MAN-11: a ban survives a restart");
  relais_params p;
  relais_params_default(&p);
  p.auth_strikes = 1;
  relais_arb a;
  relais_arb_init(&a, &p);
  T_CHECK(relais_arb_auth_fail(&a, RELAIS_NODE_B));
  uint8_t snap[64];
  size_t len = relais_arb_snapshot(&a, snap, sizeof snap);
  relais_arb b;
  relais_arb_init(&b, &p);
  T_CHECK(relais_arb_restore(&b, snap, len));
  T_CHECK(relais_arb_is_banned(&b, RELAIS_NODE_B));
}

static void test_params_strikes(void) {
  t_section("idea 38: auth_strikes parses and validates");
  relais_params p;
  relais_params_default(&p);
  char why[128] = "";
  T_CHECK(relais_params_parse(&p, "strikes=3", why, sizeof why));
  T_EQ_U(p.auth_strikes, 3);
  relais_params_default(&p);
  p.auth_strikes = 200; /* out of the 0..32 band */
  T_CHECK(!relais_params_valid(&p, why, sizeof why));
}

/* ------------------------------------------------- byzantine frame rules ---- */

static void test_future_mandate_rejected(void) {
  t_section("SEC-04: an order with a future mandate is rejected");
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_mandate man = establish_holder_a(&a, &p, 1);
  relais_msg ord;
  t_order(&ord, RELAIS_NODE_A, man + 1, 2, 0.88f, &p); /* nobody appoints itself */
  relais_arb_out out;
  relais_source src = step_frames(&a, 2, &ord, 1, NULL, &out);
  T_CHECK(src != RELAIS_SRC_HOLDER);
  T_CHECK(out.value < 0.87f);
  T_EQ_U(a.rejected[RELAIS_REJ_MANDATE_FUTURE], 1);
  T_EQ_U(a.mandate, man); /* the mandate never moves on a forged claim */
}

static void test_wrong_tick_rejected(void) {
  t_section("SEC-03: a future or past tick, and a replay, are rejected");
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_mandate man = establish_holder_a(&a, &p, 1);

  relais_msg future;
  t_order(&future, RELAIS_NODE_A, man, 9, 0.66f, &p); /* targets a far tick */
  relais_arb_out out;
  relais_source src = step_frames(&a, 2, &future, 1, NULL, &out);
  T_CHECK(src != RELAIS_SRC_HOLDER);
  T_EQ_U(a.rejected[RELAIS_REJ_TICK_MISSED], 1);

  /* A replay: the honest order captured for tick 3, replayed at tick 4. */
  relais_msg honest, replayed;
  t_order(&honest, RELAIS_NODE_A, man, 3, 0.11f, &p);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(&honest, NULL, buf, sizeof buf);
  T_CHECK(len > 0);
  relais_reject why = RELAIS_REJ_NONE;
  T_CHECK(relais_frame_decode(buf, len, NULL, &replayed, &why)); /* authentic */
  relais_arb_out out2;
  relais_source src2 = step_frames(&a, 4, &replayed, 1, NULL, &out2); /* late */
  T_CHECK(src2 != RELAIS_SRC_HOLDER);
  T_CHECK(a.rejected[RELAIS_REJ_TICK_MISSED] >= 2);
}

static void test_statut_etat_ignored(void) {
  t_section("idea 16: a forged STATUT / stolen-key ETAT never moves the arbiter");
  relais_params p;
  relais_params_default(&p);
  relais_key k;
  relais_byz_key_local(&k);
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_mandate man = establish_holder_a(&a, &p, 1);

  /* A machine usurps the actuator and publishes a STATUT naming B holder. */
  relais_msg statut;
  relais_msg_clear(&statut);
  statut.type = RELAIS_MSG_STATUT;
  statut.sender = RELAIS_NODE_B;
  statut.tick = 2;
  statut.mandate = man + 5;
  statut.holder = RELAIS_NODE_B;

  /* A corrupt but authenticated ETAT (stolen link key). */
  relais_msg etat;
  relais_ctl_state st;
  relais_ctl_init(&st, &p);
  t_etat(&etat, RELAIS_NODE_B, 2, &st, 0.55f);
  for (uint8_t i = 0; i < etat.ctl_state_len; i++) etat.ctl_state[i] ^= 0xffu;

  relais_msg frames[2] = {statut, etat};
  relais_arb_out out;
  step_frames(&a, 2, frames, 2, &k, &out);
  /* The arbiter consumes neither a STATUT nor an ETAT: the forged status did
   * not grant B, and the corrupt state changed nothing. */
  T_CHECK(!has_event(&out, RELAIS_EV_MANDATE_GRANTED));
  T_CHECK(!has_event(&out, RELAIS_EV_MANDATE_AUTO_SWITCH));
  T_EQ_U(a.holder, RELAIS_NODE_A); /* the mandate never moved */
  T_EQ_U(a.mandate, man);
}

static void test_forged_without_key_rejected(void) {
  t_section("SEC-02: a frame forged without the link key never decodes");
  relais_key k1, k2;
  relais_byz_key_local(&k1);
  relais_key_clear(&k2);
  T_CHECK(relais_key_from_hex(&k2, "00112233445566778899aabbccddeeff"));
  relais_params p;
  relais_params_default(&p);
  relais_msg ord;
  t_order(&ord, RELAIS_NODE_A, 1, 1, 0.42f, &p);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(&ord, &k1, buf, sizeof buf); /* signed by k1 */
  T_CHECK(len > 0);
  relais_msg back;
  relais_reject why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, len, &k2, &back, &why)); /* verified by k2 */
  T_EQ_U(why, RELAIS_REJ_CHECK);
}

static void test_demande_burst_rate_limited(void) {
  t_section("SEC-06: a demande burst does not delay the holder's order");
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_mandate man = establish_holder_a(&a, &p, 1);

  /* The holder keeps ordering while B floods demandes in the same tick. */
  relais_msg frames[RELAIS_IN_MSG_MAX];
  size_t n = 0;
  t_order(&frames[n++], RELAIS_NODE_A, man, 2, 0.25f, &p);
  while (n < RELAIS_IN_MSG_MAX) t_demande(&frames[n++], RELAIS_NODE_B, man, 2, false, 0, &p);
  relais_arb_out out;
  relais_source src = step_frames(&a, 2, frames, n, NULL, &out);
  T_EQ_U(src, RELAIS_SRC_HOLDER); /* the order is applied despite the flood */
  T_NEAR(out.value, 0.25f, 1e-6);
  T_EQ_U(a.holder, RELAIS_NODE_A); /* B did not steal the mandate */
  T_EQ_U(a.mandate, man);
}

/* A local bench key so this suite stays independent of host/byzantine. */
void relais_byz_key_local(relais_key *k) {
  relais_key_clear(k);
  relais_key_from_hex(k, "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
}

int main(void) {
  test_default_unchanged();
  test_revoke_rejects_and_counts();
  test_revoke_survives_restart();
  test_revoke_blocks_certification();
  test_strikes_ban_after_n();
  test_strikes_success_resets();
  test_ban_survives_restart();
  test_params_strikes();
  test_future_mandate_rejected();
  test_wrong_tick_rejected();
  test_statut_etat_ignored();
  test_forged_without_key_rejected();
  test_demande_burst_rate_limited();
  return t_summary();
}
