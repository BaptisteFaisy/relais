/* Relais — one computer, running on the PC (bench mode "pc").
 *
 * The same binary plays A and B: it is the QNX machine minus QNX. All protocol
 * behaviour comes from the core library (relais_node_sm); this file owns the
 * sockets, the clock and the fault injection switches of the bench.
 *
 * The QNX equivalent lives in target/ (relais-net, relais-core, relais-ctl).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "relais/node.h"

typedef struct {
  relais_node self;
  relais_params params;
  relais_key key;
  int sock;
  struct sockaddr_in actuator;
  struct sockaddr_in peer;
  uint32_t max_ticks;
  float bias;
  relais_death_point death_point;
  relais_tick death_at;
  bool verbose;
} node_cfg_t;

static void usage(void) {
  fprintf(stderr,
          "usage: relais-node --self A --port 7101 --actuator 127.0.0.1:7100\n"
          "       --peer 127.0.0.1:7102 [--ticks 4000] [--params T=5,p=1,N=8]\n"
          "       [--bias 0.1] [--die P3 --at 500] [--key <32 hex>] [--verbose]\n");
}

static bool parse_addr(const char *spec, struct sockaddr_in *out) {
  char host[64] = {0};
  int port = 0;
  if (sscanf(spec, "%63[^:]:%d", host, &port) != 2 || port <= 0) return false;
  memset(out, 0, sizeof *out);
  out->sin_family = AF_INET;
  out->sin_port = htons((uint16_t)port);
  return inet_pton(AF_INET, host, &out->sin_addr) == 1;
}

static relais_death_point parse_death_point(const char *s) {
  if (s == NULL || s[0] == '\0') return RELAIS_DP_NONE;
  if (s[0] == 'P' && s[1] >= '1' && s[1] <= '9' && s[2] == '\0')
    return (relais_death_point)(s[1] - '0');
  return RELAIS_DP_NONE;
}

static void send_to(int fd, const struct sockaddr_in *dst, const relais_msg *m,
                    const relais_key *key) {
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(m, key, buf, sizeof buf);
  if (len == 0) return;
  ssize_t sent = sendto(fd, buf, len, 0, (const struct sockaddr *)dst, sizeof *dst);
  (void)sent;
}

/* Orders, shadows, demandes and cessions go to the actuator; the replicated
 * state and the follow-up go to the peer computer (spec 4.9). */
static void dispatch(const node_cfg_t *cfg, const relais_node_out *out) {
  for (uint8_t i = 0; i < out->count; i++) {
    const relais_msg *m = &out->out[i];
    bool to_peer = relais_msg_to_peer(m->type);
    send_to(cfg->sock, to_peer ? &cfg->peer : &cfg->actuator, m, &cfg->key);
  }
}

