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

uint32_t relais_crc32c(const void *data, size_t len);
uint32_t relais_crc32c_update(uint32_t crc, const void *data, size_t len);

/* SipHash-2-4, truncated to 64 bits: authenticator of a frame (SEC-02). */
uint64_t relais_mac(const relais_key *k, const void *data, size_t len);

/* The 8-byte trailer of a frame, according to the key. */
uint64_t relais_check_compute(const relais_key *k, const void *frame, size_t len);
bool relais_check_verify(const relais_key *k, const void *frame, size_t len, uint64_t got);

#endif /* RELAIS_CHECK_H */
