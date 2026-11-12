/* Relais — one computer, running on the PC (bench mode "pc").
 *
 * The same binary plays A and B: it is the QNX machine minus QNX. All protocol
 * behaviour comes from the core library (relais_node_sm); this file owns the
 * sockets, the clock and the fault injection switches of the bench.
 *
 * Mission M02 adds an optional latency measurement (PERF-01, PERF-07, PERF-08):
 * with --latency FILE the node times, per tick, the segment "STATUT received ->
 * ORDRE emitted", the frame decode + MAC check (SEC-05) and the control law
 * (CMD-03), and appends one JSON line per sample. Without the option nothing is
 * measured and the binary behaves exactly as before: every clock read sits
 * behind the g_lat_on switch, and the recorder is a fixed table (no allocation
 * on the measured path, ROB-04).
 *
 * The QNX equivalent lives in target/ (relais-net, relais-core, relais-ctl),
 * which keeps its own trace points (rl_trace, QNX-04); this file is host-only.
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

#include "latency.h"
#include "relais/node.h"

/* Flush the sample file every so many lines: a SIGKILL'd node (INJ-07) loses at
 * most this many samples, and the flush stays off the measured interval. */
#define LAT_FLUSH_EVERY 256u

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
  const char *latency_path; /* M02: NULL keeps the node uninstrumented */
} node_cfg_t;

/* Monotonic instants taken around the receive path (M02). */
typedef struct {
  uint64_t rx;      /* just after recv() returned the datagram */
  uint64_t decoded; /* just after the frame passed CRC + MAC */
} lat_marks;

/* One recorder per measured segment, in .bss: nothing is allocated at runtime. */
static rl_lat_rec g_lat[RL_LAT_SRC_COUNT];
static FILE *g_lat_file;
static bool g_lat_on;
static unsigned g_lat_pending;

static void usage(void) {
  fprintf(stderr,
          "usage: relais-node --self A --port 7101 --actuator 127.0.0.1:7100\n"
          "       --peer 127.0.0.1:7102 [--ticks 4000] [--params T=5,p=1,N=8]\n"
          "       [--bias 0.1] [--die P3 --at 500] [--key <32 hex>] [--verbose]\n"
          "       [--latency samples.jsonl]\n");
}

/* ---- M02: measurement plumbing ---- */

static bool lat_setup(const node_cfg_t *cfg) {
  if (cfg->latency_path == NULL) return true;
  const char *role = relais_node_name(cfg->self);
  for (unsigned i = 0; i < (unsigned)RL_LAT_SRC_COUNT; i++)
    rl_lat_reset(&g_lat[i], (rl_lat_source)i, role[0]);
  g_lat_file = rl_lat_open(cfg->latency_path);
  if (g_lat_file == NULL) {
    fprintf(stderr, "latency: cannot open %s\n", cfg->latency_path);
    return false;
  }
  g_lat_on = true;
  g_lat_pending = 0;
  return true;
}

static void lat_flush(void) {
  if (g_lat_file != NULL) fflush(g_lat_file);
  g_lat_pending = 0;
}

static void lat_record(rl_lat_source src, uint64_t tick, uint64_t ns) {
  if (!g_lat_on) return;
  if (!rl_lat_record_to(&g_lat[src], g_lat_file, tick, ns)) return;
  if (++g_lat_pending >= LAT_FLUSH_EVERY) lat_flush();
}

/* Print and append one summary line per segment (journal spirit, stable keys). */
static void lat_finish(void) {
  if (!g_lat_on) return;
  for (unsigned i = 0; i < (unsigned)RL_LAT_SRC_COUNT; i++) {
    rl_lat_stats st;
    rl_lat_compute_stats(&g_lat[i], &st);
    if (st.empty) continue;
    char line[RL_LAT_SUMMARY_MAX];
    if (g_lat_file != NULL &&
        rl_lat_format_summary(&g_lat[i], &st, line, sizeof line) > 0) {
      fprintf(g_lat_file, "%s\n", line);
      printf("latency %-6s n=%-6llu p50=%8.3f us p99=%8.3f us p99.9=%8.3f us "
             "max=%9.3f us (%s)\n",
             rl_lat_source_name(g_lat[i].source), (unsigned long long)st.count,
             (double)st.p50_ns / 1000.0, (double)st.p99_ns / 1000.0,
             (double)st.p999_ns / 1000.0, (double)st.max_ns / 1000.0,
             st.exact ? "exact" : "estimated");
    }
  }
  lat_flush();
  if (g_lat_file != NULL) fclose(g_lat_file);
  g_lat_file = NULL;
}

/* CMD-09 hook used only while measuring: the timed law calls the very same
 * core function on the very same state as the built-in law, so the emitted
 * bits are unchanged — the measurement adds two clock reads, nothing else. */
