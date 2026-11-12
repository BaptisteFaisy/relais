/* Relais — unit tests of the authenticated key exchange at start-up (SEC-07,
 * idea 31). The rotation of a key on a live link is tested in test_keyring.c,
 * so that each file stays small and proves one thing.
 *
 * What is proved here:
 *   SEC-07  both ends of a link derive the same session key at start-up, from an
 *           authenticated three-flight exchange over the pre-shared key;
 *   SEC-07  the session key is fresh at every start-up (new entropy, new key),
 *           bound to the link (the node ids are part of every derivation), and
 *           never the pre-shared key itself;
 *   SEC-02  a session key protects a real frame: a frame forged under another
 *           key is rejected by the decoder;
 *   SEC-03  a captured flight cannot complete a new exchange, so a captured
 *           session cannot be replayed into a fresh one.
 *
 * Every input is a constant or comes from the shared deterministic generator:
 * the test reads no clock and no device.
 */
#include "test_util.h"

#include <string.h>

#include "relais/keys.h"

#define PSK_A "000102030405060708090a0b0c0d0e0f"
#define PSK_B "0f0e0d0c0b0a09080706050403020100"

/* Two different start-ups: the entropy a caller would draw from /dev/random. */
static const uint8_t ENTROPY_1[32] = {
  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
  0xdd, 0xee, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
  0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
};
static const uint8_t ENTROPY_2[32] = {
  0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18, 0x29, 0x3a, 0x4b, 0x5c,
  0x6d, 0x7e, 0x8f, 0x90, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
  0x13, 0x57, 0x9b, 0xdf, 0x24, 0x68, 0xac, 0xe0
};

static bool begin(relais_kx *kx, const relais_key *k, relais_node self, relais_node peer,
                  relais_kx_role role, const uint8_t *entropy) {
  const bool ok = relais_kx_begin(kx, k, self, peer, role, entropy, 32u);
  T_CHECK(ok);
  return ok;
}

/* The whole exchange, flight by flight. The buffers stay available to the caller
 * so a test can replay or alter one of them afterwards. */
static bool run_exchange(relais_kx *ini, relais_kx *res, uint8_t f1[RELAIS_KX_CHALLENGE_LEN],
                         uint8_t f2[RELAIS_KX_CHALLENGE_LEN],
                         uint8_t f3[RELAIS_KX_CONFIRM_LEN]) {
  if (!relais_kx_hello(ini, f1, RELAIS_KX_CHALLENGE_LEN)) return false;
  if (!relais_kx_on_hello(res, f1, RELAIS_KX_CHALLENGE_LEN)) return false;
  if (!relais_kx_reply(res, f2, RELAIS_KX_CHALLENGE_LEN)) return false;
  if (!relais_kx_on_reply(ini, f2, RELAIS_KX_CHALLENGE_LEN)) return false;
  if (!relais_kx_confirm(ini, f3, RELAIS_KX_CONFIRM_LEN)) return false;
  return relais_kx_on_confirm(res, f3, RELAIS_KX_CONFIRM_LEN);
}

static bool both_sessions(const relais_kx *ini, const relais_kx *res, relais_key *si,
                          relais_key *sr) {
  return relais_kx_session_key(ini, si) && relais_kx_session_key(res, sr);
}

/* SEC-07: one start-up, one shared session key, and it is not the secret itself. */
void test_sec07_exchange_yields_one_shared_session_key(void) {
  t_section("SEC-07 authenticated exchange at start-up");
  relais_key secret;
  t_key(&secret, PSK_A);
  relais_kx ini, res;
  begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, &secret, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_2);
  uint8_t f1[RELAIS_KX_CHALLENGE_LEN], f2[RELAIS_KX_CHALLENGE_LEN], f3[RELAIS_KX_CONFIRM_LEN];
  T_CHECK(run_exchange(&ini, &res, f1, f2, f3));

  relais_key si, sr;
  T_CHECK(both_sessions(&ini, &res, &si, &sr));
  T_CHECK(si.enabled && sr.enabled);
  T_CHECK(memcmp(si.bytes, sr.bytes, sizeof si.bytes) == 0); /* one key, two ends */
  T_CHECK(memcmp(si.bytes, secret.bytes, sizeof si.bytes) != 0); /* not the PSK */
  /* The two nonces really differ, so the freshness is not an accident. */
  T_CHECK(memcmp(f1, f2, RELAIS_KX_NONCE_LEN) != 0);
  relais_kx_clear(&ini);
  relais_kx_clear(&res);
}

