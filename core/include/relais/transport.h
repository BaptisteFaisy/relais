/* Relais — transport interface (mission M32, idea 75).
 *
 * The core is a pure library: no clock, no file, no socket, no allocation. The
 * wire it speaks has always been UDP, but nothing in core/ says so: this header
 * is the ONLY place where a transport is named, and it names none. A transport
 * is a vtable of four functions over caller-provided memory.
 *
 * Proven independence: the implementations live outside the core
 * (host/transport/udp.c, host/transport/can.c) and the protocol state machines
 * (arbiter.c, node.c) are compiled and tested unchanged against a memory
 * transport (core/tests/test_transport.c). Requirement IDs: SEC-05 (only the
 * net layer owns a socket), DET-04 (a malformed frame never reaches the core),
 * ROB-01 (a transport failure is reported, never guessed), CMD-04.
 *
 * A transport moves OPAQUE, already-encoded frames. Authentication, CRC and the
 * frame shape stay in core/src/frame.c, so a new transport cannot weaken them.
 */
#ifndef RELAIS_TRANSPORT_H
#define RELAIS_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "relais/frame.h"
#include "relais/types.h"

/* Which of the two links a frame travels on. Spec 4.9 gives one rule, already
 * implemented by relais_msg_to_peer(): ETAT and SUIVI go to the other computer,
 * everything else goes to the actuator. The two links are indexed, not named
 * after a peer, because the actuator end uses the same pair: its link 0 goes to
 * computer A and its link 1 to computer B, and a STATUT is sent on both.
 * A transport with a single medium (a CAN bus, a shared-memory ring) maps both
 * links onto it and keeps the distinction only through the CAN identifier or
 * the port number. */
typedef enum {
  RELAIS_LINK_ACTUATOR = 0, /* a computer: towards the actuator */
  RELAIS_LINK_PEER = 1,     /* a computer: towards the other computer */
  RELAIS_LINK_COUNT = 2
} relais_link;

/* Transport status. Negative means "did not happen"; the caller decides what it
 * means for the protocol (a timeout is the ordinary silence of MAN-09, an I/O
 * error is a link loss). No transport invents a retry: ROB-01. */
enum {
  RELAIS_TR_OK = 0,        /* frame sent / received */
  RELAIS_TR_TIMEOUT = -1,  /* nothing arrived within the configured timeout */
  RELAIS_TR_ERR_CONFIG = -2, /* bad endpoint spec, unusable vtable */
  RELAIS_TR_ERR_IO = -3,   /* the medium refused the frame */
  RELAIS_TR_ERR_FULL = -4, /* buffer or queue too small for this frame */
  RELAIS_TR_CLOSED = -5    /* used after close, or never initialised */
};

/* Endpoint description, transport neutral. Every field is a string or a number:
 * an implementation parses what it needs and ignores the rest, so the same
 * structure configures "127.0.0.1:7200" (UDP), "vcan0:0x120" (CAN) or
 * "ring0" (memory). NULL means "transport default". */
typedef struct {
  const char *actuator;  /* where the actuator is reached (link ACTUATOR) */
  const char *peer;      /* where the other computer is reached (link PEER) */
  const char *local;     /* where to bind / what to answer to; NULL = any */
  uint32_t timeout_ms;   /* recv budget; 0 = do not block */
  uint32_t mtu;          /* largest frame the medium carries whole; 0 = default */
} relais_tr_cfg;

/* The vtable. `ctx` is owned by the caller and by the implementation: the core
 * never allocates it, never frees it and never looks inside. */
typedef struct relais_transport {
  const char *name;  /* "udp", "can", "memory": for the journal and the logs */
  void *ctx;
  int (*init)(void *ctx, const relais_tr_cfg *cfg);
  int (*send)(void *ctx, relais_link link, const uint8_t *frame, size_t len);
  /* On RELAIS_TR_OK, *len holds the frame size and *link the link it came in
   * on. buf must be at least RELAIS_FRAME_MAX bytes for a Relais frame. */
  int (*recv)(void *ctx, relais_link *link, uint8_t *buf, size_t cap, size_t *len);
  void (*close)(void *ctx);
} relais_transport;

/* Link a message type travels on: one rule, used by every caller (spec 4.9). */
static inline relais_link relais_link_of(relais_msg_type type) {
  return relais_msg_to_peer(type) ? RELAIS_LINK_PEER : RELAIS_LINK_ACTUATOR;
}

/* A usable transport: the four verbs and a name. Cheap, called once at start. */
static inline bool relais_tr_valid(const relais_transport *t) {
  return t != NULL && t->init != NULL && t->send != NULL && t->recv != NULL &&
         t->close != NULL && t->name != NULL;
}

static inline int relais_tr_init(relais_transport *t, const relais_tr_cfg *cfg) {
  if (!relais_tr_valid(t)) return RELAIS_TR_ERR_CONFIG;
  return t->init(t->ctx, cfg);
}

static inline int relais_tr_send_raw(relais_transport *t, relais_link link,
                                     const uint8_t *frame, size_t len) {
  if (!relais_tr_valid(t) || frame == NULL || len == 0) return RELAIS_TR_ERR_CONFIG;
  if ((unsigned)link >= (unsigned)RELAIS_LINK_COUNT) return RELAIS_TR_ERR_CONFIG;
  return t->send(t->ctx, link, frame, len);
}

static inline int relais_tr_recv_raw(relais_transport *t, relais_link *link,
                                     uint8_t *buf, size_t cap, size_t *len) {
  if (len != NULL) *len = 0; /* always defined on return, even on refusal */
  if (!relais_tr_valid(t) || buf == NULL || len == NULL) return RELAIS_TR_ERR_CONFIG;
  return t->recv(t->ctx, link, buf, cap, len);
}

static inline void relais_tr_close(relais_transport *t) {
  if (relais_tr_valid(t)) t->close(t->ctx);
}

/* Encode then send. The only place where a message meets a medium, so the two
 * real transports and the fake one used by the tests share one implementation
 * (no duplication). Returns RELAIS_TR_OK, or a negative status. */
static inline int relais_tr_send_msg(relais_transport *t, const relais_key *key,
                                     const relais_msg *m, uint8_t *scratch,
                                     size_t cap) {
  if (m == NULL || scratch == NULL || cap < RELAIS_FRAME_MAX)
    return RELAIS_TR_ERR_FULL;
  size_t len = relais_frame_encode(m, key, scratch, cap);
  if (len == 0) return RELAIS_TR_ERR_FULL; /* does not fit: never sent partly */
  return relais_tr_send_raw(t, relais_link_of(m->type), scratch, len);
}

/* Receive then decode and authenticate. A frame that fails the check is reported
 * through *why and never handed to the protocol (DET-04, SEC-01). */
static inline int relais_tr_recv_msg(relais_transport *t, const relais_key *key,
                                     relais_link *link, relais_msg *out,
                                     relais_reject *why, uint8_t *buf, size_t cap) {
  size_t len = 0;
  int rc = relais_tr_recv_raw(t, link, buf, cap, &len);
  if (rc != RELAIS_TR_OK) return rc;
  if (!relais_frame_decode(buf, len, key, out, why)) return RELAIS_TR_ERR_IO;
  return RELAIS_TR_OK;
}

#endif /* RELAIS_TRANSPORT_H */
