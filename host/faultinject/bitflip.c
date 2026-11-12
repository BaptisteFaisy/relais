/* Relais — bit flips: the implementation of bitflip.h.
 *
 * A fixed layout table gives, for each target type and zone, the byte range
 * that holds the named field. The flip then walks the same bounds-checked
 * path whatever the target is: one primitive, three public entry points.
 *
 * The wire layout used for the frame zones is the one documented in
 * core/src/frame.c: magic 4 | version 1 | type 1 | sender 1 | flags 1 | tick 8
 * | mandate 4 | payload_len 2 | reserved 2 | payload | check 8.
 */
#include "bitflip.h"

#include <stddef.h>
#include <string.h>

void fi_rng_seed(fi_rng *r, uint64_t seed) {
  if (r == NULL) return;
  r->s = seed != 0 ? seed : 0x9e3779b97f4a7c15ull;
}

uint64_t fi_rng_next(fi_rng *r) {
  if (r == NULL) return 0;
  uint64_t x = r->s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  r->s = x;
  return x * 0x2545f4914f6cdd1dull;
}

uint32_t fi_rng_below(fi_rng *r, uint32_t n) {
  if (r == NULL || n == 0u) return 0;
  return (uint32_t)(fi_rng_next(r) % n);
}

/* Pick the byte (and bit) inside [off, off+len) to flip. bit_opt < 0 means
 * "draw one from rng". */
static void fi_pick(size_t off, size_t len, int bit_opt, fi_rng *rng,
                    size_t *at, unsigned *bit) {
  size_t rel;
  if (bit_opt < 0) {
    uint32_t span = len > (size_t)UINT32_MAX ? UINT32_MAX : (uint32_t)len;
    rel = (size_t)fi_rng_below(rng, span);
    *bit = (unsigned)fi_rng_below(rng, 8u);
  } else {
    rel = ((size_t)bit_opt) / 8u;
    if (rel >= len) rel = len - 1u;
    *bit = ((unsigned)bit_opt) % 8u;
  }
  *at = off + rel;
}

/* The one primitive: invert bytes of base[] inside [off, off+len), grain from
 * mode, and report on the primary byte. size is the total writable object. */
static size_t fi_flip_bytes(uint8_t *base, size_t size, size_t off, size_t len,
                            fi_mode mode, int bit_opt, fi_rng *rng,
                            fi_report *rep) {
  if (base == NULL || size == 0 || len == 0 || off + len > size) return 0;
  size_t at;
  unsigned bit;
  fi_pick(off, len, bit_opt, rng, &at, &bit);

  size_t first = at;
  size_t count = 1u;
  uint8_t mask = (mode == FI_MODE_BIT) ? (uint8_t)(1u << bit) : 0xFFu;
  if (mode == FI_MODE_WORD) {
    first = at & ~((size_t)3u);            /* the 4-byte boundary holding `at` */
    if (first + 4u > off + len) first = (off + len >= 4u) ? off + len - 4u : off;
    if (first + 4u > size) first = size - 4u;
    count = 4u;
  }

  uint8_t before = base[first];
  for (size_t i = 0; i < count; i++) base[first + i] = (uint8_t)(base[first + i] ^ mask);

  if (rep != NULL) {
    rep->ok = true;
    rep->byte_off = first;
    rep->bit = (mode == FI_MODE_BIT) ? bit : 0u;
    rep->mask = mask;
    rep->before = before;
    rep->after = base[first];
  }
  return count;
}

/* ---- node state machine ------------------------------------------------- */

typedef struct { size_t off; size_t len; } fi_range;

static bool fi_node_range(fi_zone zone, fi_range *r) {
  switch (zone) {
    case FI_ZONE_ANY:
      r->off = 0; r->len = sizeof(relais_node_sm); return true;
    case FI_ZONE_MANDATE:
      r->off = offsetof(relais_node_sm, mandate); r->len = sizeof(relais_mandate); return true;
    case FI_ZONE_TICK:
      r->off = offsetof(relais_node_sm, now); r->len = sizeof(relais_tick); return true;
    case FI_ZONE_CTLSTATE:
      r->off = offsetof(relais_node_sm, ctl); r->len = sizeof(relais_ctl_state); return true;
    case FI_ZONE_VALUE:
      r->off = offsetof(relais_node_sm, ctl) + offsetof(relais_ctl_state, last_value);
      r->len = sizeof(float); return true;
    default: return false;
  }
}

