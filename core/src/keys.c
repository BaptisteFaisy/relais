/* Relais — key agreement and key rotation (SEC-07, ideas 31 and 32).
 *
 * The exchange is a three-flight challenge/response over the pre-shared key:
 *
 *   flight 1  initiator -> responder   nonce_I || tag1
 *   flight 2  responder -> initiator   nonce_R || tag2
 *   flight 3  initiator -> responder   tag3
 *
 *   tagN = SipHash-2-4(PSK, label || stage || initiator || responder || nonces)
 *
 * tag1 proves to the responder that the initiator holds the PSK. tag2 proves the
 * same to the initiator and binds nonce_R, so the initiator knows its session
 * key is fresh and not one an attacker chose. tag3 binds the whole transcript:
 * replaying the two first flights of a captured session cannot complete, because
 * the third tag cannot be produced without the PSK. Both ends then derive the
 * same 128-bit session key from the transcript, under two labels.
 *
 * Pure C11: no clock, no file, no socket, no allocation. The entropy that makes
 * a nonce fresh is handed in by the caller.
 */
#include "relais/keys.h"

#include <string.h>

/* Domain separation labels: one per derivation, so two uses of the same PSK can
 * never produce the same value for different purposes. */
#define LBL_NONCE "relais-kx-nonce"
#define LBL_TAG   "relais-kx-tag"
#define LBL_KEY   "relais-kx-key"

/* One SipHash-2-4 over label || bytes..., all fed as chunks (no copy). */
static uint64_t kx_hash(const relais_key *psk, const char *label, const uint8_t *parts[],
                        const size_t lens[], size_t count) {
  relais_siphash h;
  relais_siphash_init(&h, psk);
  relais_siphash_update(&h, label, strlen(label));
  relais_siphash_update(&h, "", 1); /* the label is NUL terminated: no ambiguity */
  for (size_t i = 0; i < count; i++) relais_siphash_update(&h, parts[i], lens[i]);
  return relais_siphash_final(&h);
}

/* The node ids of the two ends, in transcript order: the initiator first. They
 * are part of every derivation, so a session key cannot be replayed onto the
 * other link of the same machine (SEC-02 keys are per link). */
static void kx_ids(const relais_kx *kx, uint8_t ids[2]) {
  ids[0] = (kx->role == RELAIS_KX_INITIATOR) ? (uint8_t)kx->self : (uint8_t)kx->peer;
  ids[1] = (kx->role == RELAIS_KX_INITIATOR) ? (uint8_t)kx->peer : (uint8_t)kx->self;
}

/* Expand the caller's entropy into a nonce: SipHash in counter mode under the
 * PSK. Four independent 64-bit blocks, so a repeated entropy buffer still gives
 * a nonce that only somebody holding the PSK could have predicted. */
static void kx_nonce(const relais_kx *kx, const uint8_t *entropy, size_t entropy_len,
                     uint8_t out[RELAIS_KX_NONCE_LEN]) {
  uint8_t ids[2];
  kx_ids(kx, ids);
  for (uint8_t block = 0; block < RELAIS_KX_NONCE_LEN / 8u; block++) {
    const uint8_t *parts[3] = { ids, &block, entropy };
    const size_t lens[3] = { 2u, 1u, entropy_len };
    relais_put_u64le(out + (size_t)block * 8u, kx_hash(&kx->psk, LBL_NONCE, parts, lens, 3u));
  }
}

/* The tag of one flight: stage 1 covers nonce_I, stages 2 and 3 cover both. */
static uint64_t kx_tag(const relais_kx *kx, uint8_t stage) {
  uint8_t ids[2];
  kx_ids(kx, ids);
  const uint8_t *parts[4] = { &stage, ids, kx->nonce_self, kx->nonce_peer };
  size_t lens[4] = { 1u, 2u, RELAIS_KX_NONCE_LEN, RELAIS_KX_NONCE_LEN };
  /* On the initiator side nonce_self is nonce_I; on the responder side the
   * transcript order is still initiator first, so the two halves are swapped. */
  if (kx->role == RELAIS_KX_RESPONDER) {
    parts[2] = kx->nonce_peer;
    parts[3] = kx->nonce_self;
  }
  size_t count = (stage == 1u) ? 3u : 4u;
  return kx_hash(&kx->psk, LBL_TAG, parts, lens, count);
}

/* Constant time comparison of two tags: the time spent must not say how many
 * bytes of a forged tag were right (SEC-02 reasoning, applied to the exchange). */
static bool tag_equal(uint64_t a, uint64_t b) {
  uint64_t diff = a ^ b;
  diff |= diff >> 32;
  diff |= diff >> 16;
  diff |= diff >> 8;
  diff |= diff >> 4;
  diff |= diff >> 2;
  diff |= diff >> 1;
  return (diff & 1u) == 0u;
}

