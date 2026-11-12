/* Relais — fuzz target for the wire parser (ROB-09, SEC-01, SEC-02, ROB-02).
 *
 * The bytes are handed to the decoder exactly as a datagram would be: no copy,
 * no rounding of the length, so a sanitizer sees any read outside what was
 * received. The decoded message is boxed between two sentinel patterns, which
 * catches a write outside relais_msg even where ASAN cannot (a stack structure
 * is not red-zoned field by field). Every accepted frame is then re-checked
 * against the trailer it carries: SEC-02 says a wrong control is never
 * accepted, at level S (CRC-32C) as well as at level C (keyed MAC).
 *
 * Build: clang -fsanitize=fuzzer,address,undefined (libFuzzer drives it), or
 * without clang, gcc -fsanitize=address,undefined plus host/fuzz/fuzz_driver.c
 * (seeded generator + corpus mutator). Both call LLVMFuzzerTestOneInput alike.
 */
#include "relais/frame.h"

#include <string.h>

#include "fuzz_util.h"

#define SENTINEL_WORDS  4u
#define SENTINEL_BEFORE 0xA5C3E1F7u
#define SENTINEL_AFTER  0x1B2D4F69u
/* A level C key, so the MAC path (relais_mac over an attacker-chosen length)
 * is fuzzed as well as the CRC path. */
#define FUZZ_KEY_HEX "00112233445566778899aabbccddeeff"

typedef struct {
  uint32_t before[SENTINEL_WORDS];
  relais_msg msg;
  uint32_t after[SENTINEL_WORDS];
} guarded;

static void guard_reset(guarded *g) {
  for (size_t i = 0; i < SENTINEL_WORDS; i++) {
    g->before[i] = SENTINEL_BEFORE;
    g->after[i] = SENTINEL_AFTER;
  }
  memset(&g->msg, 0xEE, sizeof g->msg);
}

static bool guard_intact(const guarded *g) {
  for (size_t i = 0; i < SENTINEL_WORDS; i++) {
    if (g->before[i] != SENTINEL_BEFORE || g->after[i] != SENTINEL_AFTER) return false;
  }
  return true;
}

/* Decode with one key, then prove the frame was accepted only on its control. */
static bool decode_guarded(const uint8_t *data, size_t size, const relais_key *k,
                           guarded *g, relais_reject *why) {
  guard_reset(g);
  bool ok = relais_frame_decode(data, size, k, &g->msg, why);
  fuzz_require(guard_intact(g));
  if (ok) {
    uint64_t want = relais_check_compute(k, data, size - RELAIS_CHECK_LEN);
    fuzz_require(want == relais_frame_read_u64(data + size - RELAIS_CHECK_LEN));
  }
  return ok;
}

/* The unchecked parser: the one an unauthenticated peer reaches first. */
static bool decode_unguarded_path(const uint8_t *data, size_t size, guarded *g) {
  guard_reset(g);
  bool ok = relais_frame_decode_unchecked(data, size, &g->msg);
  fuzz_require(guard_intact(g));
  return ok;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  relais_key key;
  relais_key_from_hex(&key, FUZZ_KEY_HEX);

  /* Level C (MAC) and level S (CRC): two independent authentication paths. */
  guarded mac_box;
  relais_reject why = RELAIS_REJ_NONE;
  bool ok_mac = decode_guarded(data, size, &key, &mac_box, &why);
  if (ok_mac) fuzz_require(why == RELAIS_REJ_NONE); /* MAN-02: a reason is set on a drop */

  guarded crc_box;
  bool ok_crc = decode_guarded(data, size, NULL, &crc_box, NULL);

  /* ROB-02: a second decode of the same bytes gives the same message. */
  guarded again;
  bool ok_again = decode_guarded(data, size, &key, &again, NULL);
  fuzz_require(ok_mac == ok_again);
  if (ok_mac) fuzz_require(memcmp(&mac_box.msg, &again.msg, sizeof again.msg) == 0);

  /* A frame the checked decoder accepted always parses unchecked, and the
   * unchecked decoder never writes outside the caller's structure. */
  guarded raw;
  bool ok_raw = decode_unguarded_path(data, size, &raw);
  fuzz_require(!ok_mac || ok_raw);
  fuzz_require(!ok_crc || ok_raw);
  return 0;
}
