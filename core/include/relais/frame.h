/* Relais — wire format (spec 6.1 and 6.2).
 *
 *   magic 4 | version 1 | type 1 | sender 1 | flags 1 | tick 8 | mandate 4
 *   payload_len 2 | reserved 2 | payload | check 8          (<= 512 bytes)
 *
 * Every field is written big-endian, byte by byte: no aliasing, no structure
 * padding, identical on PC and on QNX. One decoded message fits in a single
 * relais_msg; only the fields of its type are meaningful.
 */
#ifndef RELAIS_FRAME_H
#define RELAIS_FRAME_H

#include "relais/check.h"
#include "relais/types.h"

#define RELAIS_FLAG_READY     0x01u /* the sender is a certified second */
#define RELAIS_FLAG_CESSION   0x02u
#define RELAIS_FLAG_TEST      0x04u /* bench frame, never applied */
#define RELAIS_FLAG_HAS_ORDER 0x08u /* a DEMANDE carries its first order */

typedef struct {
  relais_msg_type type;
  relais_node sender;
  uint8_t flags;
  relais_tick tick;      /* tick emitted (STATUT) or tick targeted (orders) */
  relais_mandate mandate;

  /* STATUT */
  relais_node holder;
  relais_source source;
  uint8_t misses;
  bool certified;
  relais_node certified_node;
  relais_lamp lamp;
  relais_cause cause;
  bool stop_requested;
  relais_meas meas;

  /* ORDRE, OMBRE, DEMANDE with an embedded order */
  float value;
  float testament[RELAIS_TESTAMENT_MAX];
  uint8_t testament_len;
  uint8_t replica_age; /* REP-03 */
  bool has_order;

  /* DEMANDE, CESSION */
  relais_mandate seen_mandate;
  uint8_t reason;

  /* ETAT */
  uint8_t ctl_state[RELAIS_CTL_STATE_MAX];
  uint8_t ctl_state_len;
  float last_value;

  /* SUIVI */
  bool ready;
} relais_msg;

void relais_msg_clear(relais_msg *m);

/* Exact on-wire size of a message, payload included. */
size_t relais_frame_encoded_size(const relais_msg *m);
/* Encode; returns the size written, or 0 when the message does not fit. */
size_t relais_frame_encode(const relais_msg *m, const relais_key *k,
                           uint8_t *buf, size_t cap);
/* Decode and verify. On failure returns false and sets *why (may be NULL). */
bool relais_frame_decode(const uint8_t *buf, size_t len, const relais_key *k,
                         relais_msg *out, relais_reject *why);
/* Same, without checking the trailer: for fuzzing the parser (ROB-09). */
bool relais_frame_decode_unchecked(const uint8_t *buf, size_t len, relais_msg *out);

/* Small helpers used by the bench and the tests. */
size_t relais_frame_write_u64(uint8_t *p, uint64_t v);
size_t relais_frame_write_u32(uint8_t *p, uint32_t v);
size_t relais_frame_write_f32(uint8_t *p, float v);
uint64_t relais_frame_read_u64(const uint8_t *p);
uint32_t relais_frame_read_u32(const uint8_t *p);
float relais_frame_read_f32(const uint8_t *p);

#endif /* RELAIS_FRAME_H */
