/* Relais — key agreement and key rotation (SEC-07, ideas 31 and 32).
 *
 * Two mechanisms, both pure C11 (no clock, no file, no socket, no allocation,
 * no external library):
 *
 *  1. relais_kx — an authenticated key exchange run at every start-up. It
 *     proves that both ends hold the provisioned pre-shared key (PSK) and
 *     derives a fresh session key from two nonces, so the key that protects
 *     the link is never the long lived secret itself.
 *  2. relais_keyring — a two-slot key set (current + next) that rotates the
 *     session key while the protocol runs. The sender signs with the current
 *     key, the receiver accepts both during a transition window, then the next
 *     key becomes current: no tick is lost by the rotation.
 *
 * WHY THIS SCHEME (and not X25519). SEC-07 asks for keys renewed at every
 * start-up by an authenticated exchange; it does not ask for forward secrecy.
 * The link already needs a provisioned secret for SEC-02, so a PSK is not a new
 * trust root: it is the one the deployment already has. A three-flight
 * challenge/response keyed by SipHash-2-4 then gives mutual authentication,
 * freshness and key confirmation with a primitive that is already implemented
 * and validated against its 64 published vectors. Re-implementing X25519 would
 * add roughly 400 lines of unproven field arithmetic to a safety core, for a
 * property (forward secrecy) that no requirement claims and that a PSK scheme
 * cannot give anyway. The limits are stated in docs/SECURITY.md.
 *
 * Entropy: the core never reads a clock or a device. The caller supplies the
 * entropy bytes (on QNX: /dev/random, read by target/, never by core/).
 */
#ifndef RELAIS_KEYS_H
#define RELAIS_KEYS_H

#include "relais/check.h"
#include "relais/frame.h"
#include "relais/types.h"

#define RELAIS_KX_NONCE_LEN   32u
#define RELAIS_KX_TAG_LEN     8u
/* Flight 1 and flight 2 carry a nonce and a tag; flight 3 carries a tag only. */
#define RELAIS_KX_CHALLENGE_LEN (RELAIS_KX_NONCE_LEN + RELAIS_KX_TAG_LEN)
#define RELAIS_KX_CONFIRM_LEN   RELAIS_KX_TAG_LEN
/* The session key is 128 bits, the width of a relais_key. */
#define RELAIS_KX_SESSION_LEN   16u

typedef enum {
  RELAIS_KX_INITIATOR = 0, /* sends flight 1 and flight 3 */
  RELAIS_KX_RESPONDER = 1  /* sends flight 2 */
} relais_kx_role;

typedef struct {
  relais_kx_role role;
  relais_node self;
  relais_node peer;
  relais_key psk;                             /* copy, wiped by relais_kx_clear */
  uint8_t nonce_self[RELAIS_KX_NONCE_LEN];
  uint8_t nonce_peer[RELAIS_KX_NONCE_LEN];
  bool have_peer_nonce;
  bool peer_proved;   /* the peer proved it holds the PSK on this transcript */
  relais_key session; /* derived once the transcript is complete */
  bool done;
} relais_kx;

/* Start an exchange. Returns false when an argument is missing, when the PSK is
 * not enabled, or when no entropy is given: a nonce that is not drawn from
 * fresh entropy would make every session key predictable. */
bool relais_kx_begin(relais_kx *kx, const relais_key *psk, relais_node self,
                     relais_node peer, relais_kx_role role, const void *entropy,
                     size_t entropy_len);

/* The three flights. Each builder returns false when the state machine is not
 * at the right step, so a flight can never be produced out of order. */
bool relais_kx_hello(const relais_kx *kx, uint8_t *out, size_t cap);
bool relais_kx_on_hello(relais_kx *kx, const void *msg, size_t len);
bool relais_kx_reply(const relais_kx *kx, uint8_t *out, size_t cap);
bool relais_kx_on_reply(relais_kx *kx, const void *msg, size_t len);
bool relais_kx_confirm(relais_kx *kx, uint8_t *out, size_t cap);
bool relais_kx_on_confirm(relais_kx *kx, const void *msg, size_t len);

/* The derived session key, usable as the link key of relais_frame_encode().
 * Returns false until the exchange is complete. */
bool relais_kx_session_key(const relais_kx *kx, relais_key *out);
/* Wipe the PSK, the nonces and the session key. */
void relais_kx_clear(relais_kx *kx);

/* ------------------------------------------------------------------ rotation */
/* Ticks during which the receiver accepts both keys. Long enough to cover the
 * jitter of one period, short enough that a stolen old key stops working. */
#define RELAIS_KEYRING_WINDOW 8u

typedef struct {
  relais_key current;
  relais_key next;
  bool staged;     /* a next key is waiting for the end of the window */
  uint32_t window; /* ticks left before next becomes current */
  uint32_t rotations;
} relais_keyring;

void relais_keyring_init(relais_keyring *kr, const relais_key *current);
/* Stage the next key and open a transition window of `window` ticks. Returns
 * false when the key is missing or a rotation is already in flight. */
bool relais_keyring_stage(relais_keyring *kr, const relais_key *next, uint32_t window);
/* The key the sender signs with: always the current one. */
const relais_key *relais_keyring_tx(const relais_keyring *kr);
/* Verify a control. Returns 0 when the current key matches, 1 when the staged
 * next key matches inside the window, -1 when nothing matches. */
int relais_keyring_rx(const relais_keyring *kr, const void *frame, size_t len,
                      uint64_t got);
/* Decode a frame on a rotating link: the current key is tried, then the staged
 * one while the window is open, so a peer that already switched loses no tick.
 * On success *why is cleared; on failure it names the reason. */
bool relais_keyring_decode(const relais_keyring *kr, const uint8_t *buf, size_t len,
                           relais_msg *out, relais_reject *why);
/* Make the staged key current now (the peer already switched). */
bool relais_keyring_promote(relais_keyring *kr);
/* One tick of the link: closes the window and promotes the staged key. Both
 * ends call it once per tick, so they switch on the same tick. */
void relais_keyring_advance(relais_keyring *kr);
void relais_keyring_clear(relais_keyring *kr);

#endif /* RELAIS_KEYS_H */