static float law_step_timed(void *ctx, const relais_meas *m,
                            const relais_params *p) {
  relais_node_sm *s = (relais_node_sm *)ctx;
  uint64_t t0 = rl_lat_now_ns();
  float value = relais_ctl_step(&s->ctl, m, p);
  lat_record(RL_LAT_SRC_LAW, s->now, rl_lat_now_ns() - t0);
  return value;
}

static uint8_t law_export_timed(void *ctx, uint8_t *buf, size_t cap) {
  relais_node_sm *s = (relais_node_sm *)ctx;
  return relais_ctl_export(&s->ctl, buf, cap);
}

static bool law_import_timed(void *ctx, const uint8_t *buf, size_t len) {
  relais_node_sm *s = (relais_node_sm *)ctx;
  return relais_ctl_import(&s->ctl, buf, len);
}

static void lat_install_timed_law(relais_node_sm *sm) {
  relais_law law;
  memset(&law, 0, sizeof law);
  law.ctx = sm; /* the node's own control state: no second copy, no drift */
  law.init = NULL; /* relais_node_init already initialised sm->ctl */
  law.step = law_step_timed;
  law.export_state = law_export_timed;
  law.import_state = law_import_timed;
  relais_node_set_law(sm, &law);
}

/* ---- sockets ---- */

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
 * state and the follow-up go to the peer computer (spec 4.9). Returns the
 * monotonic instant the ORDRE hit the wire, 0 when no order was sent (M02). */
static uint64_t dispatch(const node_cfg_t *cfg, const relais_node_out *out) {
  uint64_t order_at = 0;
  for (uint8_t i = 0; i < out->count; i++) {
    const relais_msg *m = &out->out[i];
    bool to_peer = relais_msg_to_peer(m->type);
    send_to(cfg->sock, to_peer ? &cfg->peer : &cfg->actuator, m, &cfg->key);
    if (m->type == RELAIS_MSG_ORDRE && order_at == 0 && g_lat_on)
      order_at = rl_lat_now_ns();
  }
  return order_at;
}

/* 1 = a frame was decoded, 0 = the socket closed, -1 = timeout or error,
 * -2 = the frame failed CRC or MAC. `marks` may be NULL: no clock is read when
 * the measurement is off. */
