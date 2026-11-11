/* Relais — the simulated actuator on the PC (level S, ACT-08 puts it in a
 * third QNX machine at level C).
 *
 * It owns the tempo, the mandate and the rack model:
 *   every period it drains the frames received, lets the arbiter pick the one
 *   value to apply, integrates the plant, writes the journal that is the
 *   authority of the campaign, and publishes STATUT to both computers.
 *
 * All protocol logic comes from the core library: this file is glue, sockets,
 * a clock and a file.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "journal_writer.h"
#include "relais/journal.h"

#define MAX_PEERS 2

typedef struct {
  struct sockaddr_in addr;
  bool set;
} peer_t;

typedef struct {
  relais_params params;
  relais_traj traj;
  relais_key key;
  peer_t peers[MAX_PEERS];
  int sock;
  FILE *journal;
  uint32_t ticks;
  bool realtime;
  const char *state_path;
} config_t;

static void usage(void) {
  fprintf(stderr,
          "usage: relais-actuator --port 7100 --peer A=127.0.0.1:7101 "
          "--peer B=127.0.0.1:7102\n"
          "       [--ticks 4000] [--params T=5,p=1,N=8] [--trajectory \"kind=slalom amp=0.6\"]\n"
          "       [--trajectory-file scenarios/x.json] [--journal out/actuator.jsonl]\n"
          "       [--key <32 hex>] [--no-realtime] [--state out/arb.state]\n");
}

static bool parse_peer(const char *spec, peer_t *out, relais_node *who) {
  char name[16] = {0};
  char host[64] = {0};
  int port = 0;
  if (sscanf(spec, "%15[^=]=%63[^:]:%d", name, host, &port) != 3) return false;
  *who = strcmp(name, "A") == 0 ? RELAIS_NODE_A : RELAIS_NODE_B;
  if (*who == RELAIS_NODE_NONE || port <= 0) return false;
  memset(&out->addr, 0, sizeof out->addr);
  out->addr.sin_family = AF_INET;
  out->addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &out->addr.sin_addr) != 1) return false;
  out->set = true;
  return true;
}

static int open_socket(int port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in local;
  memset(&local, 0, sizeof local);
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  local.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&local, sizeof local) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* Collect every frame received during the previous period. A malformed or
 * forged frame is dropped here and counted by the arbiter (DET-04, SEC-01). */
static size_t drain_socket(int fd, const relais_key *key, relais_msg *msgs, size_t cap) {
  size_t n = 0;
  uint8_t buf[RELAIS_FRAME_MAX];
  while (n < cap) {
    ssize_t got = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
    if (got <= 0) break;
    relais_reject why = RELAIS_REJ_NONE;
    if (relais_frame_decode(buf, (size_t)got, key, &msgs[n], &why)) n++;
  }
  return n;
}

static void publish_statut(int fd, const peer_t *peers, const relais_msg *statut,
                           const relais_key *key) {
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(statut, key, buf, sizeof buf);
  if (len == 0) return;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].set) continue;
    ssize_t sent = sendto(fd, buf, len, 0, (const struct sockaddr *)&peers[i].addr,
                          sizeof peers[i].addr);
    (void)sent; /* UDP: a lost STATUT is a fault the protocol must absorb */
  }
}

static void deadline_add(struct timespec *ts, uint32_t ms) {
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  while (ts->tv_nsec >= 1000000000L) {
    ts->tv_nsec -= 1000000000L;
    ts->tv_sec++;
  }
}

/* Collect every frame that arrives before the tick boundary. Waiting for the
 * whole period instead of sampling it once is what absorbs the jitter of a
 * shared PC (spec 11, first risk): a reply sent 3 ms after the STATUT is still
 * in time for the tick it targets. */
static size_t collect_until(int fd, const relais_key *key, relais_msg *msgs, size_t cap,
                            const struct timespec *deadline) {
  size_t n = drain_socket(fd, key, msgs, cap);
  while (n < cap) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long left_ms = (deadline->tv_sec - now.tv_sec) * 1000 +
                   (deadline->tv_nsec - now.tv_nsec) / 1000000;
    if (left_ms <= 0) break;
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ready = poll(&pfd, 1, (int)(left_ms > 500 ? 500 : left_ms));
    if (ready <= 0) continue;
    n += drain_socket(fd, key, msgs + n, cap - n);
  }
  return n;
}

