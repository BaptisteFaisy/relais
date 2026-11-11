/* Relais — unit tests for frame integrity (spec 6.1, 5.10).
 *
 * Requirements covered here:
 *   SEC-01  malformed / unknown version / unknown sender frames are rejected;
 *   SEC-02  every frame carries an authenticator computed with a link key; a
 *           frame forged without the key is rejected (one modified byte is
 *           enough), and a single flipped bit is always detected;
 *   DET-04  an invalid control is the reason reported, so the caller can count
 *           the frame as lost.
 *
 * The check vector set is the published one: CRC-32C check value, the 64
 * official SipHash-2-4 vectors (key 00..0f, input 00..n-1) and the published
 * 64-byte output. All are pure constants: the test reads no clock.
 */
#include "test_util.h"

#include <string.h>

/* Published SipHash-2-4 vectors: key 00..0f, input 00..n-1, output of the
 * reference implementation. SIP64[n] is the 8-byte authenticator of the
 * first n input bytes, little-endian. */
static const uint8_t SIP64[64][8] = {
  {0x31, 0x0e, 0x0e, 0xdd, 0x47, 0xdb, 0x6f, 0x72,}, {0xfd, 0x67, 0xdc, 0x93, 0xc5, 0x39, 0xf8, 0x74,},
  {0x5a, 0x4f, 0xa9, 0xd9, 0x09, 0x80, 0x6c, 0x0d,}, {0x2d, 0x7e, 0xfb, 0xd7, 0x96, 0x66, 0x67, 0x85,},
  {0xb7, 0x87, 0x71, 0x27, 0xe0, 0x94, 0x27, 0xcf,}, {0x8d, 0xa6, 0x99, 0xcd, 0x64, 0x55, 0x76, 0x18,},
  {0xce, 0xe3, 0xfe, 0x58, 0x6e, 0x46, 0xc9, 0xcb,}, {0x37, 0xd1, 0x01, 0x8b, 0xf5, 0x00, 0x02, 0xab,},
  {0x62, 0x24, 0x93, 0x9a, 0x79, 0xf5, 0xf5, 0x93,}, {0xb0, 0xe4, 0xa9, 0x0b, 0xdf, 0x82, 0x00, 0x9e,},
  {0xf3, 0xb9, 0xdd, 0x94, 0xc5, 0xbb, 0x5d, 0x7a,}, {0xa7, 0xad, 0x6b, 0x22, 0x46, 0x2f, 0xb3, 0xf4,},
  {0xfb, 0xe5, 0x0e, 0x86, 0xbc, 0x8f, 0x1e, 0x75,}, {0x90, 0x3d, 0x84, 0xc0, 0x27, 0x56, 0xea, 0x14,},
  {0xee, 0xf2, 0x7a, 0x8e, 0x90, 0xca, 0x23, 0xf7,}, {0xe5, 0x45, 0xbe, 0x49, 0x61, 0xca, 0x29, 0xa1,},
  {0xdb, 0x9b, 0xc2, 0x57, 0x7f, 0xcc, 0x2a, 0x3f,}, {0x94, 0x47, 0xbe, 0x2c, 0xf5, 0xe9, 0x9a, 0x69,},
  {0x9c, 0xd3, 0x8d, 0x96, 0xf0, 0xb3, 0xc1, 0x4b,}, {0xbd, 0x61, 0x79, 0xa7, 0x1d, 0xc9, 0x6d, 0xbb,},
  {0x98, 0xee, 0xa2, 0x1a, 0xf2, 0x5c, 0xd6, 0xbe,}, {0xc7, 0x67, 0x3b, 0x2e, 0xb0, 0xcb, 0xf2, 0xd0,},
  {0x88, 0x3e, 0xa3, 0xe3, 0x95, 0x67, 0x53, 0x93,}, {0xc8, 0xce, 0x5c, 0xcd, 0x8c, 0x03, 0x0c, 0xa8,},
  {0x94, 0xaf, 0x49, 0xf6, 0xc6, 0x50, 0xad, 0xb8,}, {0xea, 0xb8, 0x85, 0x8a, 0xde, 0x92, 0xe1, 0xbc,},
  {0xf3, 0x15, 0xbb, 0x5b, 0xb8, 0x35, 0xd8, 0x17,}, {0xad, 0xcf, 0x6b, 0x07, 0x63, 0x61, 0x2e, 0x2f,},
  {0xa5, 0xc9, 0x1d, 0xa7, 0xac, 0xaa, 0x4d, 0xde,}, {0x71, 0x65, 0x95, 0x87, 0x66, 0x50, 0xa2, 0xa6,},
  {0x28, 0xef, 0x49, 0x5c, 0x53, 0xa3, 0x87, 0xad,}, {0x42, 0xc3, 0x41, 0xd8, 0xfa, 0x92, 0xd8, 0x32,},
  {0xce, 0x7c, 0xf2, 0x72, 0x2f, 0x51, 0x27, 0x71,}, {0xe3, 0x78, 0x59, 0xf9, 0x46, 0x23, 0xf3, 0xa7,},
  {0x38, 0x12, 0x05, 0xbb, 0x1a, 0xb0, 0xe0, 0x12,}, {0xae, 0x97, 0xa1, 0x0f, 0xd4, 0x34, 0xe0, 0x15,},
  {0xb4, 0xa3, 0x15, 0x08, 0xbe, 0xff, 0x4d, 0x31,}, {0x81, 0x39, 0x62, 0x29, 0xf0, 0x90, 0x79, 0x02,},
  {0x4d, 0x0c, 0xf4, 0x9e, 0xe5, 0xd4, 0xdc, 0xca,}, {0x5c, 0x73, 0x33, 0x6a, 0x76, 0xd8, 0xbf, 0x9a,},
  {0xd0, 0xa7, 0x04, 0x53, 0x6b, 0xa9, 0x3e, 0x0e,}, {0x92, 0x59, 0x58, 0xfc, 0xd6, 0x42, 0x0c, 0xad,},
  {0xa9, 0x15, 0xc2, 0x9b, 0xc8, 0x06, 0x73, 0x18,}, {0x95, 0x2b, 0x79, 0xf3, 0xbc, 0x0a, 0xa6, 0xd4,},
  {0xf2, 0x1d, 0xf2, 0xe4, 0x1d, 0x45, 0x35, 0xf9,}, {0x87, 0x57, 0x75, 0x19, 0x04, 0x8f, 0x53, 0xa9,},
  {0x10, 0xa5, 0x6c, 0xf5, 0xdf, 0xcd, 0x9a, 0xdb,}, {0xeb, 0x75, 0x09, 0x5c, 0xcd, 0x98, 0x6c, 0xd0,},
  {0x51, 0xa9, 0xcb, 0x9e, 0xcb, 0xa3, 0x12, 0xe6,}, {0x96, 0xaf, 0xad, 0xfc, 0x2c, 0xe6, 0x66, 0xc7,},
  {0x72, 0xfe, 0x52, 0x97, 0x5a, 0x43, 0x64, 0xee,}, {0x5a, 0x16, 0x45, 0xb2, 0x76, 0xd5, 0x92, 0xa1,},
  {0xb2, 0x74, 0xcb, 0x8e, 0xbf, 0x87, 0x87, 0x0a,}, {0x6f, 0x9b, 0xb4, 0x20, 0x3d, 0xe7, 0xb3, 0x81,},
  {0xea, 0xec, 0xb2, 0xa3, 0x0b, 0x22, 0xa8, 0x7f,}, {0x99, 0x24, 0xa4, 0x3c, 0xc1, 0x31, 0x57, 0x24,},
  {0xbd, 0x83, 0x8d, 0x3a, 0xaf, 0xbf, 0x8d, 0xb7,}, {0x0b, 0x1a, 0x2a, 0x32, 0x65, 0xd5, 0x1a, 0xea,},
  {0x13, 0x50, 0x79, 0xa3, 0x23, 0x1c, 0xe6, 0x60,}, {0x93, 0x2b, 0x28, 0x46, 0xe4, 0xd7, 0x06, 0x66,},
  {0xe1, 0x91, 0x5f, 0x5c, 0xb1, 0xec, 0xa4, 0x6c,}, {0xf3, 0x25, 0x96, 0x5c, 0xa1, 0x6d, 0x62, 0x9f,},
  {0x57, 0x5f, 0xf2, 0x8e, 0x60, 0x38, 0x1b, 0xe5,}, {0x72, 0x45, 0x06, 0xeb, 0x4c, 0x32, 0x8a, 0x95,}
};