/* SEC-07: the key is renewed at every start-up, and the derivation is pure. */
void test_sec07_session_key_is_fresh_at_every_start_up(void) {
  t_section("SEC-07 a new start-up gives a new key");
  relais_key secret, first_a, first_b, second_a, second_b;
  t_key(&secret, PSK_A);
  for (int run = 0; run < 2; run++) {
    relais_kx ini, res;
    const uint8_t *e1 = (run == 0) ? ENTROPY_1 : ENTROPY_2;
    const uint8_t *e2 = (run == 0) ? ENTROPY_2 : ENTROPY_1;
    begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, e1);
    begin(&res, &secret, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, e2);
    uint8_t f1[RELAIS_KX_CHALLENGE_LEN], f2[RELAIS_KX_CHALLENGE_LEN],
        f3[RELAIS_KX_CONFIRM_LEN];
    T_CHECK(run_exchange(&ini, &res, f1, f2, f3));
    relais_key si, sr;
    T_CHECK(both_sessions(&ini, &res, &si, &sr));
    if (run == 0) { first_a = si; first_b = sr; } else { second_a = si; second_b = sr; }
    relais_kx_clear(&ini);
    relais_kx_clear(&res);
  }
  T_CHECK(memcmp(first_a.bytes, second_a.bytes, sizeof first_a.bytes) != 0);
  T_CHECK(memcmp(first_a.bytes, first_b.bytes, sizeof first_a.bytes) == 0);
  T_CHECK(memcmp(second_a.bytes, second_b.bytes, sizeof second_a.bytes) == 0);

  /* The same entropy gives the same key: the core is a pure function, so the
   * freshness comes from the entropy the caller hands in, never from a clock. */
  relais_key k1, k2;
  t_key(&k1, PSK_A);
  k2 = k1;
  relais_kx a1, a2, b1, b2;
  begin(&a1, &k1, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&b1, &k2, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_1);
  begin(&a2, &k1, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&b2, &k2, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_1);
  uint8_t f1[RELAIS_KX_CHALLENGE_LEN], f2[RELAIS_KX_CHALLENGE_LEN], f3[RELAIS_KX_CONFIRM_LEN];
  uint8_t g1[RELAIS_KX_CHALLENGE_LEN], g2[RELAIS_KX_CHALLENGE_LEN], g3[RELAIS_KX_CONFIRM_LEN];
  T_CHECK(run_exchange(&a1, &b1, f1, f2, f3));
  T_CHECK(run_exchange(&a2, &b2, g1, g2, g3));
  T_CHECK(memcmp(f1, g1, sizeof f1) == 0);
  relais_key sa, sb;
  T_CHECK(relais_kx_session_key(&a1, &sa) && relais_kx_session_key(&a2, &sb));
  T_CHECK(memcmp(sa.bytes, sb.bytes, sizeof sa.bytes) == 0);
  relais_kx_clear(&a1);
  relais_kx_clear(&b1);
  relais_kx_clear(&a2);
  relais_kx_clear(&b2);
}

/* SEC-02: the session key is a link key, bound to the two ends of that link. */
void test_sec07_session_key_is_bound_to_the_link(void) {
  t_section("SEC-07 the key belongs to one link");
  relais_key secret, ab_a, ab_b, ba_a, ba_b, ac_a, ac_b;
  t_key(&secret, PSK_A);
  relais_kx ini, res;
  uint8_t f1[RELAIS_KX_CHALLENGE_LEN], f2[RELAIS_KX_CHALLENGE_LEN], f3[RELAIS_KX_CONFIRM_LEN];

  begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, &secret, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_1);
  T_CHECK(run_exchange(&ini, &res, f1, f2, f3));
  T_CHECK(both_sessions(&ini, &res, &ab_a, &ab_b));

  /* The same two machines, the other way round: a different transcript. */
  relais_kx_clear(&ini);
  relais_kx_clear(&res);
  begin(&ini, &secret, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_RESPONDER, ENTROPY_1);
  T_CHECK(run_exchange(&ini, &res, f1, f2, f3));
  T_CHECK(both_sessions(&ini, &res, &ba_a, &ba_b));
  T_CHECK(memcmp(ab_a.bytes, ba_a.bytes, sizeof ab_a.bytes) != 0);

  /* Another peer on the same machine: another key, so one link never opens two. */
  relais_kx_clear(&ini);
  relais_kx_clear(&res);
  begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_C, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, &secret, RELAIS_NODE_C, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_1);
  T_CHECK(run_exchange(&ini, &res, f1, f2, f3));
  T_CHECK(both_sessions(&ini, &res, &ac_a, &ac_b));
  T_CHECK(memcmp(ab_a.bytes, ac_a.bytes, sizeof ab_a.bytes) != 0);
  relais_kx_clear(&ini);
  relais_kx_clear(&res);
}

