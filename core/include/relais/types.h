/* Relais — base types shared by the core library, the actuator, the QNX
 * processes, the exhaustive explorer and the seeded simulator.
 *
 * The core is portable C11: no clock, no file, no socket, no allocation.
 * Everything lives in caller-provided memory.
 *
 * Spec: docs/SPEC.md sections 4.2 (tick), 4.3 (mandate), 6 (frames).
 */
#ifndef RELAIS_TYPES_H
#define RELAIS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RELAIS_VERSION        1u
#define RELAIS_MAGIC          0x31534c52u /* "RLS1" as sent on the wire */
#define RELAIS_FRAME_MAX      512u
#define RELAIS_HEADER_LEN     24u
#define RELAIS_CHECK_LEN      8u
#define RELAIS_PAYLOAD_MAX    (RELAIS_FRAME_MAX - RELAIS_HEADER_LEN - RELAIS_CHECK_LEN)
#define RELAIS_TESTAMENT_MAX  8u   /* N, horizon of the will embedded in an order */
#define RELAIS_CTL_STATE_MAX  256u /* REP-01: a full state fits in 256 bytes */
#define RELAIS_EVENTS_MAX     8u   /* journal events produced by one tick */
#define RELAIS_IN_MSG_MAX     8u   /* frames handed to one arbiter step */
#define RELAIS_OUT_MSG_MAX    4u   /* frames emitted by one node step */
#define RELAIS_FULL_SCALE     1.0f /* command full scale, in normalised units */
#define RELAIS_TICK_NONE      UINT64_MAX
#define RELAIS_REJECT_SLOTS   12u

typedef uint64_t relais_tick;    /* tick number, given by the actuator */
typedef uint32_t relais_mandate; /* mandate number, handed out by the actuator */

/* Who is speaking. The actuator is a node too: it sends STATUT. */
typedef enum {
  RELAIS_NODE_NONE = 0,
  RELAIS_NODE_A = 1,
  RELAIS_NODE_B = 2,
  RELAIS_NODE_C = 3,
  RELAIS_NODE_ACTUATOR = 4
} relais_node;

/* Frame types, spec section 6.2. */
typedef enum {
  RELAIS_MSG_STATUT = 1,   /* actuator -> A and B, every tick */
  RELAIS_MSG_ORDRE = 2,    /* holder -> actuator, every tick */
  RELAIS_MSG_OMBRE = 3,    /* second -> actuator, every tick (level C) */
  RELAIS_MSG_DEMANDE = 4,  /* non-holder -> actuator, on suspicion */
  RELAIS_MSG_CESSION = 5,  /* holder -> actuator and second */
  RELAIS_MSG_ETAT = 6,     /* holder -> second, full control state */
  RELAIS_MSG_SUIVI = 7     /* second -> holder, replica age and readiness */
} relais_msg_type;

/* Where the value applied at a tick came from (spec 4.4). */
typedef enum {
  RELAIS_SRC_NONE = 0,
  RELAIS_SRC_HOLDER = 1,    /* a fresh order from the mandate holder */
  RELAIS_SRC_SHADOW = 2,    /* substitution: shadow of the certified second */
  RELAIS_SRC_TESTAMENT = 3, /* the will of the last order received */
  RELAIS_SRC_FALLBACK = 4   /* local damped hold inside the actuator */
} relais_source;

/* Warning lamp, DEG-01. */
typedef enum {
  RELAIS_LAMP_OFF = 0,    /* full redundancy */
  RELAIS_LAMP_YELLOW = 1, /* redundancy lost, command still fresh */
  RELAIS_LAMP_RED = 2     /* fallback law, or disagreement */
} relais_lamp;

typedef enum {
  RELAIS_CAUSE_NONE = 0,
  RELAIS_CAUSE_NODE_MISSING = 1,
  RELAIS_CAUSE_LINK_LOST = 2,
  RELAIS_CAUSE_SECOND_UNCERTIFIED = 3,
  RELAIS_CAUSE_DISAGREEMENT = 4,
  RELAIS_CAUSE_FALLBACK = 5,
  RELAIS_CAUSE_STOP_REQUESTED = 6
} relais_cause;

/* Role of a computer, as it knows it. */
typedef enum {
  RELAIS_ROLE_NONE = 0,   /* booting, not yet a second */
  RELAIS_ROLE_SECOND = 1,
  RELAIS_ROLE_HOLDER = 2
} relais_role;

/* Why a frame was dropped. Counted and journaled (MAN-02, DET-04, SEC-01). */
typedef enum {
  RELAIS_REJ_NONE = 0,
  RELAIS_REJ_NOT_HOLDER = 1,
  RELAIS_REJ_MANDATE_STALE = 2,
  RELAIS_REJ_MANDATE_FUTURE = 3,
  RELAIS_REJ_TICK_MISSED = 4,
  RELAIS_REJ_CHECK = 5,
  RELAIS_REJ_MALFORMED = 6,
  RELAIS_REJ_RATE_LIMITED = 7,
  RELAIS_REJ_NOT_CERTIFIED = 8,
  RELAIS_REJ_VERSION = 9,
  RELAIS_REJ_REVOKED = 10,     /* M20: the sender is on the revocation list */
  RELAIS_REJ_AUTH_BANNED = 11  /* M20: the sender failed the authenticator too
                                  many times in a row (DEG-09) */
} relais_reject;

/* Journal events, spec 6.4. */
typedef enum {
  RELAIS_EV_NONE = 0,
  RELAIS_EV_MANDATE_GRANTED = 1,
  RELAIS_EV_MANDATE_REVOKED = 2,
  RELAIS_EV_MANDATE_AUTO_SWITCH = 3,
  RELAIS_EV_REQUEST_DENIED = 4,
  RELAIS_EV_CERTIFIED = 5,
  RELAIS_EV_DECERTIFIED = 6,
  RELAIS_EV_ORDER_REJECTED = 7,
  RELAIS_EV_DISAGREEMENT = 8,
  RELAIS_EV_FALLBACK_ENTER = 9,
  RELAIS_EV_FALLBACK_EXIT = 10,
  RELAIS_EV_CESSION = 11,
  RELAIS_EV_LAMP_CHANGE = 12,
  RELAIS_EV_STOP_REQUESTED = 13,
  RELAIS_EV_FORGED = 14,
  RELAIS_EV_NODE_BACK = 15,
  RELAIS_EV_NODE_REVOKED = 16, /* M20: a node was put on the revocation list */
  RELAIS_EV_AUTH_BANNED = 17   /* M20: a node was banned after auth strikes */
} relais_event_kind;

typedef struct {
  relais_event_kind kind;
  relais_node node;
  relais_mandate mandate;
  relais_tick tick;
  uint32_t detail; /* reject reason, lamp value, streak length, ... */
} relais_event;

/* Measurements carried by STATUT and used by the control law. */
typedef struct {
  float pos;      /* rack position, normalised */
  float vel;      /* rack velocity */
  float applied;  /* value applied by the actuator at the previous tick */
  float setpoint; /* steering wheel request for the next tick */
} relais_meas;

const char *relais_msg_type_name(relais_msg_type t);
const char *relais_source_name(relais_source s);
const char *relais_reject_name(relais_reject r);
const char *relais_event_name(relais_event_kind k);
const char *relais_node_name(relais_node n);
const char *relais_lamp_name(relais_lamp l);
const char *relais_cause_name(relais_cause c);
bool relais_node_is_computer(relais_node n);

#endif /* RELAIS_TYPES_H */