/* A real, encoded frame used by the one-byte alteration test. */
static size_t build_real_frame(uint8_t *buf, size_t cap, const relais_key *k) {
  relais_msg m;
  relais_params p;
  relais_params_default(&p);
  t_order(&m, RELAIS_NODE_A, 7u, 4242u, 0.375f, &p);
  return relais_frame_encode(&m, k, buf, cap);
}

static void fill_key(relais_key *k) {
  T_CHECK(relais_key_from_hex(k, "000102030405060708090a0b0c0d0e0f"));
  T_CHECK(k->enabled);
}

void test_crc32c_known_vectors(void) {
  t_section("CRC-32C published check values");
  T_EQ_U(relais_crc32c("", 0), 0x00000000u);
  T_EQ_U(relais_crc32c("123456789", 9), 0xE3069283u);
  /* The running form is the raw one: continued feeding equals one shot. */
  uint32_t running = relais_crc32c_update(0xFFFFFFFFu, "1234", 4);
  running = relais_crc32c_update(running, "56789", 5);
  T_EQ_U(~running, 0xE3069283u);
  /* A NULL pointer is tolerated (no read, no crash). */
  T_EQ_U(relais_crc32c(NULL, 0), 0x00000000u);
  T_EQ_U(relais_crc32c_update(0xFFFFFFFFu, NULL, 4), 0xFFFFFFFFu);
}

