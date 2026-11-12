/* Relais — durable persistence of the actuator's mandate state (MAN-11).
 *
 * One implementation used by the PC tests and by the QNX actuator. All the
 * protocol knowledge stays in core/; this file only moves bytes to and from a
 * POSIX filesystem with the durability and the corruption detection the safety
 * argument needs.
 *
 * Record layout (see persist.h): a CRC-32C over the header and payload, so a
 * torn write, a bit flip or a truncation is detected before any restore.
 */
#include "persist.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "relais/check.h"

static void put_le32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xffu);
}

static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* Build one complete record into rec (>= 12 + RELAIS_PERSIST_MAX bytes).
 * Returns the total record size. */
static size_t persist_pack(uint8_t *rec, const uint8_t *payload, size_t len) {
  put_le32(rec, RELAIS_PERSIST_MAGIC);
  rec[4] = RELAIS_PERSIST_VERSION;
  rec[5] = 0; /* flags, reserved */
  rec[6] = (uint8_t)(len & 0xffu);
  rec[7] = (uint8_t)((len >> 8) & 0xffu);
  if (len > 0) memcpy(rec + RELAIS_PERSIST_HDR_LEN - 4, payload, len);
  uint32_t crc = relais_crc32c(rec, 8 + len);
  put_le32(rec + 8 + len, crc);
  return 12 + len;
}

/* Atomic single-file write: temporary file, fsync, rename (POSIX rename is
 * atomic on the same filesystem). A crash can leave the `.tmp` file behind,
 * never a half-written primary. */
static int persist_write_file(const char *path, const uint8_t *payload, size_t len) {
  if (path == NULL || payload == NULL || len > RELAIS_PERSIST_MAX) return -1;
  uint8_t rec[RELAIS_PERSIST_HDR_LEN + RELAIS_PERSIST_MAX];
  size_t total = persist_pack(rec, payload, len);
  char tmp[600];
  if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return -1;
  int ok = (write(fd, rec, total) == (ssize_t)total) && (fsync(fd) == 0);
  if (close(fd) != 0) ok = 0;
  if (!ok) {
    unlink(tmp);
    return -1;
  }
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

/* Read and verify one record. Returns 0 ok (payload + *len), 1 missing,
 * 2 corrupt, -1 unreadable. */
static int persist_read_file(const char *path, uint8_t *payload, size_t cap,
                             size_t *len) {
  uint8_t rec[RELAIS_PERSIST_HDR_LEN + RELAIS_PERSIST_MAX];
  int fd = open(path, O_RDONLY);
  if (fd < 0) return (errno == ENOENT) ? 1 : -1;
  ssize_t got = read(fd, rec, sizeof rec);
  close(fd);
  if (got < 0) return -1;
  if ((size_t)got < RELAIS_PERSIST_HDR_LEN) return 2;
  if (get_le32(rec) != RELAIS_PERSIST_MAGIC || rec[4] != RELAIS_PERSIST_VERSION ||
      rec[5] != 0)
    return 2;
  size_t plen = (size_t)rec[6] | ((size_t)rec[7] << 8);
  if (plen > RELAIS_PERSIST_MAX || (size_t)got != 12 + plen) return 2;
  uint32_t want = get_le32(rec + 8 + plen);
  if (relais_crc32c(rec, 8 + plen) != want) return 2;
  if (cap < plen) return -1;
  if (payload != NULL && plen > 0) memcpy(payload, rec + 8, plen);
  if (len != NULL) *len = plen;
  return 0;
}

int relais_persist_init(relais_persist *p, const char *path) {
  if (p == NULL || path == NULL) return -1;
  memset(p, 0, sizeof *p);
  if (snprintf(p->base, sizeof p->base, "%s", path) >= (int)sizeof p->base) return -1;
  if (snprintf(p->backup, sizeof p->backup, "%s.bak", path) >=
      (int)sizeof p->backup)
    return -1;
  return 0;
}

int relais_persist_save(relais_persist *p, const uint8_t *data, size_t len) {
  if (p == NULL || data == NULL || len == 0 || len > RELAIS_PERSIST_MAX) return -1;
  if (p->have_last && p->last_len == len && memcmp(p->last, data, len) == 0)
    return 1; /* wear-limited: nothing changed */
  if (persist_write_file(p->base, data, len) != 0) return -1;
  (void)persist_write_file(p->backup, data, len); /* best-effort second copy */
  memcpy(p->last, data, len);
  p->last_len = len;
  p->have_last = true;
  return 0;
}

int relais_persist_load(relais_persist *p, uint8_t *data, size_t cap, size_t *len) {
  if (p == NULL || data == NULL) return -1;
  p->recovered = false;
  int r = persist_read_file(p->base, data, cap, len);
  if (r == 0) return 0; /* primary healthy */
  /* Primary missing or corrupt (SEC-07): fall back to the secondary copy. */
  int rb = persist_read_file(p->backup, data, cap, len);
  if (rb == 0) {
    p->recovered = true;
    return 0;
  }
  if (r == 1 && rb == 1) return 1; /* neither copy exists: fresh start */
  return -1; /* at least one copy is present but corrupt or unreadable */
}

/* MAN-11: the mandate number and its holder are the only truth the persistence
 * carries. relais_arb_snapshot already bounds and formats them. */
int relais_persist_save_arb(relais_persist *p, const relais_arb *arb) {
  uint8_t buf[RELAIS_PERSIST_MAX];
  size_t len = relais_arb_snapshot(arb, buf, sizeof buf);
  if (len == 0) return -1;
  return relais_persist_save(p, buf, len);
}

int relais_persist_load_arb(relais_persist *p, relais_arb *arb) {
  uint8_t buf[RELAIS_PERSIST_MAX];
  size_t len = 0;
  int r = relais_persist_load(p, buf, sizeof buf, &len);
  if (r != 0) return r;
  if (!relais_arb_restore(arb, buf, len)) return -1;
  return 0;
}