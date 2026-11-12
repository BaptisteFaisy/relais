/* Relais — the UDP transport, checked for real (mission M32, idea 75).
 *
 * Two proofs, both on this machine, no simulation:
 *
 *  1. loopback: two relais_udp endpoints on 127.0.0.1 exchange encoded Relais
 *     frames and measure what the medium costs — round-trip latency and
 *     throughput, with the numbers printed;
 *  2. interoperability: the UNMODIFIED build/relais-node binary is started as a
 *     child process and driven by this harness acting as the actuator through
 *     the new transport. If the extraction behind the interface had changed one
 *     byte on the wire, the node would not answer, or the arbiter would reject
 *     its orders. This is the "refactor changes nothing" test.
 *
 * Requirement IDs: SEC-05, SEC-02, DET-04, MAN-02, CMD-02, PERF-01, ROB-02.
 *
 * Usage: ./build/transport-udp-check [--ticks 300] [--act-port 7930]
 *                                    [--node-port 7931] [--peer-port 7932]
 *                                    [--node ./build/relais-node] [--rounds 2000]
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "udp.h"

#define KEY_HEX "0f1e2d3c4b5a69788796a5b4c3d2e1f0"

static int g_failures;

static void check(bool ok, const char *what) {
  if (ok) return;
  g_failures++;
  printf("  FAIL %s\n", what);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct {
  int act_port, node_port, peer_port, ticks, rounds;
  const char *node_bin;
} opts_t;

/* A complete order, testament included: the biggest frame a computer sends
 * every tick, so the latency measured is the worst case, not a small one. */
static void make_order(relais_msg *m, const relais_params *p, relais_node from,
                       relais_tick tick, relais_mandate mandate, float value) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ORDRE;
  m->sender = from;
  m->tick = tick;
  m->mandate = mandate;
  m->value = value;
  m->has_order = true;
  relais_ctl_state st;
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  relais_ctl_init(&st, p);
  relais_testament_fill(&st, &meas, p, m->testament, &m->testament_len);
}

/* ---------------------------------------------------- 1. loopback latency -- */

static void report_latency(double *rtt, int n, uint32_t bytes_each_way) {
  qsort(rtt, (size_t)n, sizeof rtt[0], cmp_double);
  double sum = 0.0;
  for (int i = 0; i < n; i++) sum += rtt[i];
  printf("  loopback round trip over %d frames of %u bytes: min %.3f ms, median %.3f ms,"
         " p99 %.3f ms, max %.3f ms, mean %.3f ms\n",
         n, (unsigned)bytes_each_way, rtt[0], rtt[n / 2], rtt[(int)((double)n * 0.99)],
         rtt[n - 1], sum / (double)n);
  printf("  throughput: %.2f MB/s each way (payload only, no UDP/IP headers)\n",
         (double)bytes_each_way * (double)n / (sum / 1000.0) / 1e6);
}

/* Bind two endpoints to each other on loopback: A talks to B and B talks back. */
static bool open_pair(const opts_t *o, relais_transport *a, relais_transport *b) {
  char spec_a[64], spec_b[64], spec_act[64], spec_peer[64];
  snprintf(spec_a, sizeof spec_a, "127.0.0.1:%d", o->act_port);
  snprintf(spec_b, sizeof spec_b, "127.0.0.1:%d", o->peer_port);
  snprintf(spec_act, sizeof spec_act, ":%d", o->act_port);
  snprintf(spec_peer, sizeof spec_peer, ":%d", o->peer_port);
  relais_tr_cfg cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.actuator = spec_b;
  cfg.local = spec_act;
  cfg.timeout_ms = 500;
  if (relais_tr_init(a, &cfg) != RELAIS_TR_OK) return false;
  memset(&cfg, 0, sizeof cfg);
  cfg.actuator = spec_a;
  cfg.local = spec_peer;
  cfg.timeout_ms = 500;
  return relais_tr_init(b, &cfg) == RELAIS_TR_OK;
}

