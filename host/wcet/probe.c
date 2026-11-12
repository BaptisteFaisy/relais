/* Relais — portable WCET probe of the critical path (mission M40).
 *
 * WHAT IS MEASURED. One machine's critical path, in process, per tick:
 *
 *     STATUT bytes received -> ORDRE bytes emitted
 *       decode + CRC-32C/SipHash verify   (relais-net,  prio 62, rx)
 *       protocol state machine            (relais-core, prio 61)
 *         control law, timed separately   (relais-ctl,  prio 60)
 *       encode + MAC of every frame out   (relais-net,  prio 62, tx)
 *
 * The window is stamped with clock_gettime(CLOCK_MONOTONIC) around exactly that
 * code, which is the same code the QNX processes run (target/core.c calls
 * relais_node_on_status the same way). The socket syscalls and the kernel stack
 * are OUTSIDE the window: they are the part that changes most between Linux and
 * QNX, and they are measured by M02's socket-level campaign (host/measure/) and
 * by host/timing/procedures-qnx.md P1 on the target. Publishing the in-process
 * number is what makes two machines comparable: same C, same core, same window.
 *
 * The STATUT frames are produced by the REAL arbiter (core/src/arbiter.c) and
 * the real rack model, and the emitted orders are fed back to it, so the loop is
 * the nominal tick of spec 4.2 — not a synthetic call sequence. The actuator
 * side and the second computer run outside the measured window.
 *
 * PORTABILITY. C11 + POSIX clock_gettime only: no Linux header, no QNX header,
 * no socket, no thread. It builds with gcc on the PC and a Raspberry Pi and with
 * qcc -V gcc_ntox86_64 on QNX 8.0 from the same source, unmodified.
 *
 * OUTPUT. One JSON line per PATH sample, in the format host/timing/wcet.py
 * consumes ("latency_us"), plus a JSON summary with every segment's
 * n/min/mean/stdev/p50/p90/p99/p99.9/max, the clock-read overhead and the
 * environment the probe could establish (locked memory, real-time priority).
 *
 * Requirements: PERF-01, PERF-07, PERF-08, QNX-04, OBS-03, SEC-02, CMD-03.
 */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "relais/frame.h"
#include "relais/node.h"
#include "relais/params.h"
#include "wcet_stats.h"

#define PROBE_DEFAULT_SAMPLES 20000u
#define PROBE_DEFAULT_WARMUP 500u
#define PROBE_MAX_SAMPLES 4000000u
/* Key of the measured link (SEC-02): level C, SipHash-2-4 MAC. --no-mac falls
 * back to level S (CRC-32C), which the summary then reports. */
#define PROBE_KEY_HEX "00112233445566778899aabbccddeeff"

typedef struct {
  relais_params params;
  relais_key key;
  relais_arb arb;
  relais_plant plant;
  relais_traj traj;
  relais_node_sm holder;
  relais_node_sm second;
  relais_wcet_set seg[RELAIS_SEG_COUNT];
  relais_msg pending[RELAIS_IN_MSG_MAX];
  size_t pending_n;
  uint8_t statut_wire[RELAIS_FRAME_MAX];
  size_t statut_len;
  uint8_t tx[RELAIS_OUT_MSG_MAX][RELAIS_FRAME_MAX];
  unsigned long long orders;
  unsigned long long etats;
  unsigned long long shadows;
  unsigned long long non_holder;
  unsigned long long refused;
  bool locked;
  bool realtime;
} probe_t;

static probe_t g_probe;
static uint64_t g_law_ns; /* set by the timed law wrapper, read by the window */

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* CMD-09: the law may live in another process. Installing a wrapper that times
 * relais_ctl_step gives the relais-ctl stage its own number without touching
 * core/ and without changing what the node computes (same state, same bits). */
static float timed_law_step(void *ctx, const relais_meas *m, const relais_params *p) {
  relais_node_sm *s = (relais_node_sm *)ctx;
  uint64_t t0 = now_ns();
  float value = relais_ctl_step(&s->ctl, m, p);
  g_law_ns = now_ns() - t0;
  return value;
}

