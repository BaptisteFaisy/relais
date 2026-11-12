/* Relais — durable persistence of the actuator's mandate state (MAN-11).
 *
 * A small POSIX layer on top of the pure core snapshot API: it writes the
 * 16-byte `relais_arb_snapshot` result to two copies with a CRC-32C checksum
 * each, atomically (temporary file + fsync + rename), and reads back through
 * `relais_arb_restore`. It is wear-limited: a save whose payload did not
 * change since the previous one is a no-op, so the actuator can call it every
 * tick and the layer only touches the storage when the mandate holder or the
 * certification actually changed.
 *
 * On load, a corrupt primary copy is detected by its checksum and the healthy
 * secondary copy is used instead (`relais_persist.recovered` reports it). Two
 * copies cannot both be corrupt as long as the two writes were separated by
 * at least one successful fsync, because each copy is a complete record or
 * absent — never a half-written one (rename is atomic on POSIX).
 *
 * Requirements covered: MAN-11 (mandate survives a restart), SEC-07 (state
 * tampering is detected by the checksum and reported, not silently trusted).
 *
 * This file is portable C11 + POSIX: the very same code runs on the PC (tests)
 * and on QNX (target/actuator_qnx.c), no QNX-only symbol.
 */
#ifndef RELAIS_PERSIST_H
#define RELAIS_PERSIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "relais/arbiter.h"

/* On-disk record: magic(4, LE) | version(1) | flags(1) | payload_len(2, LE)
 * | payload[payload_len] | crc32c(4, LE). The checksum covers everything
 * before it, so a truncated or tampered length field is caught too. */
#define RELAIS_PERSIST_MAGIC   0x31535052u /* "RPS1" */
#define RELAIS_PERSIST_VERSION 1u
#define RELAIS_PERSIST_HDR_LEN 12u /* magic+version+flags+len+crc */
#define RELAIS_PERSIST_MAX     256u

typedef struct {
  char base[512];    /* primary copy */
  char backup[512];  /* secondary copy */
  uint8_t last[RELAIS_PERSIST_MAX]; /* last written payload, for wear limiting */
  size_t last_len;
  bool have_last;
  bool recovered;    /* the last load fell back to the secondary copy */
} relais_persist;

/* Fill base/backup paths from `path` (and `path.bak`). Returns 0 on success,
 * -1 when `path` is NULL or does not fit. */
int relais_persist_init(relais_persist *p, const char *path);

/* Atomically persist `data[0..len)` to both copies. Wear-limited: returns 1
 * (and writes nothing) when the payload equals the previous one. Returns 0 on
 * success, -1 when the primary write failed (the secondary stays best-effort). */
int relais_persist_save(relais_persist *p, const uint8_t *data, size_t len);

/* Load the payload. Returns 0 with the payload in `data[0..*len)`, 1 when
 * neither copy exists (fresh start), -1 when both copies are corrupt or
 * unreadable. Sets `p->recovered` when the secondary copy was used. */
int relais_persist_load(relais_persist *p, uint8_t *data, size_t cap, size_t *len);

/* Convenience wrappers around the snapshot API (MAN-11). */
int relais_persist_save_arb(relais_persist *p, const relais_arb *arb);
int relais_persist_load_arb(relais_persist *p, relais_arb *arb);

#endif /* RELAIS_PERSIST_H */