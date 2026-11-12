/* Relais — glue shared by every RTOS port (mission M32, ideas 73 and 74).
 *
 * A port to another RTOS must not re-invent the protocol: the state machine is
 * the core, and the only decision a port makes is "which entry point does this
 * decoded frame go to, and where does each output frame go". Both answers are
 * here, once, so ports/zephyr and ports/freertos cannot drift apart.
 *
 * Pure C11, no allocation, no clock, no I/O — the same contract as core/.
 *
 * Requirement IDs: CMD-02 (the tick comes from STATUT), MAN-09 (silence),
 * REP-01/REP-02 (state replication), spec 4.9 (the two links).
 */
#ifndef RELAIS_PORTS_COMMON_H
#define RELAIS_PORTS_COMMON_H

#include <stdbool.h>
#include <stddef.h>

#include "relais/frame.h"
#include "relais/node.h"
#include "relais/transport.h"

/* Hand one decoded frame to the right entry point of the node state machine.
 * Returns false when the frame is not for a computer at all (a STATUT echoed
 * back, an unknown type): the caller drops it and counts nothing, because the
 * transport already authenticated it (DET-04 happened before this point). */
static inline bool relais_port_dispatch(relais_node_sm *sm, const relais_msg *m,
                                        relais_node_out *o) {
  if (sm == NULL || m == NULL) return false;
  switch (m->type) {
    case RELAIS_MSG_STATUT:
      relais_node_on_status(sm, m, o); /* CMD-02: this IS the tick */
      return true;
    case RELAIS_MSG_ETAT:
      relais_node_on_etat(sm, m, o); /* REP-02 */
      return true;
    case RELAIS_MSG_SUIVI:
      relais_node_on_suivi(sm, m, o);
      return true;
    default:
      return false;
  }
}

/* Send every frame a tick produced, on the link the frame belongs to. One rule
 * for all ports and for the PC node: spec 4.9, already implemented by the core
 * through relais_tr_send_msg(). `scratch` is caller memory of RELAIS_FRAME_MAX
 * bytes, so nothing is allocated on the way out (SEC-05). */
static inline size_t relais_port_publish(relais_transport *t, const relais_key *key,
                                         const relais_node_out *o, uint8_t *scratch,
                                         size_t cap) {
  size_t sent = 0;
  if (t == NULL || o == NULL || scratch == NULL) return 0;
  for (uint8_t i = 0; i < o->count; i++)
    if (relais_tr_send_msg(t, key, &o->out[i], scratch, cap) == RELAIS_TR_OK) sent++;
  return sent;
}

/* Map a QNX SCHED_FIFO priority (62 net, 61 core, 60 ctl — lower is stronger)
 * onto the priority scale of another kernel. The two ports below state their own
 * rule next to their thread definitions; keeping the source numbers here means a
 * reader can check the ordering claim in one place (QNX-01, spec 4.9). */
#define RELAIS_QNX_PRIO_NET  62
#define RELAIS_QNX_PRIO_CORE 61
#define RELAIS_QNX_PRIO_CTL  60

#endif /* RELAIS_PORTS_COMMON_H */
