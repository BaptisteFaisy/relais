/* Relais — a CAN transport behind the interface of relais/transport.h
 * (mission M32, idea 75).
 *
 * CHOICE: classic CAN 2.0 (11-bit identifier, 8-byte payload) with an ISO
 * 15765-2 style segmentation, rather than SOME/IP over automotive Ethernet.
 * Why, and what the protocol loses and gains:
 *
 *   + bounded latency: a CAN frame is at most ~130 bits on the wire and the
 *     identifier fixes the arbitration order, so the actuator link (lower
 *     identifier) always wins the bus. SOME/IP inherits Ethernet's switch and
 *     queueing delays, which are harder to bound on a shared trunk.
 *   + no IP stack, no DHCP, no port: one identifier per direction is the whole
 *     addressing, which is what a steering ECU can be audited on (SEC-05).
 *   - frame size: a Relais frame is 40..512 bytes and a CAN payload is 8, so
 *     every frame is segmented (a 68-byte STATUT becomes 10 CAN frames). The
 *     bus load is the real cost, measured by host/transport/test_can.c.
 *   - one identifier per direction, not per message: the identifier is spent on
 *     routing and priority, not on the message type. Giving each of the seven
 *     Relais types its own identifier would refine the arbitration, at the cost
 *     of peeking into the frame header, which this layer refuses to do: the
 *     frames stay opaque here, exactly as they are over UDP.
 *
 * Segmentation follows ISO 15765-2 in the way a real stack does it: one
 * reassembly channel per identifier, so two senders can have a multi-frame
 * transfer in flight at the same time without eating each other's fragments.
 * A single channel per endpoint was tried first and host/transport/test_can.c
 * caught it: on a shared bus the flows DO interleave, and 88 of 200 ticks came
 * back with a different decision. That is why the state below is an array.
 *
 * The core is untouched: this file is one more implementation of the vtable.
 */
#ifndef RELAIS_HOST_TRANSPORT_CAN_H
#define RELAIS_HOST_TRANSPORT_CAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "relais/transport.h"

#define RELAIS_CAN_DLC 8u        /* classic CAN payload */
#define RELAIS_CAN_RX_FILTERS 4u /* identifiers one endpoint listens to */
#define RELAIS_CAN_ID_MAX 0x7ffu /* 11-bit identifier of CAN 2.0A */

/* Protocol byte of ISO 15765-2, reduced to what Relais needs: single frame,
 * first frame, consecutive frame. No flow control — the sender segments a whole
 * Relais frame in one call, and one flow per direction is enough because a node
 * emits its frames sequentially inside a tick (spec 4.9). */
#define RELAIS_CAN_PCI_SF 0x00u
#define RELAIS_CAN_PCI_FF 0x10u
#define RELAIS_CAN_PCI_CF 0x20u
#define RELAIS_CAN_PCI_MASK 0xf0u
#define RELAIS_CAN_SEQ_MASK 0x0fu
#define RELAIS_CAN_SF_MAX 7u /* payload bytes of a single frame */
#define RELAIS_CAN_FF_MAX 6u /* payload bytes of a first frame */
#define RELAIS_CAN_CF_MAX 7u /* payload bytes of a consecutive frame */

typedef struct {
  uint32_t id;
  uint8_t len;
  uint8_t data[RELAIS_CAN_DLC];
} relais_can_frame;

/* The bus itself. One implementation for SocketCAN (can_socketcan.c), one for
 * the tests (an in-memory shared medium). A transport never opens a device. */
typedef struct {
  void *ctx;
  const char *name;
  int (*xmit)(void *ctx, const relais_can_frame *f);
  /* Non-blocking: RELAIS_TR_TIMEOUT when the medium has nothing for us. */
  int (*recv)(void *ctx, relais_can_frame *f);
} relais_can_bus;

/* One reassembly channel: the state of a segmented Relais frame arriving on one
 * identifier. RELAIS_CAN_RX_FILTERS of them run at the same time. */
typedef struct {
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len;
  size_t total;
  uint8_t seq;
  bool active;
} relais_can_flow;

typedef struct {
  relais_can_bus bus;
  uint32_t tx_id[RELAIS_LINK_COUNT]; /* identifier used to send on a link */
  uint32_t rx_id[RELAIS_CAN_RX_FILTERS];
  relais_link rx_link[RELAIS_CAN_RX_FILTERS];
  relais_can_flow rx[RELAIS_CAN_RX_FILTERS];
  uint8_t rx_filters;
  bool closed;

  /* Counters, for the bench and the tests. */
  uint32_t tx_frames;   /* CAN frames put on the bus */
  uint32_t rx_frames;   /* CAN frames accepted by the filter */
  uint32_t dropped_id;  /* somebody else's traffic */
  uint32_t dropped_pci; /* unexpected or out-of-sequence fragment */
  uint32_t lost_flows;  /* a segmentation abandoned: counted, never guessed */
  uint32_t delivered;   /* whole Relais frames handed up */
} relais_can;

/* CAN frames needed to carry a Relais frame of `len` bytes. Pure arithmetic:
 * what the bus load of a transport change is, before plugging anything in. */
uint32_t relais_can_fragment_count(size_t len);

/* Attach the vtable and set the two transmit identifiers. The actuator link is
 * meant to get the LOWER identifier: on CAN, a lower identifier wins the
 * arbitration, so ORDRE and STATUT are the frames the bus serves first. */
void relais_can_attach(relais_transport *t, relais_can *c, const relais_can_bus *bus,
                       uint32_t id_actuator, uint32_t id_peer);

/* One entry of the receive filter: which identifier arrives on which link. */
void relais_can_accept(relais_can *c, uint32_t id, relais_link link);

#endif /* RELAIS_HOST_TRANSPORT_CAN_H */