/* One frame out, the same frame back: how long the medium takes, and whether it
 * altered a single byte on the way. */
static int measure_roundtrips(relais_transport *a, relais_transport *b, const uint8_t *scratch,
                              size_t len, int rounds, double *rtt) {
  uint8_t back[RELAIS_FRAME_MAX];
  int done = 0;
  for (int i = 0; i < rounds; i++) {
    double start = now_ms();
    if (relais_tr_send_raw(a, RELAIS_LINK_ACTUATOR, scratch, len) != RELAIS_TR_OK) break;
    size_t got = 0;
    if (relais_tr_recv_raw(b, NULL, back, sizeof back, &got) != RELAIS_TR_OK) break;
    if (got != len || memcmp(back, scratch, len) != 0) break;
    if (relais_tr_send_raw(b, RELAIS_LINK_ACTUATOR, back, got) != RELAIS_TR_OK) break;
    got = 0;
    if (relais_tr_recv_raw(a, NULL, back, sizeof back, &got) != RELAIS_TR_OK) break;
    if (got != len) break;
    rtt[done++] = now_ms() - start;
  }
  return done;
}

static void phase_loopback(const opts_t *o) {
  printf("== 1. two UDP transports on 127.0.0.1 ==\n");
  relais_udp ua, ub;
  relais_transport a, b;
  relais_udp_attach(&a, &ua);
  relais_udp_attach(&b, &ub);
  if (!open_pair(o, &a, &b)) {
    check(false, "both endpoints bind on loopback");
    return;
  }
  relais_key key;
  relais_key_from_hex(&key, KEY_HEX);
  relais_params p;
  relais_params_default(&p);
  relais_msg m;
  make_order(&m, &p, RELAIS_NODE_A, 1, 1, 0.25f);
  uint8_t scratch[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(&m, &key, scratch, sizeof scratch);
  check(len > 0, "a frame encodes");

  double *rtt = calloc((size_t)o->rounds, sizeof *rtt);
  check(rtt != NULL, "latency buffer");
  if (rtt == NULL) return;
  double t0 = now_ms();
  int done = measure_roundtrips(&a, &b, scratch, len, o->rounds, rtt);
  double elapsed = now_ms() - t0;
  check(done == o->rounds, "every frame came back identical");
  printf("  %d/%d frames made the round trip, byte for byte, in %.0f ms (%u sent, %u received,"
         " %u timeouts, %u send errors)\n",
         done, o->rounds, elapsed, ua.sent + ub.sent, ua.received + ub.received,
         ua.timeouts + ub.timeouts, ua.send_errors + ub.send_errors);
  if (done > 0) report_latency(rtt, done, (uint32_t)len);
  free(rtt);
  relais_tr_close(&a);
  relais_tr_close(&b);
}

/* ------------------------- 2. the untouched relais-node, driven over UDP ---- */

typedef struct {
  pid_t pid;
  relais_udp act, peer;
  relais_transport tact, tpeer;
  relais_arb arb;
  relais_key key;
  relais_params params;
  uint32_t holder_ticks, statut_sent, ordre_seen, etat_seen, suivi_seen, rejected;
} interop_t;

static bool start_node(interop_t *it, const opts_t *o) {
  char actuator[64], peer[64], port[16], ticks[16];
  snprintf(actuator, sizeof actuator, "127.0.0.1:%d", o->act_port);
  snprintf(peer, sizeof peer, "127.0.0.1:%d", o->peer_port);
  snprintf(port, sizeof port, "%d", o->node_port);
  snprintf(ticks, sizeof ticks, "%d", o->ticks + 50);
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execl(o->node_bin, "relais-node", "--self", "A", "--port", port, "--actuator",
          actuator, "--peer", peer, "--ticks", ticks, "--key", KEY_HEX, (char *)NULL);
    _exit(127);
  }
  it->pid = pid;
  return true;
}

