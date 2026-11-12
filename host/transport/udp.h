/* Relais — the UDP transport, behind the interface of relais/transport.h
 * (mission M32, idea 75).
 *
 * This is the wire the project has always used, extracted from the PC node and
 * the PC actuator into ONE implementation of the interface: same socket type,
 * same bind, same timeout, same "a lost datagram is a fault the protocol
 * absorbs" behaviour. Nothing here knows what a mandate is; the frames are
 * opaque bytes (SEC-05: only this layer owns a socket).
 *
 * Two roles, one code path, chosen by the configuration:
 *   computer  cfg.local = ":7201"        -> bind INADDR_ANY, blocking recv with
 *                                           a timeout (host/node/relais-node.c)
 *   actuator  cfg.local = "127.0.0.1:7200", cfg.timeout_ms = 0
 *                                        -> bind loopback, non-blocking drain
 *                                           (actuator/pc/relais-actuator.c)
 */
#ifndef RELAIS_HOST_TRANSPORT_UDP_H
#define RELAIS_HOST_TRANSPORT_UDP_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "relais/transport.h"

/* One UDP endpoint. All memory is caller-provided: no allocation anywhere. */
typedef struct {
  int sock;
  struct sockaddr_in dst[RELAIS_LINK_COUNT];
  bool dst_set[RELAIS_LINK_COUNT];
  uint32_t timeout_ms;  /* 0 = non-blocking recv (MSG_DONTWAIT) */
  uint32_t sent;
  uint32_t received;
  uint32_t timeouts;
  uint32_t send_errors;
  uint32_t bytes_out;
  uint32_t bytes_in;
} relais_udp;

/* Parse "host:port", ":port" or "port". An empty or "*" host means INADDR_ANY,
 * exactly like the PC node binds it (a QEMU guest is reached through slirp, so
 * binding loopback only would break the bench). */
bool relais_udp_parse_endpoint(const char *spec, struct sockaddr_in *out);

/* Fill a vtable that drives `u`. Call once, then relais_tr_init(). */
void relais_udp_attach(relais_transport *t, relais_udp *u);

#endif /* RELAIS_HOST_TRANSPORT_UDP_H */