/* Two empty reads: the floor below which no segment is meaningful (PERF-07). */
static double clock_overhead_us(unsigned long long rounds) {
  uint64_t best = UINT64_MAX;
  for (unsigned long long i = 0; i < rounds; i++) {
    uint64_t t0 = now_ns();
    uint64_t t1 = now_ns();
    if (t1 - t0 < best) best = t1 - t0;
  }
  return (double)best / 1000.0;
}

static bool actuator_tick(probe_t *p, relais_tick tick) {
  relais_arb_out arb_out;
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  meas.pos = p->plant.pos;
  meas.vel = p->plant.vel;
  double step = relais_params_step_s(&p->params);
  meas.setpoint = relais_traj_setpoint(&p->traj, (double)(tick > 0 ? tick - 1 : 0) * step);
  relais_arb_step(&p->arb, tick, p->pending, p->pending_n, &meas, &arb_out);
  relais_plant_step(&p->plant, arb_out.value,
                    relais_traj_disturbance(&p->traj, (double)tick * step), (float)step);
  arb_out.statut.meas.pos = p->plant.pos;
  arb_out.statut.meas.vel = p->plant.vel;
  arb_out.statut.meas.applied = arb_out.value;
  p->statut_len = relais_frame_encode(&arb_out.statut, &p->key, p->statut_wire,
                                      sizeof p->statut_wire);
  p->pending_n = 0;
  return p->statut_len > 0;
}

static size_t encode_outputs(probe_t *p, const relais_node_out *out) {
  size_t total = 0;
  for (uint8_t i = 0; i < out->count; i++) {
    total += relais_frame_encode(&out->out[i], &p->key, p->tx[i], sizeof p->tx[i]);
  }
  return total;
}

static void feedback(probe_t *p, const relais_node_out *out) {
  for (uint8_t i = 0; i < out->count; i++) {
    const relais_msg *m = &out->out[i];
    if (relais_msg_to_peer(m->type)) continue;
    if (p->pending_n < RELAIS_IN_MSG_MAX) p->pending[p->pending_n++] = *m;
  }
}

static void record(probe_t *p, relais_wcet_seg seg, double us, bool on) {
  if (!on) return;
  if (!relais_wcet_add(&p->seg[seg], us)) p->refused++;
}

/* The measured window. `on` false = warm-up: the same code runs, the samples
 * are simply not kept, so warm-up and measurement are the same path.
 * seg_us receives the five segment durations in microseconds. */
static bool measure_holder(probe_t *p, bool on, relais_msg *statut,
                           relais_node_out *out, double *seg_us) {
  relais_reject why = RELAIS_REJ_NONE;
  uint64_t t0 = now_ns();
  bool ok = relais_frame_decode(p->statut_wire, p->statut_len, &p->key, statut, &why);
  uint64_t t1 = now_ns();
  if (!ok) return false;
  g_law_ns = 0;
  relais_node_clear_out(out);
  relais_node_on_status(&p->holder, statut, out);
  uint64_t t2 = now_ns();
  size_t bytes = encode_outputs(p, out);
  uint64_t t3 = now_ns();
  if (bytes == 0) return false;
  seg_us[RELAIS_SEG_PATH] = (double)(t3 - t0) / 1000.0;
  seg_us[RELAIS_SEG_DECODE] = (double)(t1 - t0) / 1000.0;
  seg_us[RELAIS_SEG_NODE] = (double)(t2 - t1) / 1000.0;
  seg_us[RELAIS_SEG_LAW] = (double)g_law_ns / 1000.0;
  seg_us[RELAIS_SEG_ENCODE] = (double)(t3 - t2) / 1000.0;
  for (int i = 0; i < RELAIS_SEG_COUNT; i++)
    record(p, (relais_wcet_seg)i, seg_us[i], on);
  return true;
}

static void count_outputs(probe_t *p, const relais_node_out *out, bool on) {
  if (!on) return;
  for (uint8_t i = 0; i < out->count; i++) {
    if (out->out[i].type == RELAIS_MSG_ORDRE) p->orders++;
    if (out->out[i].type == RELAIS_MSG_ETAT) p->etats++;
  }
  if (p->holder.role != RELAIS_ROLE_HOLDER) p->non_holder++;
}

/* The second computer, outside the window: it certifies the redundancy (REP-05)
 * so the measured holder runs the nominal configuration, lamp off. */