static bool open_endpoints(interop_t *it, const opts_t *o) {
  relais_udp_attach(&it->tact, &it->act);
  relais_udp_attach(&it->tpeer, &it->peer);
  char local_a[64], local_b[64], node_spec[64];
  snprintf(local_a, sizeof local_a, "127.0.0.1:%d", o->act_port);
  snprintf(local_b, sizeof local_b, "127.0.0.1:%d", o->peer_port);
  snprintf(node_spec, sizeof node_spec, "127.0.0.1:%d", o->node_port);
  relais_tr_cfg cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.actuator = node_spec; /* the actuator's link 0 is computer A */
  cfg.peer = node_spec;
  cfg.local = local_a;
  cfg.timeout_ms = 0; /* the actuator never blocks: it owns the tempo */
  if (relais_tr_init(&it->tact, &cfg) != RELAIS_TR_OK) return false;
  memset(&cfg, 0, sizeof cfg);
  cfg.actuator = node_spec;
  cfg.local = local_b;
  cfg.timeout_ms = 0; /* non-blocking: the passive peer only drains what A sent */
  return relais_tr_init(&it->tpeer, &cfg) == RELAIS_TR_OK;
}

/* One tick of the actuator: drain what the node sent, arbitrate, publish.
 * Returns the single value applied at this tick (spec 4.4). */
static float interop_tick(interop_t *it, relais_tick tick, const relais_meas *meas) {
  relais_msg msgs[RELAIS_IN_MSG_MAX];
  size_t n = 0;
  uint8_t buf[RELAIS_FRAME_MAX];
  while (n < RELAIS_IN_MSG_MAX) {
    relais_msg m;
    relais_reject why = RELAIS_REJ_NONE;
    if (relais_tr_recv_msg(&it->tact, &it->key, NULL, &m, &why, buf, sizeof buf) != RELAIS_TR_OK)
      break;
    if (m.type == RELAIS_MSG_ORDRE) it->ordre_seen++;
    msgs[n++] = m;
  }
  relais_arb_out out;
  relais_arb_step(&it->arb, tick, msgs, n, meas, &out);
  if (out.source == RELAIS_SRC_HOLDER) it->holder_ticks++;
  it->statut_sent++;
  uint8_t scratch[RELAIS_FRAME_MAX];
  relais_tr_send_msg(&it->tact, &it->key, &out.statut, scratch, sizeof scratch);
  return out.value;
}

/* The other computer of the pair, reduced to a drain: it proves that ETAT and
 * SUIVI really leave the node on the peer link, and that no ICMP rejection
 * comes back to disturb it. */
static void drain_peer(interop_t *it) {
  uint8_t buf[RELAIS_FRAME_MAX];
  for (int guard = 0; guard < 64; guard++) {
    relais_msg m;
    relais_reject why = RELAIS_REJ_NONE;
    if (relais_tr_recv_msg(&it->tpeer, &it->key, NULL, &m, &why, buf, sizeof buf) != RELAIS_TR_OK)
      return;
    if (m.type == RELAIS_MSG_ETAT) it->etat_seen++;
    else if (m.type == RELAIS_MSG_SUIVI) it->suivi_seen++;
  }
}

/* The tick loop of the harness: publish the tempo, read what the node answers,
 * move the rack model with the value the arbiter applied. */
static void run_ticks(interop_t *it, const opts_t *o, uint32_t period) {
  relais_traj traj;
  relais_traj_default(&traj);
  relais_plant plant;
  relais_plant_init(&plant, 0.0f, traj.seed);
  const double dt = relais_params_step_s(&it->params);
  for (relais_tick tick = 1; tick <= (relais_tick)o->ticks; tick++) {
    relais_meas meas;
    memset(&meas, 0, sizeof meas);
    meas.pos = plant.pos;
    meas.vel = plant.vel;
    meas.setpoint = relais_traj_setpoint(&traj, (double)(tick - 1) * dt);
    float applied = interop_tick(it, tick, &meas);
    relais_plant_step(&plant, applied, relais_traj_disturbance(&traj, (double)tick * dt),
                      (float)dt);
    drain_peer(it);
    struct timespec nap = {0, (long)(period * 250000u)};
    nanosleep(&nap, NULL);
  }
}

