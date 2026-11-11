/* Relais — a computer's protocol state machine (spec 4.3, 4.6, 4.9, 5.2, 5.5).
 *
 * The node never invents a mandate: it reads the one the actuator publishes.
 * An order names the tick it targets, so a machine that wakes up from a freeze
 * produces a stale order that the actuator drops (spec 4.6, I3).
 *
 * Everything the node wants to send is returned in relais_node_out; the caller
 * (relais-net on QNX, the simulator on PC) owns the sockets.
 */
#ifndef RELAIS_NODE_H
#define RELAIS_NODE_H

#include "relais/control.h"
#include "relais/frame.h"
#include "relais/mutant.h"
#include "relais/params.h"

/* Death points of spec 7.5, for the fault campaign (INJ-07). */
typedef enum {
  RELAIS_DP_NONE = 0,
  RELAIS_DP_P1 = 1, /* STATUT received, before computing */
  RELAIS_DP_P2 = 2, /* computed, before any send */
  RELAIS_DP_P3 = 3, /* ORDRE sent, ETAT not yet */
  RELAIS_DP_P4 = 4, /* ETAT sent on one link only */
  RELAIS_DP_P5 = 5, /* CESSION sent to the actuator only, or the second only */
  RELAIS_DP_P6 = 6, /* suspicion raised, DEMANDE not sent */
  RELAIS_DP_P7 = 7, /* mandate received, first ORDRE not sent */
  RELAIS_DP_P8 = 8, /* deposed node, while it becomes a second again */
  RELAIS_DP_P9 = 9  /* reintegration, before certification */
} relais_death_point;

typedef struct {
  relais_params params;
  relais_node self;
  relais_role role;
  relais_mandate mandate;      /* the mandate the actuator gave us */
  relais_mandate seen_mandate; /* last mandate read in a STATUT */
  relais_node seen_holder;

  relais_ctl_state ctl;
  relais_meas last_meas;
  relais_tick last_statut;     /* last tick we heard from the actuator */
  relais_tick last_etat;       /* last tick we got a state from the holder */
  relais_tick now;             /* tick of the last event processed */
  uint8_t replica_age;         /* ticks since the last accepted ETAT */
  uint8_t bail_ticks;          /* silence counter, MAN-09 */
  bool ready;                  /* we consider ourselves a certified second */
  bool link_lost;              /* no ETAT and no STATUT from the peer */
  uint8_t link_lost_ticks;
  bool deposed;                /* we just learned we lost the mandate */
  bool without_actuator;       /* DET-07: no STATUT at all */
  float bias;                  /* INJ-10: value fault injected by the bench */
  relais_death_point death_point;
  relais_tick death_at;        /* tick at which the death point fires */

  uint32_t orders_sent;
  uint32_t shadows_sent;
  uint32_t demandes_sent;
  uint32_t cessions_sent;
  uint32_t stale_dropped;      /* our own orders the actuator rejected */
} relais_node_sm;

typedef struct {
  relais_msg out[RELAIS_OUT_MSG_MAX];
  uint8_t count;
  bool died;             /* the death point fired: the caller kills the machine */
  relais_death_point at; /* which one, for the journal */
  bool deposed;          /* we stopped being the holder at this event */
  bool requested;        /* we asked for the mandate at this event */
} relais_node_out;

void relais_node_init(relais_node_sm *s, const relais_params *p, relais_node self);
void relais_node_clear_out(relais_node_out *o);

/* The tick driver: called on every STATUT (CMD-02). */
void relais_node_on_status(relais_node_sm *s, const relais_msg *statut, relais_node_out *o);
/* State replication from the holder (REP-01, REP-02). */
void relais_node_on_etat(relais_node_sm *s, const relais_msg *etat, relais_node_out *o);
/* SUIVI from the second, seen by the holder. */
void relais_node_on_suivi(relais_node_sm *s, const relais_msg *suivi, relais_node_out *o);
/* No STATUT for bail_ticks: the node deposes itself (MAN-09). */
void relais_node_on_silence(relais_node_sm *s, relais_tick now, relais_node_out *o);
/* A critical local process died: cede the mandate in the same tick (DET-05). */
void relais_node_on_local_fault(relais_node_sm *s, uint8_t reason, relais_node_out *o);
/* Voluntary handover, for maintenance (REL-09). */
void relais_node_on_handover(relais_node_sm *s, relais_node_out *o);

void relais_node_set_bias(relais_node_sm *s, float bias);
void relais_node_arm_death(relais_node_sm *s, relais_death_point dp, relais_tick at);
/* Diagnostic text for /dev/relais/status (OBS-02). */
int relais_node_status_text(const relais_node_sm *s, char *buf, size_t cap);

#endif /* RELAIS_NODE_H */
