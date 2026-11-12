/* Relais — SocketCAN backend, declarations (mission M32).
 *
 * The whole file compiles to nothing when the host has no <linux/can.h>:
 * the CAN transport itself (can.c) is portable and is tested against a memory
 * bus, only this backend is Linux-specific. The Makefile defines
 * RELAIS_HAVE_SOCKETCAN when the header is present.
 */
#ifndef RELAIS_HOST_TRANSPORT_CAN_SOCKETCAN_H
#define RELAIS_HOST_TRANSPORT_CAN_SOCKETCAN_H

#include "can.h"

#ifdef RELAIS_HAVE_SOCKETCAN

typedef struct {
  int sock;
} relais_can_socketcan;

/* Open and bind a CAN interface by name ("can0", "vcan0"). */
int relais_can_socketcan_open(relais_can_socketcan *s, const char *ifname);
/* Fill the bus vtable that relais_can_attach() takes. */
void relais_can_socketcan_bus(relais_can_bus *bus, relais_can_socketcan *s);
void relais_can_socketcan_close(relais_can_socketcan *s);

#endif /* RELAIS_HAVE_SOCKETCAN */

#endif /* RELAIS_HOST_TRANSPORT_CAN_SOCKETCAN_H */