static void phase_interop(const opts_t *o) {
  printf("== 2. the unmodified %s, driven by the transport ==\n", o->node_bin);
  static interop_t it;
  memset(&it, 0, sizeof it);
  relais_key_from_hex(&it.key, KEY_HEX);
  relais_params_default(&it.params);
  relais_arb_init(&it.arb, &it.params);
  if (!open_endpoints(&it, o)) {
    check(false, "the harness binds its two sockets");
    return;
  }
  if (!start_node(&it, o)) {
    check(false, "the node process starts");
    return;
  }
  const uint32_t period = relais_params_period_ms(&it.params);
  double t0 = now_ms();
  run_ticks(&it, o, period);
  double elapsed = now_ms() - t0;
  kill(it.pid, SIGTERM);
  int status = 0;
  waitpid(it.pid, &status, 0);

  uint32_t forged = it.arb.forged;
  uint32_t rejected = relais_arb_rejected_total(&it.arb);
  printf("  %d ticks in %.0f ms (%.2f ms per tick, period %u ms): %u STATUT sent,"
         " %u ORDRE received, %u ETAT and %u SUIVI seen by the peer\n",
         o->ticks, elapsed, elapsed / (double)o->ticks, period, (unsigned)it.statut_sent,
         (unsigned)it.ordre_seen, (unsigned)it.etat_seen, (unsigned)it.suivi_seen);
  printf("  arbiter: %u/%d ticks applied the holder's order, mandate %u, holder %s,"
         " %u rejected, %u forged, UDP %u sent / %u received / %u timeouts\n",
         (unsigned)it.holder_ticks, o->ticks, (unsigned)it.arb.mandate,
         relais_node_name(it.arb.holder), (unsigned)rejected, (unsigned)forged,
         (unsigned)it.act.sent, (unsigned)it.act.received, (unsigned)it.act.timeouts);
  check(it.ordre_seen > 0, "the node answered over the new transport");
  check(it.etat_seen > 0, "the replicated state reached the peer link");
  check(forged == 0, "no frame failed the MAC");
  /* The node needs a few ticks to be granted the mandate, then it must hold it:
   * at least 80 % of the run applies the holder's order. */
  check(it.holder_ticks * 10u >= (uint32_t)o->ticks * 8u, "the holder's orders were applied");
  relais_tr_close(&it.tact);
  relais_tr_close(&it.tpeer);
}

static void parse_args(int argc, char **argv, opts_t *o) {
  o->act_port = 7930;
  o->node_port = 7931;
  o->peer_port = 7932;
  o->ticks = 300;
  o->rounds = 2000;
  o->node_bin = "build/relais-node";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--ticks") && i + 1 < argc) o->ticks = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--rounds") && i + 1 < argc) o->rounds = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--act-port") && i + 1 < argc) o->act_port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--node-port") && i + 1 < argc) o->node_port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--peer-port") && i + 1 < argc) o->peer_port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--node") && i + 1 < argc) o->node_bin = argv[++i];
    else fprintf(stderr, "unknown argument: %s\n", argv[i]);
  }
}

int main(int argc, char **argv) {
  opts_t o;
  parse_args(argc, argv, &o);
  phase_loopback(&o);
  phase_interop(&o);
  if (g_failures == 0) {
    printf("UDP-TRANSPORT-OK\n");
    return 0;
  }
  printf("UDP-TRANSPORT-FAILED %d check(s)\n", g_failures);
  return 1;
}
