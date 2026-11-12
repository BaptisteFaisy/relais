/* Relais — SocketCAN backend for the CAN transport (mission M32).
 *
 * The thin layer that turns a Linux CAN interface into the relais_can_bus
 * vtable of can.h. It owns the socket (SEC-05); the segmentation and the
 * protocol stay in can.c and the core. Filtering by identifier is done in
 * can.c, not with CAN_RAW_FILTER: one place implements it, so a memory bus in a
 * test and a real controller behave the same. On a busy production bus, adding
 * CAN_RAW_FILTER here is a pure optimisation — it drops nothing that can.c
 * would have kept.
 *
 * BUILD: compiled only where <linux/can.h> exists (RELAIS_HAVE_SOCKETCAN). It
 * was compiled on this machine; running it needs a CAN interface, which needs
 * privileges this checkout does not have (see docs/PORT-TRANSPORTS.md).
 */
#include "can_socketcan.h"

#ifdef RELAIS_HAVE_SOCKETCAN

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int relais_can_socketcan_open(relais_can_socketcan *s, const char *ifname) {
  if (s == NULL || ifname == NULL) return RELAIS_TR_ERR_CONFIG;
  memset(s, 0, sizeof *s);
  s->sock = -1;
  s->sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (s->sock < 0) return RELAIS_TR_ERR_IO;

  /* if_nametoindex is POSIX, so no ioctl, no struct ifreq and no _GNU_SOURCE:
   * this file stays inside the same feature macros as the rest of the host. */
  unsigned index = if_nametoindex(ifname);
  if (index == 0u) {
    relais_can_socketcan_close(s);
    return RELAIS_TR_ERR_CONFIG; /* no such interface */
  }
  struct sockaddr_can addr;
  memset(&addr, 0, sizeof addr);
  addr.can_family = AF_CAN;
  addr.can_ifindex = (int)index;
  if (bind(s->sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
    relais_can_socketcan_close(s);
    return RELAIS_TR_ERR_IO;
  }
  return RELAIS_TR_OK;
}

static int socketcan_xmit(void *ctx, const relais_can_frame *f) {
  relais_can_socketcan *s = (relais_can_socketcan *)ctx;
  if (s == NULL || s->sock < 0 || f == NULL) return RELAIS_TR_CLOSED;
  struct can_frame out;
  memset(&out, 0, sizeof out);
  out.can_id = (canid_t)f->id;
  out.can_dlc = (f->len > RELAIS_CAN_DLC) ? RELAIS_CAN_DLC : f->len;
  memcpy(out.data, f->data, out.can_dlc);
  ssize_t written = write(s->sock, &out, sizeof out);
  return written == (ssize_t)sizeof out ? RELAIS_TR_OK : RELAIS_TR_ERR_IO;
}

static int socketcan_recv(void *ctx, relais_can_frame *f) {
  relais_can_socketcan *s = (relais_can_socketcan *)ctx;
  if (s == NULL || s->sock < 0 || f == NULL) return RELAIS_TR_CLOSED;
  struct can_frame in;
  memset(&in, 0, sizeof in);
  ssize_t got = recv(s->sock, &in, sizeof in, MSG_DONTWAIT);
  if (got < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return RELAIS_TR_TIMEOUT;
    return RELAIS_TR_ERR_IO;
  }
  if ((size_t)got < sizeof in) return RELAIS_TR_ERR_IO;
  f->id = (uint32_t)in.can_id & RELAIS_CAN_ID_MAX; /* extended flag dropped */
  f->len = in.can_dlc > RELAIS_CAN_DLC ? RELAIS_CAN_DLC : in.can_dlc;
  memcpy(f->data, in.data, f->len);
  return RELAIS_TR_OK;
}

void relais_can_socketcan_bus(relais_can_bus *bus, relais_can_socketcan *s) {
  if (bus == NULL) return;
  memset(bus, 0, sizeof *bus);
  bus->name = "socketcan";
  bus->ctx = s;
  bus->xmit = socketcan_xmit;
  bus->recv = socketcan_recv;
}

void relais_can_socketcan_close(relais_can_socketcan *s) {
  if (s == NULL) return;
  if (s->sock >= 0) close(s->sock);
  s->sock = -1;
}

#endif /* RELAIS_HAVE_SOCKETCAN */