static void run_second(probe_t *p, const relais_msg *statut, const relais_node_out *held,
                       bool on) {
  relais_node_out ignored;
  for (uint8_t i = 0; i < held->count; i++) {
    if (held->out[i].type == RELAIS_MSG_ETAT)
      relais_node_on_etat(&p->second, &held->out[i], &ignored);
  }
  relais_node_out out;
  relais_node_clear_out(&out);
  relais_node_on_status(&p->second, statut, &out);
  for (uint8_t i = 0; i < out.count; i++) {
    if (out.out[i].type == RELAIS_MSG_SUIVI)
      relais_node_on_suivi(&p->holder, &out.out[i], &ignored);
    if (on && out.out[i].type == RELAIS_MSG_OMBRE) p->shadows++;
  }
  feedback(p, &out);
}

static void write_sample(FILE *f, const char *host, relais_tick tick, double us,
                         const char *segment) {
  if (f == NULL) return;
  fprintf(f,
          "{\"latency_us\":%.4f,\"tick\":%llu,\"segment\":\"%s\","
          "\"source\":\"statut_to_order\",\"role\":\"holder\",\"host\":\"%s\"}\n",
          us, (unsigned long long)tick, segment, host);
}

/* ---- configuration, environment, summary ------------------------------ */

typedef struct {
  unsigned long long samples;
  unsigned long long warmup;
  const char *out;     /* JSONL samples of one segment, or NULL */
  const char *summary; /* JSON summary path, or NULL for stdout */
  const char *host;    /* label of the machine, embedded in every line */
  const char *params;  /* "T=5,p=1,N=8" style override, or NULL */
  const char *segment; /* which segment the JSONL carries, default "path" */
  bool mac;            /* SEC-02: level C (MAC) or level S (CRC-32C) */
  bool lock;           /* try mlockall (QNX-03) */
  int prio;            /* try SCHED_FIFO at that priority, 0 = do not try */
  double burn;         /* --burn SECONDS: portable CPU interference */
  bool quiet;
} probe_cfg;

static void usage(void) {
  fprintf(stderr,
          "usage: relais-wcet-probe [--samples N] [--warmup N]\n"
          "       [--out samples.jsonl] [--segment path|decode|node|law|encode]\n"
          "       [--summary summary.json] [--host NAME] [--params T=5,p=1,N=8]\n"
          "       [--no-mac] [--lock] [--prio P] [--burn SECONDS] [--quiet]\n");
}

/* Best effort, never a lie: each step is recorded in the summary as done or
 * not done, with the reason left to errno. Nothing here needs root; when root
 * would help, host/wcet/isolate.sh says so instead of pretending. */
static void setup_env(probe_t *p, const probe_cfg *cfg) {
  if (cfg->lock) {
#ifdef MCL_FUTURE
    p->locked = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
    if (!p->locked) p->locked = mlockall(MCL_CURRENT) == 0;
#else
    p->locked = mlockall(MCL_CURRENT) == 0;
#endif
  }
  if (cfg->prio > 0) {
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = cfg->prio;
    p->realtime = sched_setscheduler(0, SCHED_FIFO, &sp) == 0;
  }
}

static bool init_probe(probe_t *p, const probe_cfg *cfg, char *why, size_t why_len) {
  memset(p, 0, sizeof *p);
  relais_params_default(&p->params);
  if (cfg->params != NULL &&
      !relais_params_parse(&p->params, cfg->params, why, why_len))
    return false;
  relais_key_clear(&p->key);
  if (cfg->mac && !relais_key_from_hex(&p->key, PROBE_KEY_HEX)) {
    snprintf(why, why_len, "bad built-in key");
    return false;
  }
  relais_arb_init(&p->arb, &p->params);
  relais_traj_default(&p->traj);
  relais_plant_init(&p->plant, 0.0f, p->traj.seed);
  relais_node_init(&p->holder, &p->params, RELAIS_NODE_A);
  relais_node_init(&p->second, &p->params, RELAIS_NODE_B);
  relais_law law;
  memset(&law, 0, sizeof law);
  law.ctx = &p->holder;
  law.step = timed_law_step; /* the same relais_ctl_step, timed (CMD-09) */
  relais_node_set_law(&p->holder, &law);
  for (int i = 0; i < RELAIS_SEG_COUNT; i++)
    if (!relais_wcet_set_init(&p->seg[i], (size_t)cfg->samples)) {
      snprintf(why, why_len, "cannot allocate %llu samples", cfg->samples);
      return false;
    }
  return true;
}