/* The 64 published SipHash-2-4 vectors, key 00..0f, input 00..n-1. */
void test_sec02_siphash_official_vectors(void) {
  t_section("SEC-02 SipHash-2-4 published vectors");
  relais_key k;
  fill_key(&k);
  for (unsigned n = 0; n < 64u; n++) {
    uint8_t in[64];
    uint64_t want = 0;
    for (unsigned i = 0; i < n; i++) in[i] = (uint8_t)i;
    for (int b = 7; b >= 0; b--) want = (want << 8) | SIP64[n][(unsigned)b];
    T_EQ_U(relais_mac(&k, in, n), want);
  }
  /* The value quoted in the brief belongs to the 15-byte input, not to the
   * 64-byte one: the published set gives a129ca6149be45e5 for n = 15 and
   * acd2c40b8502cad8 for n = 64. Both are asserted, so a regression on either
   * length is caught. */
  uint8_t in[64];
  for (unsigned i = 0; i < 64u; i++) in[i] = (uint8_t)i;
  T_EQ_U(relais_mac(&k, in, 15), 0xa129ca6149be45e5ull);
  T_EQ_U(relais_mac(&k, in, 64), 0xacd2c40b8502cad8ull);
  /* A different key must give a different authenticator (SEC-02). */
  relais_key other;
  T_CHECK(relais_key_from_hex(&other, "0f0e0d0c0b0a09080706050403020100"));
  T_CHECK(relais_mac(&other, in, 64) != relais_mac(&k, in, 64));
  /* No key at all: zero, never a valid-looking authenticator. */
  T_EQ_U(relais_mac(NULL, in, 64), 0u);
}

void test_key_from_hex_accepts_and_rejects(void) {
  t_section("key parsing: 32 hex digits, and every refusal");
  relais_key k;
  relais_key_clear(&k);
  T_CHECK(relais_key_from_hex(&k, "0123456789abcdefABCDEF0123456789"));
  T_CHECK(k.enabled);
  T_EQ_U(k.bytes[0], 0x01u);
  T_EQ_U(k.bytes[1], 0x23u);
  T_EQ_U(k.bytes[8], 0xABu); /* upper case accepted */
  T_EQ_U(k.bytes[15], 0x89u);

  relais_key before = k;
  T_CHECK(!relais_key_from_hex(&k, "0123456789abcdef"));            /* too short */
  T_CHECK(!relais_key_from_hex(&k, "0123456789abcdef0123456789abcdef0")); /* too long */
  T_CHECK(!relais_key_from_hex(&k, "0123456789abcdef0123456789abcdeg")); /* not hex */
  T_CHECK(!relais_key_from_hex(&k, ""));                             /* empty */
  T_CHECK(!relais_key_from_hex(&k, NULL));                           /* NULL string */
  T_CHECK(!relais_key_from_hex(NULL, "0123456789abcdef0123456789abcdef")); /* NULL key */
  /* A refused key leaves the key untouched and unusable. */
  T_CHECK(memcmp(&before, &k, sizeof k) == 0);
  relais_key cleared;
  memset(&cleared, 0xff, sizeof cleared);
  relais_key_clear(&cleared);
  T_CHECK(!cleared.enabled);
  for (size_t i = 0; i < sizeof cleared.bytes; i++) T_EQ_U(cleared.bytes[i], 0u);
}