static int recv_frame(const node_cfg_t *cfg, relais_msg *msg, uint32_t timeout_ms) {
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
  setsockopt(cfg->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  uint8_t buf[RELAIS_FRAME_MAX];
  ssize_t got = recv(cfg->sock, buf, sizeof buf, 0);
  if (got <= 0) return got == 0 ? 0 : -1;
  relais_reject why = RELAIS_REJ_NONE;
  if (!relais_frame_decode(buf, (size_t)got, &cfg->key, msg, &why)) return -2;
  return 1;
}

static void report(const node_cfg_t *cfg, const relais_node_sm *sm, const char *what) {
  if (!cfg->verbose) return;
  char text[256];
  relais_node_status_text(sm, text, sizeof text);
  printf("node %s [%s] %s\n", relais_node_name(cfg->self), what, text);
  fflush(stdout);
}

int main(int argc, char **argv) {
  node_cfg_t cfg;
  memset(&cfg, 0, sizeof cfg);
  relais_params_default(&cfg.params);
  relais_key_clear(&cfg.key);
  cfg.self = RELAIS_NODE_NONE;
  cfg.max_ticks = 4000;
  int port = 0;
  char why[128] = "";
  const char *death = NULL;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--self") == 0 && i + 1 < argc) {
      const char *v = argv[++i];
      cfg.self = strcmp(v, "A") == 0 ? RELAIS_NODE_A : (strcmp(v, "B") == 0 ? RELAIS_NODE_B : RELAIS_NODE_NONE);
    } else if (strcmp(a, "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
    else if (strcmp(a, "--actuator") == 0 && i + 1 < argc) {
      if (!parse_addr(argv[++i], &cfg.actuator)) { usage(); return 2; }
    } else if (strcmp(a, "--peer") == 0 && i + 1 < argc) {
      if (!parse_addr(argv[++i], &cfg.peer)) { usage(); return 2; }
    } else if (strcmp(a, "--ticks") == 0 && i + 1 < argc) cfg.max_ticks = (uint32_t)atoi(argv[++i]);
    else if (strcmp(a, "--params") == 0 && i + 1 < argc) {
      if (!relais_params_parse(&cfg.params, argv[++i], why, sizeof why)) {
        fprintf(stderr, "parametres invalides: %s\n", why);
        return 2;
      }
    } else if (strcmp(a, "--bias") == 0 && i + 1 < argc) cfg.bias = (float)atof(argv[++i]);
    else if (strcmp(a, "--die") == 0 && i + 1 < argc) death = argv[++i];
    else if (strcmp(a, "--at") == 0 && i + 1 < argc) cfg.death_at = (relais_tick)strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--key") == 0 && i + 1 < argc) {
      if (!relais_key_from_hex(&cfg.key, argv[++i])) { usage(); return 2; }
    } else if (strcmp(a, "--verbose") == 0) cfg.verbose = true;
    else { usage(); return 2; }
  }
  if (!relais_node_is_computer(cfg.self) || port <= 0) { usage(); return 2; }

  cfg.sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (cfg.sock < 0) { perror("socket"); return 2; }
  struct sockaddr_in local;
  memset(&local, 0, sizeof local);
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  local.sin_port = htons((uint16_t)port);
  if (bind(cfg.sock, (struct sockaddr *)&local, sizeof local) != 0) {
    perror("bind");
    return 2;
  }

  relais_node_sm sm;
  relais_node_init(&sm, &cfg.params, cfg.self);
  relais_node_set_bias(&sm, cfg.bias);
  cfg.death_point = parse_death_point(death);
  if (cfg.death_point != RELAIS_DP_NONE) relais_node_arm_death(&sm, cfg.death_point, cfg.death_at);

  const uint32_t period = relais_params_period_ms(&cfg.params);
  relais_tick ticks = 0;
  while (ticks < cfg.max_ticks) {
    relais_msg msg;
    int got = recv_frame(&cfg, &msg, period * 3);
    relais_node_out out;
    relais_node_clear_out(&out);
    if (got < 0) {
      /* No STATUT: the node deposes itself after the bail (MAN-09). */
      relais_node_on_silence(&sm, sm.now + 1, &out);
      report(&cfg, &sm, "silence");
    } else if (got == 0) {
      continue;
    } else if (msg.type == RELAIS_MSG_STATUT) {
      relais_node_on_status(&sm, &msg, &out);
      ticks = msg.tick;
      report(&cfg, &sm, "statut");
    } else if (msg.type == RELAIS_MSG_ETAT) {
      relais_node_on_etat(&sm, &msg, &out);
      report(&cfg, &sm, "etat");
    } else if (msg.type == RELAIS_MSG_SUIVI) {
      relais_node_on_suivi(&sm, &msg, &out);
    } else {
      relais_node_clear_out(&out);
    }
    dispatch(&cfg, &out);
    if (out.died) {
      printf("node %s: point de mort %d au tick %llu\n", relais_node_name(cfg.self),
             (int)out.at, (unsigned long long)ticks);
      fflush(stdout);
      raise(SIGKILL); /* INJ-07: the machine dies exactly here */
      return 3;
    }
  }

  char text[512];
  relais_node_status_text(&sm, text, sizeof text);
  printf("node %s: %llu ticks, %s\n", relais_node_name(cfg.self),
         (unsigned long long)ticks, text);
  close(cfg.sock);
  return 0;
}
