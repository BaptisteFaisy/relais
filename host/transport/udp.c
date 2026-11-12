/* Relais — the UDP transport (mission M32, idea 75). See udp.h for the roles.
 *
 * Behaviour kept identical to the code it replaces:
 *   - SOCK_DGRAM, one socket for both links (host/node/relais-node.c);
 *   - recv with SO_RCVTIMEO when a timeout is configured, MSG_DONTWAIT when it
 *     is 0 (actuator/pc/relais-actuator.c drains without ever blocking);
 *   - a send failure is counted, never retried: UDP loses, the protocol absorbs
 *     (ROB-02, ROB-03);
 *   - a received datagram bigger than the buffer is truncated by the kernel and
 *     then refused by the frame decoder, exactly as before (DET-04).
 */
#include "udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

bool relais_udp_parse_endpoint(const char *spec, struct sockaddr_in *out) {
  if (spec == NULL || out == NULL) return false;
  char host[64] = {0};
  int port = 0;
  memset(out, 0, sizeof *out);
  out->sin_family = AF_INET;
  out->sin_addr.s_addr = htonl(INADDR_ANY);
  if (spec[0] == ':') {
    /* ":7201" — a port to bind on every interface, what the PC node does. */
    if (sscanf(spec + 1, "%d", &port) != 1) return false;
  } else if (sscanf(spec, "%63[^:]:%d", host, &port) == 2) {
    if (host[0] != '\0' && strcmp(host, "*") != 0 &&
        inet_pton(AF_INET, host, &out->sin_addr) != 1)
      return false;
  } else if (sscanf(spec, "%d", &port) != 1) {
    return false;
  }
  if (port <= 0 || port > 65535) return false;
  out->sin_port = htons((uint16_t)port);
  return true;
}

static int udp_init(void *ctx, const relais_tr_cfg *cfg) {
  relais_udp *u = (relais_udp *)ctx;
  if (u == NULL || cfg == NULL) return RELAIS_TR_ERR_CONFIG;
  memset(u, 0, sizeof *u);
  u->sock = -1;
  u->dst_set[RELAIS_LINK_ACTUATOR] =
      relais_udp_parse_endpoint(cfg->actuator, &u->dst[RELAIS_LINK_ACTUATOR]);
  u->dst_set[RELAIS_LINK_PEER] =
      relais_udp_parse_endpoint(cfg->peer, &u->dst[RELAIS_LINK_PEER]);
  if (!u->dst_set[RELAIS_LINK_ACTUATOR] && !u->dst_set[RELAIS_LINK_PEER])
    return RELAIS_TR_ERR_CONFIG;

  u->sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (u->sock < 0) return RELAIS_TR_ERR_IO;

  if (cfg->local != NULL) {
    struct sockaddr_in local;
    if (!relais_udp_parse_endpoint(cfg->local, &local)) {
      close(u->sock);
      u->sock = -1;
      return RELAIS_TR_ERR_CONFIG;
    }
    if (bind(u->sock, (const struct sockaddr *)&local, sizeof local) != 0) {
      close(u->sock);
      u->sock = -1;
      return RELAIS_TR_ERR_IO;
    }
  }

  u->timeout_ms = cfg->timeout_ms;
  if (u->timeout_ms > 0) {
    struct timeval tv;
    tv.tv_sec = (time_t)(u->timeout_ms / 1000u);
    tv.tv_usec = (suseconds_t)((u->timeout_ms % 1000u) * 1000u);
    if (setsockopt(u->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
      close(u->sock);
      u->sock = -1;
      return RELAIS_TR_ERR_IO;
    }
  }
  return RELAIS_TR_OK;
}

static int udp_send(void *ctx, relais_link link, const uint8_t *frame, size_t len) {
  relais_udp *u = (relais_udp *)ctx;
  if (u == NULL || u->sock < 0) return RELAIS_TR_CLOSED;
  if ((unsigned)link >= (unsigned)RELAIS_LINK_COUNT || !u->dst_set[link])
    return RELAIS_TR_ERR_CONFIG;
  if (frame == NULL || len == 0 || len > RELAIS_FRAME_MAX) return RELAIS_TR_ERR_FULL;
  ssize_t sent = sendto(u->sock, frame, len, 0, (const struct sockaddr *)&u->dst[link],
                        sizeof u->dst[link]);
  if (sent < 0 || (size_t)sent != len) {
    u->send_errors++;
    return RELAIS_TR_ERR_IO; /* a lost datagram: reported, never retried */
  }
  u->sent++;
  u->bytes_out += (uint32_t)len;
  return RELAIS_TR_OK;
}

/* Which link a datagram came in on: the source address decides, so a computer
 * tells a STATUT from the actuator apart from an ETAT of its peer even though
 * both arrive on the same socket. An unknown source is reported as the actuator
 * link and the frame decoder judges it (DET-04). */
static relais_link link_of_source(const relais_udp *u, const struct sockaddr_in *from) {
  for (int l = 0; l < RELAIS_LINK_COUNT; l++) {
    if (!u->dst_set[l]) continue;
    if (u->dst[l].sin_addr.s_addr == from->sin_addr.s_addr &&
        u->dst[l].sin_port == from->sin_port)
      return (relais_link)l;
  }
  return RELAIS_LINK_ACTUATOR;
}

static int udp_recv(void *ctx, relais_link *link, uint8_t *buf, size_t cap, size_t *len) {
  relais_udp *u = (relais_udp *)ctx;
  if (u == NULL || u->sock < 0) return RELAIS_TR_CLOSED;
  if (buf == NULL || len == NULL || cap == 0) return RELAIS_TR_ERR_CONFIG;
  struct sockaddr_in from;
  socklen_t fromlen = sizeof from;
  memset(&from, 0, sizeof from);
  int flags = u->timeout_ms > 0 ? 0 : MSG_DONTWAIT;
  ssize_t got = recvfrom(u->sock, buf, cap, flags, (struct sockaddr *)&from, &fromlen);
  if (got < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      u->timeouts++;
      return RELAIS_TR_TIMEOUT; /* ROB-01: silence, not a stale frame */
    }
    return RELAIS_TR_ERR_IO;
  }
  if (got == 0) {
    u->timeouts++;
    return RELAIS_TR_TIMEOUT;
  }
  *len = (size_t)got;
  u->received++;
  u->bytes_in += (uint32_t)got;
  if (link != NULL) *link = link_of_source(u, &from);
  return RELAIS_TR_OK;
}

static void udp_close(void *ctx) {
  relais_udp *u = (relais_udp *)ctx;
  if (u == NULL) return;
  if (u->sock >= 0) close(u->sock);
  u->sock = -1;
}

void relais_udp_attach(relais_transport *t, relais_udp *u) {
  if (t == NULL) return;
  memset(t, 0, sizeof *t);
  t->name = "udp";
  t->ctx = u;
  t->init = udp_init;
  t->send = udp_send;
  t->recv = udp_recv;
  t->close = udp_close;
}