/* SEC-07: without the secret, no flight is accepted and no key is derived. */
void test_sec07_wrong_secret_is_refused(void) {
  t_section("SEC-07 an attacker without the secret gets nothing");
  relais_key good, bad;
  t_key(&good, PSK_A);
  t_key(&bad, PSK_B);
  relais_kx ini, res;
  uint8_t f1[RELAIS_KX_CHALLENGE_LEN], f2[RELAIS_KX_CHALLENGE_LEN], f3[RELAIS_KX_CONFIRM_LEN];
  relais_key none;
  relais_key_clear(&none);

  /* The initiator does not hold the secret: its first flight is refused. */
  begin(&ini, &bad, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, &good, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_2);
  T_CHECK(relais_kx_hello(&ini, f1, sizeof f1));
  T_CHECK(!relais_kx_on_hello(&res, f1, sizeof f1));
  T_CHECK(!relais_kx_reply(&res, f2, sizeof f2)); /* nothing to reply to */
  T_CHECK(!relais_kx_session_key(&res, &none));

  /* The responder does not hold the secret: the initiator stops at flight 2. */
  relais_kx_clear(&ini);
  relais_kx_clear(&res);
  begin(&ini, &good, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, &bad, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_2);
  T_CHECK(relais_kx_hello(&ini, f1, sizeof f1));
  T_CHECK(!relais_kx_on_hello(&res, f1, sizeof f1));
  T_CHECK(!relais_kx_confirm(&ini, f3, sizeof f3));
  T_CHECK(!relais_kx_session_key(&ini, &none));
  relais_kx_clear(&ini);
  relais_kx_clear(&res);
}

/* SEC-02: one altered bit in a flight is enough to refuse it. The altered flight
 * is the one presented to the receiver, so what is proved is the tag check and
 * not the "already received" guard of the state machine. */
static bool altered_flight_refused(uint8_t flight, const relais_key *secret, t_rng *rng) {
  relais_kx ini, res;
  bool refused;
  begin(&ini, secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, secret, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_2);
  uint8_t f1[RELAIS_KX_CHALLENGE_LEN], f2[RELAIS_KX_CHALLENGE_LEN], f3[RELAIS_KX_CONFIRM_LEN];
  T_CHECK(relais_kx_hello(&ini, f1, sizeof f1));
  if (flight > 1) T_CHECK(relais_kx_on_hello(&res, f1, sizeof f1));
  T_CHECK(relais_kx_reply(&res, f2, sizeof f2) == (flight > 1));
  if (flight > 2) T_CHECK(relais_kx_on_reply(&ini, f2, sizeof f2));
  T_CHECK(relais_kx_confirm(&ini, f3, sizeof f3) == (flight > 2));
  uint8_t *msg = (flight == 1) ? f1 : (flight == 2) ? f2 : f3;
  const size_t len = (flight == 3) ? (size_t)RELAIS_KX_CONFIRM_LEN
                                   : (size_t)RELAIS_KX_CHALLENGE_LEN;
  const size_t at = (size_t)t_rng_below(rng, (uint32_t)len);
  msg[at] = (uint8_t)(msg[at] ^ (uint8_t)(1u << t_rng_below(rng, 8u)));
  refused = (flight == 1) ? !relais_kx_on_hello(&res, msg, len)
          : (flight == 2) ? !relais_kx_on_reply(&ini, msg, len)
                          : !relais_kx_on_confirm(&res, msg, len);
  relais_kx_clear(&ini);
  relais_kx_clear(&res);
  return refused;
}