static int recv_frame(const node_cfg_t *cfg, relais_msg *msg, uint32_t timeout_ms,
                      lat_marks *marks) {
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
  setsockopt(cfg->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  uint8_t buf[RELAIS_FRAME_MAX];
  ssize_t got = recv(cfg->sock, buf, sizeof buf, 0);
  if (got <= 0) return got == 0 ? 0 : -1;
  if (marks != NULL) marks->rx = rl_lat_now_ns();
  relais_reject why = RELAIS_REJ_NONE;
  if (!relais_frame_decode(buf, (size_t)got, &cfg->key, msg, &why)) return -2;
  if (marks != NULL) marks->decoded = rl_lat_now_ns();
  return 1;
}

static void report(const node_cfg_t *cfg, const relais_node_sm *sm, const char *what) {
  if (!cfg->verbose) return;
  char text[256];
  relais_node_status_text(sm, text, sizeof text);
  printf("node %s [%s] %s\n", relais_node_name(cfg->self), what, text);
  fflush(stdout);
}

/* ---- one event of the loop, kept short so main stays readable ---- */

static void handle_frame(const node_cfg_t *cfg, relais_node_sm *sm,
                         const relais_msg *msg, relais_node_out *out,
                         relais_tick *ticks) {
  if (msg->type == RELAIS_MSG_STATUT) {
    relais_node_on_status(sm, msg, out);
    *ticks = msg->tick;
    report(cfg, sm, "statut");
  } else if (msg->type == RELAIS_MSG_ETAT) {
    relais_node_on_etat(sm, msg, out);
    report(cfg, sm, "etat");
  } else if (msg->type == RELAIS_MSG_SUIVI) {
    relais_node_on_suivi(sm, msg, out);
  } else {
    relais_node_clear_out(out);
  }
}

/* The tick loop. Latency samples (M02) are taken around three segments:
 * decode (recv -> frame accepted), law (inside relais_ctl_step) and path
 * (recv -> ORDRE sent), the last one being PERF-01's critical path. */
static relais_tick run_loop(const node_cfg_t *cfg, relais_node_sm *sm) {
  const uint32_t period = relais_params_period_ms(&cfg->params);
  relais_tick ticks = 0;
  while (ticks < cfg->max_ticks) {
    relais_msg msg;
    lat_marks marks;
    memset(&marks, 0, sizeof marks);
    int got = recv_frame(cfg, &msg, period * 3, g_lat_on ? &marks : NULL);
    relais_node_out out;
    relais_node_clear_out(&out);
    if (got < 0) {
      /* No STATUT (or a forged one): the node deposes itself after the bail
       * (MAN-09, DET-04). */
      relais_node_on_silence(sm, sm->now + 1, &out);
      report(cfg, sm, "silence");
    } else if (got == 0) {
      continue;
    } else {
      /* Only the STATUT is timed: it is the frame that opens the critical
       * path. ETAT and SUIVI are off it (the holder sends both every tick). */
      if (g_lat_on && marks.decoded > marks.rx && msg.type == RELAIS_MSG_STATUT)
        lat_record(RL_LAT_SRC_DECODE, msg.tick, marks.decoded - marks.rx);
      handle_frame(cfg, sm, &msg, &out, &ticks);
    }
    uint64_t order_at = dispatch(cfg, &out);
    if (order_at != 0 && marks.rx != 0 && msg.type == RELAIS_MSG_STATUT)
      lat_record(RL_LAT_SRC_PATH, msg.tick, order_at - marks.rx);
    if (out.died) {
      printf("node %s: point de mort %d au tick %llu\n", relais_node_name(cfg->self),
             (int)out.at, (unsigned long long)ticks);
      fflush(stdout);
      lat_flush(); /* keep the samples of a machine that dies (INJ-07) */
      raise(SIGKILL); /* INJ-07: the machine dies exactly here */
      return ticks;
    }
  }
  return ticks;
}

static bool parse_args(int argc, char **argv, node_cfg_t *cfg, int *port,
                       const char **death) {
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--self") == 0 && i + 1 < argc) {
      const char *v = argv[++i];
      cfg->self = strcmp(v, "A") == 0 ? RELAIS_NODE_A : (strcmp(v, "B") == 0 ? RELAIS_NODE_B : RELAIS_NODE_NONE);
    } else if (strcmp(a, "--port") == 0 && i + 1 < argc) *port = atoi(argv[++i]);
    else if (strcmp(a, "--actuator") == 0 && i + 1 < argc) {
      if (!parse_addr(argv[++i], &cfg->actuator)) { usage(); return false; }
    } else if (strcmp(a, "--peer") == 0 && i + 1 < argc) {
      if (!parse_addr(argv[++i], &cfg->peer)) { usage(); return false; }
    } else if (strcmp(a, "--ticks") == 0 && i + 1 < argc) cfg->max_ticks = (uint32_t)atoi(argv[++i]);
    else if (strcmp(a, "--params") == 0 && i + 1 < argc) {
      char why[128] = "";
      if (!relais_params_parse(&cfg->params, argv[++i], why, sizeof why)) {
        fprintf(stderr, "parametres invalides: %s\n", why);
        return false;
      }
    } else if (strcmp(a, "--bias") == 0 && i + 1 < argc) cfg->bias = (float)atof(argv[++i]);
    else if (strcmp(a, "--die") == 0 && i + 1 < argc) *death = argv[++i];
    else if (strcmp(a, "--at") == 0 && i + 1 < argc) cfg->death_at = (relais_tick)strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--key") == 0 && i + 1 < argc) {
      if (!relais_key_from_hex(&cfg->key, argv[++i])) { usage(); return false; }
    } else if (strcmp(a, "--latency") == 0 && i + 1 < argc) cfg->latency_path = argv[++i];
    else if (strcmp(a, "--verbose") == 0) cfg->verbose = true;
    else { usage(); return false; }
  }
  return true;
}

static bool open_socket(const node_cfg_t *cfg, int port) {
  struct sockaddr_in local;
  memset(&local, 0, sizeof local);
  local.sin_family = AF_INET;
  /* INADDR_ANY, not loopback: the peer may be a virtual machine reached through
   * QEMU's user network, and slirp delivers on the host's own stack. */
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons((uint16_t)port);
  if (bind(cfg->sock, (struct sockaddr *)&local, sizeof local) != 0) {
    perror("bind");
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  node_cfg_t cfg;
  memset(&cfg, 0, sizeof cfg);
  relais_params_default(&cfg.params);
  relais_key_clear(&cfg.key);
  cfg.self = RELAIS_NODE_NONE;
  cfg.max_ticks = 4000;
  int port = 0;
  const char *death = NULL;
  if (!parse_args(argc, argv, &cfg, &port, &death)) return 2;
  if (!relais_node_is_computer(cfg.self) || port <= 0) { usage(); return 2; }

  cfg.sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (cfg.sock < 0) { perror("socket"); return 2; }
  if (!open_socket(&cfg, port)) return 2;
  if (!lat_setup(&cfg)) return 2;

  relais_node_sm sm;
  relais_node_init(&sm, &cfg.params, cfg.self);
  relais_node_set_bias(&sm, cfg.bias);
  if (g_lat_on) lat_install_timed_law(&sm);
  cfg.death_point = parse_death_point(death);
  if (cfg.death_point != RELAIS_DP_NONE) relais_node_arm_death(&sm, cfg.death_point, cfg.death_at);

  relais_tick ticks = run_loop(&cfg, &sm);

  char text[512];
  relais_node_status_text(&sm, text, sizeof text);
  printf("node %s: %llu ticks, %s\n", relais_node_name(cfg.self),
         (unsigned long long)ticks, text);
  lat_finish();
  close(cfg.sock);
  return 0;
}