/* The session key: two labelled derivations over the finished transcript, so the
 * 128 bits do not rest on a single 64-bit output. */
static void kx_derive_session(const relais_kx *kx, relais_key *out) {
  uint8_t ids[2];
  kx_ids(kx, ids);
  const uint8_t *ni = (kx->role == RELAIS_KX_INITIATOR) ? kx->nonce_self : kx->nonce_peer;
  const uint8_t *nr = (kx->role == RELAIS_KX_INITIATOR) ? kx->nonce_peer : kx->nonce_self;
  static const char *const halves[2] = { LBL_KEY "-0", LBL_KEY "-1" };
  for (size_t half = 0; half < 2u; half++) {
    const uint8_t *parts[3] = { ids, ni, nr };
    const size_t lens[3] = { 2u, RELAIS_KX_NONCE_LEN, RELAIS_KX_NONCE_LEN };
    relais_put_u64le(out->bytes + half * 8u, kx_hash(&kx->psk, halves[half], parts, lens, 3u));
  }
  out->enabled = true;
}

bool relais_kx_begin(relais_kx *kx, const relais_key *psk, relais_node self,
                     relais_node peer, relais_kx_role role, const void *entropy,
                     size_t entropy_len) {
  if (kx == NULL) return false;
  /* Clear FIRST, before any validation. A refused handshake must leave no state
   * behind: `relais_kx_session_key` only tests kx->done, so a stale non-zero
   * `done` in a reused or uninitialised struct would hand out stack residue as a
   * session key. Found by the CI runner (gcc 14.2), whose stack layout differs
   * from the development host (gcc 13.3): the check at test_keys.c:307 failed
   * there and passed here, which is exactly the signature of uninitialised state
   * being read as if it were a completed handshake (SEC-02, SEC-07). */
  memset(kx, 0, sizeof *kx);
  if (psk == NULL || !psk->enabled) return false;
  if (self == RELAIS_NODE_NONE || peer == RELAIS_NODE_NONE || self == peer) return false;
  if (entropy == NULL || entropy_len == 0u) return false;
  kx->role = role;
  kx->self = self;
  kx->peer = peer;
  kx->psk = *psk;
  kx_nonce(kx, (const uint8_t *)entropy, entropy_len, kx->nonce_self);
  return true;
}

/* Build a flight: the payload (a nonce when the flight carries one) then the tag. */
static bool kx_build(const relais_kx *kx, uint8_t stage, bool with_nonce,
                     const uint8_t *nonce, uint8_t *out, size_t cap) {
  size_t need = with_nonce ? RELAIS_KX_CHALLENGE_LEN : RELAIS_KX_CONFIRM_LEN;
  if (out == NULL || cap < need) return false;
  size_t at = 0;
  if (with_nonce) {
    memcpy(out, nonce, RELAIS_KX_NONCE_LEN);
    at = RELAIS_KX_NONCE_LEN;
  }
  relais_put_u64le(out + at, kx_tag(kx, stage));
  return true;
}

/* Consume a flight: check its length, its tag, then remember the peer nonce. */
static bool kx_consume(relais_kx *kx, uint8_t stage, bool with_nonce, const void *msg,
                       size_t len) {
  size_t need = with_nonce ? RELAIS_KX_CHALLENGE_LEN : RELAIS_KX_CONFIRM_LEN;
  if (kx == NULL || msg == NULL || len != need) return false;
  const uint8_t *p = (const uint8_t *)msg;
  const uint64_t got = relais_get_u64le(p + need - RELAIS_KX_TAG_LEN);
  if (with_nonce) memcpy(kx->nonce_peer, p, RELAIS_KX_NONCE_LEN);
  if (!tag_equal(kx_tag(kx, stage), got)) {
    if (with_nonce) memset(kx->nonce_peer, 0, sizeof kx->nonce_peer);
    return false;
  }
  if (with_nonce) kx->have_peer_nonce = true;
  kx->peer_proved = true;
  return true;
}

bool relais_kx_hello(const relais_kx *kx, uint8_t *out, size_t cap) {
  if (kx == NULL || kx->role != RELAIS_KX_INITIATOR) return false;
  return kx_build(kx, 1u, true, kx->nonce_self, out, cap);
}

bool relais_kx_on_hello(relais_kx *kx, const void *msg, size_t len) {
  if (kx == NULL || kx->role != RELAIS_KX_RESPONDER || kx->have_peer_nonce) return false;
  return kx_consume(kx, 1u, true, msg, len);
}