void test_check_compute_and_verify_keyed_and_not(void) {
  t_section("check compute/verify, with and without a key");
  uint8_t frame[RELAIS_FRAME_MAX];
  relais_key k;
  fill_key(&k);

  /* Level S: CRC-32C, 32 bits wide, zero extended in the 64-bit trailer. */
  size_t n = build_real_frame(frame, sizeof frame, NULL);
  T_CHECK(n > RELAIS_HEADER_LEN + RELAIS_CHECK_LEN);
  uint64_t crc_check = relais_check_compute(NULL, frame, n - RELAIS_CHECK_LEN);
  T_EQ_U(crc_check, (uint64_t)relais_crc32c(frame, n - RELAIS_CHECK_LEN));
  T_CHECK(crc_check <= 0xFFFFFFFFull);
  T_CHECK(relais_check_verify(NULL, frame, n - RELAIS_CHECK_LEN, crc_check));
  T_CHECK(!relais_check_verify(NULL, frame, n - RELAIS_CHECK_LEN, crc_check ^ 1u));

  /* A key that parsed is a key that is used: the MAC replaces the CRC. */
  size_t nk = build_real_frame(frame, sizeof frame, &k);
  uint64_t mac = relais_check_compute(&k, frame, nk - RELAIS_CHECK_LEN);
  T_EQ_U(mac, relais_mac(&k, frame, nk - RELAIS_CHECK_LEN));
  T_CHECK(mac != crc_check);
  T_CHECK(relais_check_verify(&k, frame, nk - RELAIS_CHECK_LEN, mac));
  /* A CRC verified against a keyed link is refused, and vice versa. */
  T_CHECK(!relais_check_verify(&k, frame, nk - RELAIS_CHECK_LEN, crc_check));
  T_CHECK(!relais_check_verify(NULL, frame, nk - RELAIS_CHECK_LEN, mac));
}

/* SEC-02: one altered byte, anywhere, is always caught. */
void test_sec02_single_byte_alteration_detected(void) {
  t_section("SEC-02 one altered byte, 1000 random positions");
  uint8_t frame[RELAIS_FRAME_MAX], copy[RELAIS_FRAME_MAX];
  relais_key k;
  fill_key(&k);
  size_t n = build_real_frame(frame, sizeof frame, &k);
  T_CHECK(n > RELAIS_HEADER_LEN + RELAIS_CHECK_LEN);
  const size_t body = n - RELAIS_CHECK_LEN; /* the bytes the control covers */
  t_rng rng;
  t_rng_seed(&rng, 0xC0FFEEu);
  long caught = 0;
  for (long i = 0; i < 1000; i++) {
    memcpy(copy, frame, n);
    size_t at = (size_t)t_rng_below(&rng, (uint32_t)body);
    uint8_t mask = (uint8_t)(1u << t_rng_below(&rng, 8u));
    copy[at] = (uint8_t)(copy[at] ^ mask);
    uint64_t got = relais_frame_read_u64(copy + body);
    if (!relais_check_verify(&k, copy, body, got)) caught++;
  }
  T_EQ_U((uint64_t)caught, 1000u);
  /* Altering the trailer itself is caught too. */
  memcpy(copy, frame, n);
  uint64_t got = relais_frame_read_u64(copy + body);
  T_CHECK(!relais_check_verify(&k, copy, body, got ^ 0x8000000000000000ull));
}

/* SEC-02: a frame forged without the key never decodes on a keyed link. */
void test_sec02_forged_frame_rejected(void) {
  t_section("SEC-02 forged frame rejected by the decoder");
  uint8_t buf[RELAIS_FRAME_MAX];
  relais_key k, attacker;
  fill_key(&k);
  relais_key_clear(&attacker); /* the attacker has no key: CRC only */
  relais_msg m, out;
  relais_params p;
  relais_params_default(&p);
  t_order(&m, RELAIS_NODE_A, 9u, 900u, 0.5f, &p);
  size_t n = relais_frame_encode(&m, &attacker, buf, sizeof buf);
  T_CHECK(n > 0);
  relais_reject why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, &k, &out, &why));
  T_EQ_U(why, RELAIS_REJ_CHECK);
  /* The same bytes, read by a link with no key, are accepted: the trailer is
   * a CRC there. That is what makes the key the only barrier at level C. */
  relais_msg ok;
  T_CHECK(relais_frame_decode(buf, n, NULL, &ok, &why));
  T_EQ_U(ok.value, m.value);
  T_EQ_U(ok.tick, m.tick);
  /* An authentic frame under the right key still decodes (no over-rejection). */
  n = relais_frame_encode(&m, &k, buf, sizeof buf);
  T_CHECK(relais_frame_decode(buf, n, &k, &ok, &why));
  T_EQ_U(ok.value, m.value);
}

int main(void) {
  test_crc32c_known_vectors();
  test_sec02_siphash_official_vectors();
  test_key_from_hex_accepts_and_rejects();
  test_check_compute_and_verify_keyed_and_not();
  test_sec02_single_byte_alteration_detected();
  test_sec02_forged_frame_rejected();
  return t_summary();
}