void test_sec07_altered_flight_is_refused(void) {
  t_section("SEC-07 a tampered flight is refused");
  relais_key secret, none;
  t_key(&secret, PSK_A);
  relais_key_clear(&none);
  t_rng rng;
  t_rng_seed(&rng, 0x5EED5EEDu);
  long caught = 0;
  for (uint8_t flight = 1; flight <= 3u; flight++)
    for (long trial = 0; trial < 40; trial++)
      if (altered_flight_refused(flight, &secret, &rng)) caught++;
  T_EQ_U((uint64_t)caught, 120u); /* 3 flights x 40 altered bits, all refused */
  T_CHECK(!relais_kx_session_key(NULL, &none));
}

/* SEC-03: flights captured on one session cannot complete another one. */
void test_sec07_captured_flights_cannot_be_replayed(void) {
  t_section("SEC-03 a captured exchange cannot be replayed");
  relais_key secret, none;
  t_key(&secret, PSK_A);
  relais_key_clear(&none);
  relais_kx ini, res;
  uint8_t f1[RELAIS_KX_CHALLENGE_LEN], f2[RELAIS_KX_CHALLENGE_LEN], f3[RELAIS_KX_CONFIRM_LEN];
  begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, &secret, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_2);
  T_CHECK(run_exchange(&ini, &res, f1, f2, f3));
  relais_key captured;
  T_CHECK(relais_kx_session_key(&ini, &captured));
  relais_kx_clear(&ini);
  relais_kx_clear(&res);

  /* A new start-up on the same link: the captured flight 1 is a valid tag over
   * the old nonce, so the responder answers it, but flight 3 of the attacker
   * cannot cover the fresh nonce it just received. No key is ever derived. */
  begin(&res, &secret, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_1);
  T_CHECK(relais_kx_on_hello(&res, f1, sizeof f1));
  uint8_t f2b[RELAIS_KX_CHALLENGE_LEN];
  T_CHECK(relais_kx_reply(&res, f2b, sizeof f2b));
  T_CHECK(!relais_kx_on_confirm(&res, f3, sizeof f3)); /* the captured confirm */
  T_CHECK(!relais_kx_on_confirm(&res, f2, sizeof f2));  /* any other old bytes */
  T_CHECK(!relais_kx_session_key(&res, &none));

  /* The captured flight 2 does not satisfy a fresh initiator either: its tag
   * covers the old nonce of the initiator, not the new one. */
  begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_2);
  T_CHECK(relais_kx_hello(&ini, f1, sizeof f1));
  T_CHECK(!relais_kx_on_reply(&ini, f2b, sizeof f2b));
  T_CHECK(!relais_kx_session_key(&ini, &none));
  relais_kx_clear(&ini);
  relais_kx_clear(&res);
}

/* The state machine only ever runs forward, in one role. */
void test_sec07_exchange_refuses_out_of_order_calls(void) {
  t_section("SEC-07 the exchange runs in order, or not at all");
  relais_key secret, none;
  t_key(&secret, PSK_A);
  relais_key_clear(&none);
  relais_kx ini, res;
  uint8_t f1[RELAIS_KX_CHALLENGE_LEN], f2[RELAIS_KX_CHALLENGE_LEN], f3[RELAIS_KX_CONFIRM_LEN];

  /* Missing or incoherent arguments are refused, and no state is left behind. */
  T_CHECK(!relais_kx_begin(NULL, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR,
                           ENTROPY_1, 32u));
  T_CHECK(!relais_kx_begin(&ini, NULL, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR,
                           ENTROPY_1, 32u));
  relais_key disabled;
  relais_key_clear(&disabled);
  T_CHECK(!relais_kx_begin(&ini, &disabled, RELAIS_NODE_A, RELAIS_NODE_B,
                           RELAIS_KX_INITIATOR, ENTROPY_1, 32u));
  T_CHECK(!relais_kx_begin(&ini, &secret, RELAIS_NODE_NONE, RELAIS_NODE_B,
                           RELAIS_KX_INITIATOR, ENTROPY_1, 32u));
  T_CHECK(!relais_kx_begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_A,
                           RELAIS_KX_INITIATOR, ENTROPY_1, 32u));
  /* No entropy, no nonce: a predictable nonce would make every key guessable. */
  T_CHECK(!relais_kx_begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR,
                           NULL, 0u));
  T_CHECK(!relais_kx_begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR,
                           ENTROPY_1, 0u));
  T_CHECK(!relais_kx_session_key(&ini, &none));

  /* Roles are not interchangeable, and no flight exists before its turn. */
  begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, &secret, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_2);
  T_CHECK(!relais_kx_reply(&ini, f2, sizeof f2));    /* the initiator never replies */
  T_CHECK(!relais_kx_confirm(&ini, f3, sizeof f3));  /* not before flight 2 */
  T_CHECK(!relais_kx_hello(&res, f1, sizeof f1));    /* the responder never hellos */
  T_CHECK(!relais_kx_on_reply(&res, f2, sizeof f2));
  T_CHECK(!relais_kx_on_confirm(&ini, f3, sizeof f3));
  T_CHECK(relais_kx_hello(&ini, f1, sizeof f1));
  T_CHECK(!relais_kx_on_hello(&ini, f1, sizeof f1)); /* wrong role for this flight */
  T_CHECK(relais_kx_on_hello(&res, f1, sizeof f1));
  T_CHECK(!relais_kx_on_hello(&res, f1, sizeof f1)); /* one flight 1, not two */
  T_CHECK(!relais_kx_hello(&ini, f1, 4u));           /* a buffer too small */
  T_CHECK(!relais_kx_on_hello(&res, f1, 39u));       /* a flight of the wrong size */
  relais_kx_clear(&ini);
  relais_kx_clear(&res);
}