static int run_probe(probe_t *p, const probe_cfg *cfg, FILE *jsonl, int seg_index) {
  unsigned long long total = cfg->warmup + cfg->samples;
  const char *seg_name = relais_wcet_seg_name((relais_wcet_seg)seg_index);
  for (unsigned long long i = 0; i < total; i++) {
    relais_tick tick = (relais_tick)(i + 1);
    if (!actuator_tick(p, tick)) return 1;
    relais_msg statut;
    relais_node_out out;
    double seg_us[RELAIS_SEG_COUNT];
    bool on = i >= cfg->warmup;
    if (!measure_holder(p, on, &statut, &out, seg_us)) return 2;
    count_outputs(p, &out, on);
    if (on) write_sample(jsonl, cfg->host, tick, seg_us[seg_index], seg_name);
    /* The second computer joins once the mandate is held, so it never competes
     * for it: the measured machine stays the holder for the whole run. */
    if (p->holder.role == RELAIS_ROLE_HOLDER) run_second(p, &statut, &out, on);
    feedback(p, &out);
  }
  return 0;
}

static int seg_index_of(const char *name) {
  for (int i = 0; i < RELAIS_SEG_COUNT; i++)
    if (strcmp(name, relais_wcet_seg_name((relais_wcet_seg)i)) == 0) return i;
  return -1;
}

static void print_human(probe_t *p, const probe_cfg *cfg, double overhead_us) {
  printf("relais-wcet-probe host=%s samples=%llu warmup=%llu integrity=%s\n",
         cfg->host, cfg->samples, cfg->warmup, cfg->mac ? "mac(level C)" : "crc(level S)");
  printf("clock overhead (two reads) = %.4f us; orders=%llu etats=%llu shadows=%llu\n",
         overhead_us, p->orders, p->etats, p->shadows);
  for (int i = 0; i < RELAIS_SEG_COUNT; i++) {
    relais_wcet_desc d;
    if (!relais_wcet_describe(&p->seg[i], &d)) continue;
    printf("  %-7s n=%-8zu p50=%8.3f p90=%8.3f p99=%8.3f p99.9=%9.3f max=%10.3f sd=%7.3f us\n",
           relais_wcet_seg_name((relais_wcet_seg)i), d.n, d.p50, d.p90, d.p99, d.p99_9,
           d.max, d.stdev);
  }
}

static int write_summary(probe_t *p, const probe_cfg *cfg, double overhead_us) {
  FILE *f = cfg->summary != NULL ? fopen(cfg->summary, "w") : stdout;
  if (f == NULL) {
    perror("summary");
    return 1;
  }
  struct utsname u;
  memset(&u, 0, sizeof u);
  if (uname(&u) != 0) snprintf(u.sysname, sizeof u.sysname, "unknown");
  fprintf(f, "{\"tool\":\"relais-wcet-probe\",\"host\":\"%s\",\"kernel\":\"%s %s %s\",",
          cfg->host, u.sysname, u.release, u.machine);
  fprintf(f, "\"clock\":\"CLOCK_MONOTONIC\",\"integrity\":\"%s\",\"params\":\"T=%u,p=%u,N=%u\",",
          cfg->mac ? "mac" : "crc", p->params.period_ms, p->params.patience,
          p->params.testament_len);
  fprintf(f, "\"samples\":%llu,\"warmup\":%llu,\"orders\":%llu,\"etats\":%llu,",
          cfg->samples, cfg->warmup, p->orders, p->etats);
  fprintf(f, "\"shadows\":%llu,\"non_holder\":%llu,\"refused\":%llu,",
          p->shadows, p->non_holder, p->refused);
  fprintf(f, "\"clock_overhead_us\":%.4f,\"memory_locked\":%s,\"sched_fifo\":%s,",
          overhead_us, p->locked ? "true" : "false", p->realtime ? "true" : "false");
  fprintf(f, "\"segments\":{");
  for (int i = 0; i < RELAIS_SEG_COUNT; i++) {
    relais_wcet_desc d;
    char body[512];
    if (!relais_wcet_describe(&p->seg[i], &d)) continue;
    if (relais_wcet_desc_json(&d, body, sizeof body) < 0) continue;
    fprintf(f, "%s\"%s\":%s", i > 0 ? "," : "", relais_wcet_seg_name((relais_wcet_seg)i),
            body);
  }
  fprintf(f, "}}\n");
  if (f != stdout) fclose(f);
  return 0;
}