bool relais_kx_reply(const relais_kx *kx, uint8_t *out, size_t cap) {
  if (kx == NULL || kx->role != RELAIS_KX_RESPONDER || !kx->have_peer_nonce) return false;
  return kx_build(kx, 2u, true, kx->nonce_self, out, cap);
}

bool relais_kx_on_reply(relais_kx *kx, const void *msg, size_t len) {
  if (kx == NULL || kx->role != RELAIS_KX_INITIATOR || kx->have_peer_nonce) return false;
  return kx_consume(kx, 2u, true, msg, len);
}

bool relais_kx_confirm(relais_kx *kx, uint8_t *out, size_t cap) {
  if (kx == NULL || kx->role != RELAIS_KX_INITIATOR || !kx->have_peer_nonce) return false;
  if (!kx_build(kx, 3u, false, NULL, out, cap)) return false;
  kx_derive_session(kx, &kx->session);
  kx->done = true;
  return true;
}

bool relais_kx_on_confirm(relais_kx *kx, const void *msg, size_t len) {
  if (kx == NULL || kx->role != RELAIS_KX_RESPONDER || !kx->have_peer_nonce) return false;
  if (!kx_consume(kx, 3u, false, msg, len)) return false;
  kx_derive_session(kx, &kx->session);
  kx->done = true;
  return true;
}

bool relais_kx_session_key(const relais_kx *kx, relais_key *out) {
  if (kx == NULL || out == NULL || !kx->done) return false;
  *out = kx->session;
  return true;
}

void relais_kx_clear(relais_kx *kx) {
  if (kx == NULL) return;
  relais_key_clear(&kx->psk);
  relais_key_clear(&kx->session);
  memset(kx->nonce_self, 0, sizeof kx->nonce_self);
  memset(kx->nonce_peer, 0, sizeof kx->nonce_peer);
  kx->have_peer_nonce = false;
  kx->peer_proved = false;
  kx->done = false;
}

/* ------------------------------------------------------------------- rotation */
void relais_keyring_init(relais_keyring *kr, const relais_key *current) {
  if (kr == NULL) return;
  memset(kr, 0, sizeof *kr);
  if (current != NULL) kr->current = *current;
}

bool relais_keyring_stage(relais_keyring *kr, const relais_key *next, uint32_t window) {
  if (kr == NULL || next == NULL || !next->enabled) return false;
  if (kr->staged) return false; /* one rotation at a time: no queued keys */
  kr->next = *next;
  kr->window = window;
  kr->staged = true;
  return true;
}

const relais_key *relais_keyring_tx(const relais_keyring *kr) {
  return (kr != NULL) ? &kr->current : NULL;
}

/* The keys a receiver accepts right now, in order: the current one, then the
 * staged one while the transition window is open. One rule, used by both the
 * control check and the full decode, so the two can never disagree. */
static size_t rx_keys(const relais_keyring *kr, const relais_key **out) {
  out[0] = &kr->current;
  if (kr->staged && kr->window > 0u) {
    out[1] = &kr->next;
    return 2u;
  }
  return 1u;
}

int relais_keyring_rx(const relais_keyring *kr, const void *frame, size_t len,
                      uint64_t got) {
  if (kr == NULL) return -1;
  const relais_key *keys[2];
  const size_t count = rx_keys(kr, keys);
  for (size_t i = 0; i < count; i++)
    if (relais_check_verify(keys[i], frame, len, got)) return (int)i;
  return -1;
}

bool relais_keyring_decode(const relais_keyring *kr, const uint8_t *buf, size_t len,
                           relais_msg *out, relais_reject *why) {
  if (kr == NULL) return false;
  const relais_key *keys[2];
  const size_t count = rx_keys(kr, keys);
  for (size_t i = 0; i < count; i++) {
    if (relais_frame_decode(buf, len, keys[i], out, why)) {
      if (why != NULL) *why = RELAIS_REJ_NONE; /* the first key was not the right one */
      return true;
    }
  }
  return false;
}

bool relais_keyring_promote(relais_keyring *kr) {
  if (kr == NULL || !kr->staged) return false;
  relais_key_clear(&kr->current);
  kr->current = kr->next;
  relais_key_clear(&kr->next);
  kr->staged = false;
  kr->window = 0u;
  kr->rotations++;
  return true;
}

void relais_keyring_advance(relais_keyring *kr) {
  if (kr == NULL || !kr->staged) return;
  if (kr->window > 0u) kr->window--;
  if (kr->window == 0u) relais_keyring_promote(kr);
}

void relais_keyring_clear(relais_keyring *kr) {
  if (kr == NULL) return;
  relais_key_clear(&kr->current);
  relais_key_clear(&kr->next);
  kr->staged = false;
  kr->window = 0u;
}
