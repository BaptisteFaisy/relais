/* Relais — fine hardware fault injection: bit flips in memory and in flight.
 *
 * A "fine hardware fault" is a single bit inverted in a stored value.
 * This module flips bits with three grains -- one bit, one byte, one 32-bit
 * word -- in the state of a machine (relais_node_sm, relais_ctl_state) and in
 * an already encoded frame, either at a chosen bit or at a random bit drawn
 * from a seeded stream, so a run is reproducible (INJ-06).
 *
 * The flip never recomputes the frame trailer: a flipped frame fails its own
 * CRC/MAC. Recomputation is the *corrupt* module's job (the component that
 * lies rather than the noisy channel), so the two failure models stay apart.
 *
 * Requirement ids: ROB-01, INJ-06, SEC-01, SEC-02, SEC-04, DET-04, DET-09, REP-01.
 */
#ifndef RELAIS_FAULTINJECT_BITFLIP_H
#define RELAIS_FAULTINJECT_BITFLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "relais/control.h"
#include "relais/frame.h"
#include "relais/node.h"

/* A zone the flip may target. */
typedef enum {
  FI_ZONE_ANY = 0,     /* any writable byte of the target */
  FI_ZONE_MANDATE,     /* the raw mandate number */
  FI_ZONE_TICK,        /* the targeted tick */
  FI_ZONE_CTLSTATE,    /* the replicated control state */
  FI_ZONE_VALUE        /* the computed / applied command value */
} fi_zone;

/* Grain of the fault: a bit, a byte, or a 32-bit word. */
typedef enum {
  FI_MODE_BIT = 0,
  FI_MODE_BYTE,
  FI_MODE_WORD
} fi_mode;

/* Deterministic PRNG (xorshift64*), same algorithm as the simulator stream. */
typedef struct { uint64_t s; } fi_rng;
void fi_rng_seed(fi_rng *r, uint64_t seed);
uint64_t fi_rng_next(fi_rng *r);
uint32_t fi_rng_below(fi_rng *r, uint32_t n);

/* Random bit selector: ask the flip to draw its own. */
#define FI_RANDOM (-1)

/* What one injection actually did. */
typedef struct {
  bool ok;           /* an injection happened */
  fi_zone zone;      /* the zone requested */
  size_t byte_off;   /* primary byte offset within the target */
  unsigned bit;      /* bit 0..7 inside that byte (FI_MODE_BIT only) */
  uint8_t mask;      /* bits actually inverted on the primary byte */
  uint8_t before;    /* primary byte value before the flip */
  uint8_t after;     /* primary byte value after the flip */
} fi_report;

/* Flip one grain in a node state machine. bit_opt is a bit offset inside the
 * chosen zone field, or FI_RANDOM. Returns the number of bytes inverted, or 0
 * when the zone has no region in this target. */
size_t fi_flip_node(relais_node_sm *s, fi_zone zone, fi_mode mode,
                    int bit_opt, fi_rng *rng, fi_report *rep);

/* Flip one grain in a control state (the replicated state of a machine). */
size_t fi_flip_ctl(relais_ctl_state *c, fi_zone zone, fi_mode mode,
                   int bit_opt, fi_rng *rng, fi_report *rep);

/* Flip one grain in an already encoded frame. The trailer is left untouched,
 * so the flip is unmasked by the CRC/MAC as soon as the frame is decoded. */
size_t fi_flip_frame(uint8_t *buf, size_t len, fi_zone zone, fi_mode mode,
                     int bit_opt, fi_rng *rng, fi_report *rep);

#endif /* RELAIS_FAULTINJECT_BITFLIP_H */