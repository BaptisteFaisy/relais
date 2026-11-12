/* Relais — frame integrity: CRC-32C at level S, keyed MAC at level C.
 * Both are pure and allocation free.
 */
#ifndef RELAIS_CHECK_H
#define RELAIS_CHECK_H

#include "relais/types.h"

/* Per-link key. When enabled is false, frames carry a CRC-32C instead of a MAC. */
typedef struct {
  uint8_t bytes[16];
  bool enabled;
} relais_key;

void relais_key_clear(relais_key *k);
bool relais_key_from_hex(relais_key *k, const char *hex);

/* Hexadecimal form of a byte string, and back. One implementation, used by the
 * link keys (SEC-02), the session keys (SEC-07) and the journal chain. */
int relais_hex_encode(const uint8_t *in, size_t len, char *out, size_t cap);
bool relais_hex_decode(const char *hex, uint8_t *out, size_t len);

/* Little-endian 64-bit access: the byte order SipHash and the journal
 * fingerprints use. The frames themselves stay big-endian (frame.h). */
void relais_put_u64le(uint8_t *out, uint64_t v);
uint64_t relais_get_u64le(const uint8_t *p);

uint32_t relais_crc32c(const void *data, size_t len);
uint32_t relais_crc32c_update(uint32_t crc, const void *data, size_t len);

/* SipHash-2-4, truncated to 64 bits: authenticator of a frame (SEC-02). */
uint64_t relais_mac(const relais_key *k, const void *data, size_t len);

/* The same primitive fed in several chunks (SEC-07, journal chain): the caller
 * hashes a prefix and a body without copying them into one buffer. The result
 * of init/update(...)/final is bit for bit the one of relais_mac() over the
 * concatenation, so the published SipHash vectors cover both forms. */
typedef struct {
  uint64_t v[4];
  uint8_t tail[8];
  uint8_t ntail;
  uint64_t total;
} relais_siphash;

void relais_siphash_init(relais_siphash *h, const relais_key *k);
void relais_siphash_update(relais_siphash *h, const void *data, size_t len);
uint64_t relais_siphash_final(const relais_siphash *h);

/* The 8-byte trailer of a frame, according to the key. */
uint64_t relais_check_compute(const relais_key *k, const void *frame, size_t len);
bool relais_check_verify(const relais_key *k, const void *frame, size_t len, uint64_t got);

#endif /* RELAIS_CHECK_H */