/* SEC-02: the derived key is a real link key, and it is the only one that works. */
void test_sec07_session_key_protects_a_real_frame(void) {
  t_section("SEC-02 the session key protects a frame");
  relais_key secret, session, other;
  t_key(&secret, PSK_A);
  t_key(&other, PSK_B);
  relais_kx ini, res;
  begin(&ini, &secret, RELAIS_NODE_A, RELAIS_NODE_B, RELAIS_KX_INITIATOR, ENTROPY_1);
  begin(&res, &secret, RELAIS_NODE_B, RELAIS_NODE_A, RELAIS_KX_RESPONDER, ENTROPY_2);
  uint8_t f1[RELAIS_KX_CHALLENGE_LEN], f2[RELAIS_KX_CHALLENGE_LEN], f3[RELAIS_KX_CONFIRM_LEN];
  T_CHECK(run_exchange(&ini, &res, f1, f2, f3));
  T_CHECK(relais_kx_session_key(&ini, &session));

  relais_params p;
  relais_params_default(&p);
  relais_msg order, out;
  t_order(&order, RELAIS_NODE_A, 3u, 4242u, 0.375f, &p);
  uint8_t buf[RELAIS_FRAME_MAX];
  const size_t n = relais_frame_encode(&order, &session, buf, sizeof buf);
  T_CHECK(n > 0);
  relais_reject why = RELAIS_REJ_NONE;
  T_CHECK(relais_frame_decode(buf, n, &session, &out, &why));
  T_EQ_U(out.tick, order.tick);
  T_NEAR(out.value, order.value, 0.0);
  /* The pre-shared key itself no longer opens the link: the session key did. */
  T_CHECK(!relais_frame_decode(buf, n, &secret, &out, &why));
  T_EQ_U(why, RELAIS_REJ_CHECK);
  T_CHECK(!relais_frame_decode(buf, n, &other, &out, &why));
  /* Clearing wipes the secret material: no key can be read back afterwards. */
  relais_kx_clear(&ini);
  T_CHECK(!relais_kx_session_key(&ini, &other));
  for (size_t i = 0; i < sizeof ini.psk.bytes; i++) T_EQ_U(ini.psk.bytes[i], 0u);
  T_CHECK(!ini.psk.enabled && !ini.done && !ini.have_peer_nonce);
  relais_kx_clear(&res);
}

int main(void) {
  test_sec07_exchange_yields_one_shared_session_key();
  test_sec07_session_key_is_fresh_at_every_start_up();
  test_sec07_session_key_is_bound_to_the_link();
  test_sec07_wrong_secret_is_refused();
  test_sec07_altered_flight_is_refused();
  test_sec07_captured_flights_cannot_be_replayed();
  test_sec07_exchange_refuses_out_of_order_calls();
  test_sec07_session_key_protects_a_real_frame();
  return t_summary();
}