static void run(config_t *cfg, relais_arb *arb, relais_plant *plant) {
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  const uint32_t period = relais_params_period_ms(&cfg->params);
  float last_value = 0.0f;
  for (uint32_t i = 0; i < cfg->ticks; i++) {
    relais_tick tick = (relais_tick)i + 1;
    deadline_add(&next, period); /* the boundary of this tick */
    relais_msg msgs[RELAIS_IN_MSG_MAX];
    size_t n = cfg->realtime
                   ? collect_until(cfg->sock, &cfg->key, msgs, RELAIS_IN_MSG_MAX, &next)
                   : drain_socket(cfg->sock, &cfg->key, msgs, RELAIS_IN_MSG_MAX);

    relais_meas meas;
    memset(&meas, 0, sizeof meas);
    double seconds = (double)i * relais_params_step_s(&cfg->params);
    meas.pos = plant->pos;
    meas.vel = plant->vel;
    meas.applied = last_value;
    meas.setpoint = relais_traj_setpoint(&cfg->traj, seconds);

    relais_arb_out out;
    relais_arb_step(arb, tick, msgs, n, &meas, &out);
    relais_plant_step(plant, out.value, relais_traj_disturbance(&cfg->traj, seconds),
                      (float)relais_params_step_s(&cfg->params));
    last_value = out.value;

    meas.pos = plant->pos;
    meas.vel = plant->vel;
    meas.applied = out.value;
    out.statut.meas = meas;

    relais_jw_write_events(cfg->journal, &out, (relais_tick)tick);
    relais_jw_write_tick(cfg->journal, &out, (relais_tick)tick, plant, meas.setpoint);
    publish_statut(cfg->sock, cfg->peers, &out.statut, &cfg->key);

    if (!cfg->realtime) continue;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {
    }
  }
}

static bool load_state(config_t *cfg, relais_arb *arb) {
  if (cfg->state_path == NULL) return true;
  FILE *f = fopen(cfg->state_path, "rb");
  if (f == NULL) return true; /* first run: nothing to restore (MAN-11) */
  uint8_t buf[512];
  size_t got = fread(buf, 1, sizeof buf, f);
  fclose(f);
  if (got > 0 && relais_arb_restore(arb, buf, got))
    printf("actuator: mandat restaure %u\n", (unsigned)arb->mandate);
  return true;
}

static void save_state(const config_t *cfg, const relais_arb *arb) {
  if (cfg->state_path == NULL) return;
  uint8_t buf[512];
  size_t len = relais_arb_snapshot(arb, buf, sizeof buf);
  if (len == 0) return;
  FILE *f = fopen(cfg->state_path, "wb");
  if (f == NULL) return;
  fwrite(buf, 1, len, f);
  fclose(f);
}

int main(int argc, char **argv) {
  config_t cfg;
  memset(&cfg, 0, sizeof cfg);
  relais_params_default(&cfg.params);
  relais_traj_default(&cfg.traj);
  relais_key_clear(&cfg.key);
  cfg.ticks = 4000;
  cfg.realtime = true;
  int port = 0;
  char why[128] = "";

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
    else if (strcmp(a, "--peer") == 0 && i + 1 < argc) {
      relais_node who = RELAIS_NODE_NONE;
      peer_t p;
      if (!parse_peer(argv[++i], &p, &who)) { usage(); return 2; }
      cfg.peers[who == RELAIS_NODE_A ? 0 : 1] = p;
    } else if (strcmp(a, "--ticks") == 0 && i + 1 < argc) cfg.ticks = (uint32_t)atoi(argv[++i]);
    else if (strcmp(a, "--params") == 0 && i + 1 < argc) {
      if (!relais_params_parse(&cfg.params, argv[++i], why, sizeof why)) {
        fprintf(stderr, "parametres invalides: %s\n", why);
        return 2;
      }
    } else if (strcmp(a, "--trajectory") == 0 && i + 1 < argc) {
      if (!relais_traj_parse(&cfg.traj, argv[++i], why, sizeof why)) {
        fprintf(stderr, "trajectoire invalide: %s\n", why);
        return 2;
      }
    } else if (strcmp(a, "--key") == 0 && i + 1 < argc) {
      if (!relais_key_from_hex(&cfg.key, argv[++i])) { usage(); return 2; }
    } else if (strcmp(a, "--journal") == 0 && i + 1 < argc) {
      cfg.journal = fopen(argv[++i], "w");
      if (cfg.journal == NULL) { fprintf(stderr, "journal illisible\n"); return 2; }
    } else if (strcmp(a, "--state") == 0 && i + 1 < argc) cfg.state_path = argv[++i];
    else if (strcmp(a, "--no-realtime") == 0) cfg.realtime = false;
    else { usage(); return 2; }
  }
  if (port <= 0 || cfg.journal == NULL) { usage(); return 2; }

  cfg.sock = open_socket(port);
  if (cfg.sock < 0) {
    fprintf(stderr, "actuator: port %d indisponible (%s)\n", port, strerror(errno));
    return 2;
  }

  relais_arb arb;
  relais_arb_init(&arb, &cfg.params);
  load_state(&cfg, &arb);
  relais_plant plant;
  relais_plant_init(&plant, 0.0f, cfg.traj.seed);
  relais_jw_write_header(cfg.journal, &cfg.params, relais_mutant_name(), cfg.traj.seed);

  run(&cfg, &arb, &plant);

  save_state(&cfg, &arb);
  fflush(cfg.journal);
  fclose(cfg.journal);
  close(cfg.sock);
  printf("actuator: %u ticks, mandat %u, titulaire %s, %u releves\n",
         (unsigned)cfg.ticks, (unsigned)arb.mandate, relais_node_name(arb.holder),
         (unsigned)arb.handovers);
  return 0;
}