size_t fi_flip_node(relais_node_sm *s, fi_zone zone, fi_mode mode,
                    int bit_opt, fi_rng *rng, fi_report *rep) {
  if (rep != NULL) memset(rep, 0, sizeof *rep);
  fi_range r;
  if (s == NULL || !fi_node_range(zone, &r)) return 0;
  size_t n = fi_flip_bytes((uint8_t *)s, sizeof *s, r.off, r.len, mode, bit_opt, rng, rep);
  if (rep != NULL) rep->zone = zone;
  return n;
}

/* ---- control state ------------------------------------------------------ */

static bool fi_ctl_range(fi_zone zone, fi_range *r) {
  switch (zone) {
    case FI_ZONE_ANY:
    case FI_ZONE_CTLSTATE:
      r->off = 0; r->len = sizeof(relais_ctl_state); return true;
    case FI_ZONE_VALUE:
      r->off = offsetof(relais_ctl_state, last_value); r->len = sizeof(float); return true;
    case FI_ZONE_TICK:
      r->off = offsetof(relais_ctl_state, ticks); r->len = sizeof(uint32_t); return true;
    default: return false; /* MANDATE has no region here */
  }
}

size_t fi_flip_ctl(relais_ctl_state *c, fi_zone zone, fi_mode mode,
                   int bit_opt, fi_rng *rng, fi_report *rep) {
  if (rep != NULL) memset(rep, 0, sizeof *rep);
  fi_range r;
  if (c == NULL || !fi_ctl_range(zone, &r)) return 0;
  size_t n = fi_flip_bytes((uint8_t *)c, sizeof *c, r.off, r.len, mode, bit_opt, rng, rep);
  if (rep != NULL) rep->zone = zone;
  return n;
}

/* ---- encoded frame ------------------------------------------------------ */

static bool fi_frame_range(const uint8_t *buf, size_t len, fi_zone zone, fi_range *r) {
  if (buf == NULL || len < RELAIS_HEADER_LEN + RELAIS_CHECK_LEN) return false;
  uint8_t type = buf[5];
  uint16_t payload_len = (uint16_t)(((uint16_t)buf[20] << 8) | buf[21]);
  size_t body = len - RELAIS_CHECK_LEN;
  if ((size_t)payload_len + RELAIS_HEADER_LEN + RELAIS_CHECK_LEN != len) return false;

  switch (zone) {
    case FI_ZONE_MANDATE: r->off = 16; r->len = 4; return true;
    case FI_ZONE_TICK:    r->off = 8;  r->len = 8; return true;
    case FI_ZONE_ANY:     r->off = 0;  r->len = body; return true;
    case FI_ZONE_VALUE:
      switch (type) {
        case RELAIS_MSG_ORDRE:   r->off = 24; r->len = 4; return true;
        case RELAIS_MSG_OMBRE:   r->off = 24; r->len = 4; return true;
        case RELAIS_MSG_DEMANDE: r->off = 30; r->len = 4; return true;
        case RELAIS_MSG_STATUT:  r->off = 40; r->len = 4; return true; /* applied */
        default: return false;
      }
    case FI_ZONE_CTLSTATE:
      if (type == RELAIS_MSG_ETAT && payload_len >= 5u) {
        r->off = RELAIS_HEADER_LEN + 5u;
        r->len = payload_len - 5u;
        return true;
      }
      return false;
    default: return false;
  }
}

size_t fi_flip_frame(uint8_t *buf, size_t len, fi_zone zone, fi_mode mode,
                     int bit_opt, fi_rng *rng, fi_report *rep) {
  if (rep != NULL) memset(rep, 0, sizeof *rep);
  fi_range r;
  if (!fi_frame_range(buf, len, zone, &r)) return 0;
  size_t n = fi_flip_bytes(buf, len, r.off, r.len, mode, bit_opt, rng, rep);
  if (rep != NULL) rep->zone = zone;
  return n;
}