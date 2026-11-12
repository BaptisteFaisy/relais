/* Relais — relais-net, priority 62 (spec 4.9).
 *
 * The only process that owns a socket (SEC-05). It does one job: turn
 * datagrammes into validated messages for relais-core, and the other way
 * round. It knows nothing about mandates, roles or the control law — that is
 * what keeps the critical path short and the crash blast radius small.
 *
 * A datagramme that fails the shape or the authenticator check is dropped here
 * and counted, so relais-core never sees a forged frame (DET-04, SEC-01).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "relais_qnx.h"

typedef struct {
  int channel;               /* our own channel: relais-core connects to it */
  int sock_actuator;         /* UDP, towards the actuator */
  int sock_peer;             /* UDP, towards the other computer */
  struct sockaddr_in actuator;
  struct sockaddr_in peer;
  relais_key key;
  uint32_t received;
  uint32_t rejected;
  uint32_t sent;
} net_t;

static void usage(void) {
  fprintf(stderr,
          "usage: relais-net --actuator HOST:PORT --peer HOST:PORT [--key HEX32]\n");
}

/* One datagramme in: validate, then hand it to the core. */
static void pump_socket(net_t *n, int fd) {
  uint8_t buf[RELAIS_FRAME_MAX];
  ssize_t got = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
  if (got <= 0) return;
  relais_msg msg;
  relais_reject why = RELAIS_REJ_NONE;
  if (!relais_frame_decode(buf, (size_t)got, &n->key, &msg, &why)) {
    n->rejected++;
    rl_log(SLOG2_WARNING, "net: frame dropped (%s)", relais_reject_name(why));
    return;
  }
  n->received++;
  rl_frame_msg body;
  memset(&body, 0, sizeof body);
  body.h.type = RL_MSG_FRAME;
  body.msg = msg;
  rl_status_msg ack;
  if (MsgSend(n->channel, &body, sizeof body, &ack, sizeof ack) == -1 &&
      errno == ESRCH) {
    /* QNX-05: the core is gone. Nothing to wait for, nothing to poll: the
     * connection is closed, and this process exits so the supervisor restarts
     * it and the machine cedes the mandate within the tick (DET-05). */
    rl_log(SLOG2_ERROR, "net: relais-core is gone");
    exit(1);
  }
}

/* One frame out: encode, then choose the socket by message type. */
static void send_frame(net_t *n, const relais_msg *m) {
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(m, &n->key, buf, sizeof buf);
  if (len == 0) {
    rl_log(SLOG2_WARNING, "net: frame too big to encode");
    return;
  }
  const struct sockaddr_in *dst =
      relais_msg_to_peer(m->type) ? &n->peer : &n->actuator;
  int fd = relais_msg_to_peer(m->type) ? n->sock_peer : n->sock_actuator;
  if (sendto(fd, buf, len, 0, (const struct sockaddr *)dst, sizeof *dst) < 0) {
    rl_log(SLOG2_WARNING, "net: send failed");
    return;
  }
  n->sent++;
  rl_trace("net tx %s tick %llu", relais_msg_type_name(m->type),
           (unsigned long long)m->tick);
}

/* What relais-core asks us to put on the wire. */
static void serve_core(net_t *n) {
  rl_frame_msg in;
  struct _msg_info info;
  /* MsgReceive() takes the message info as its fourth argument and returns the
   * receive id, which is what MsgReply() answers. */
  rcvid_t rcvid = MsgReceive(n->channel, &in, sizeof in, &info);
  if (rcvid == -1) return;
  if (in.h.type == RL_MSG_FRAME) send_frame(n, &in.msg);
  MsgReply(rcvid, 0, NULL, 0);
}

/* A QNX channel is pollable like a socket, so one poll covers the two links
 * and the core at the same time: no busy waiting anywhere (QNX-02). */
static void net_loop(net_t *n) {
  struct pollfd fds[3];
  fds[0].fd = n->sock_actuator;
  fds[1].fd = n->sock_peer;
  fds[2].fd = n->channel;
  for (int i = 0; i < 3; i++) fds[i].events = POLLIN;
  for (;;) {
    int ready = poll(fds, 3, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[0].revents & POLLIN) pump_socket(n, n->sock_actuator);
    if (fds[1].revents & POLLIN) pump_socket(n, n->sock_peer);
    if (fds[2].revents & POLLIN) serve_core(n);
  }
}

int main(int argc, char **argv) {
  net_t n;
  memset(&n, 0, sizeof n);
  relais_key_clear(&n.key);
  const char *actuator = NULL;
  const char *peer = NULL;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--actuator") && i + 1 < argc) actuator = argv[++i];
    else if (!strcmp(argv[i], "--peer") && i + 1 < argc) peer = argv[++i];
    else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
      if (!relais_key_from_hex(&n.key, argv[++i])) { usage(); return 2; }
    } else { usage(); return 2; }
  }
  if (actuator == NULL || peer == NULL) { usage(); return 2; }
  if (rl_udp_peer(actuator, &n.actuator, sizeof n.actuator) != 0 ||
      rl_udp_peer(peer, &n.peer, sizeof n.peer) != 0) {
    usage();
    return 2;
  }

  rl_realtime(RELAIS_PRIO_NET);
  n.sock_actuator = rl_udp_bind(0);
  n.sock_peer = rl_udp_bind(0);
  n.channel = rl_channel(RELAIS_NET_CHANNEL);
  if (n.sock_actuator < 0 || n.sock_peer < 0) {
    rl_log(SLOG2_ERROR, "net: socket unavailable");
    return 2;
  }
#ifdef __QNX__
  if (n.channel < 0) {
    rl_log(SLOG2_ERROR, "net: cannot attach %s", RELAIS_NET_CHANNEL);
    return 2;
  }
#endif
  rl_log(SLOG2_INFO, "net: up, priority %d", RELAIS_PRIO_NET);
  net_loop(&n);
  rl_log(SLOG2_INFO, "net: %u received, %u rejected, %u sent", (unsigned)n.received,
         (unsigned)n.rejected, (unsigned)n.sent);
  close(n.sock_actuator);
  close(n.sock_peer);
  return 0;
}