static int burn_cpu(double seconds) {
  uint64_t until = now_ns() + (uint64_t)(seconds * 1e9);
  volatile uint64_t x = 0;
  while (now_ns() < until) x++;
  return 0;
}

static bool parse_args(int argc, char **argv, probe_cfg *cfg) {
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--samples") == 0 && i + 1 < argc)
      cfg->samples = strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--warmup") == 0 && i + 1 < argc)
      cfg->warmup = strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--out") == 0 && i + 1 < argc) cfg->out = argv[++i];
    else if (strcmp(a, "--summary") == 0 && i + 1 < argc) cfg->summary = argv[++i];
    else if (strcmp(a, "--host") == 0 && i + 1 < argc) cfg->host = argv[++i];
    else if (strcmp(a, "--params") == 0 && i + 1 < argc) cfg->params = argv[++i];
    else if (strcmp(a, "--segment") == 0 && i + 1 < argc) cfg->segment = argv[++i];
    else if (strcmp(a, "--prio") == 0 && i + 1 < argc) cfg->prio = atoi(argv[++i]);
    else if (strcmp(a, "--burn") == 0 && i + 1 < argc) cfg->burn = atof(argv[++i]);
    else if (strcmp(a, "--no-mac") == 0) cfg->mac = false;
    else if (strcmp(a, "--lock") == 0) cfg->lock = true;
    else if (strcmp(a, "--quiet") == 0) cfg->quiet = true;
    else return false;
  }
  return cfg->samples > 0 && cfg->samples <= PROBE_MAX_SAMPLES;
}

static FILE *open_samples(const char *path) {
  if (path == NULL) return NULL;
  FILE *f = fopen(path, "w");
  if (f != NULL) setvbuf(f, NULL, _IOFBF, 1u << 18u);
  return f;
}

int main(int argc, char **argv) {
  probe_cfg cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.samples = PROBE_DEFAULT_SAMPLES;
  cfg.warmup = PROBE_DEFAULT_WARMUP;
  cfg.host = "pc";
  cfg.segment = "path";
  cfg.mac = true;
  if (!parse_args(argc, argv, &cfg)) { usage(); return 2; }
  if (cfg.burn > 0.0) return burn_cpu(cfg.burn);
  int seg_index = seg_index_of(cfg.segment);
  if (seg_index < 0) { usage(); return 2; }

  char why[128] = "";
  if (!init_probe(&g_probe, &cfg, why, sizeof why)) {
    fprintf(stderr, "probe: %s\n", why);
    return 2;
  }
  setup_env(&g_probe, &cfg);
  double overhead_us = clock_overhead_us(2000);
  FILE *jsonl = open_samples(cfg.out);
  if (cfg.out != NULL && jsonl == NULL) { perror(cfg.out); return 2; }
  int rc = run_probe(&g_probe, &cfg, jsonl, seg_index);
  if (jsonl != NULL) fclose(jsonl);
  if (rc == 0 && (g_probe.orders == 0 || g_probe.non_holder != 0)) {
    /* Honesty guard: a run that never held the mandate measures nothing. */
    fprintf(stderr, "probe: holder path not exercised (orders=%llu non_holder=%llu)\n",
            g_probe.orders, g_probe.non_holder);
    rc = 3;
  }
  if (rc == 0) {
    if (!cfg.quiet) print_human(&g_probe, &cfg, overhead_us);
    rc = write_summary(&g_probe, &cfg, overhead_us);
  }
  for (int i = 0; i < RELAIS_SEG_COUNT; i++) relais_wcet_set_free(&g_probe.seg[i]);
  return rc;
}
