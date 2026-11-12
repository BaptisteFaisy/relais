/* Relais — the physics bench of mission M30 (host/physics).
 *
 * Two measurements that the unit tests cannot make, because they need the
 * whole protocol pipeline and a sweep of parameters:
 *
 *   epsilon  the arbiter compares an order with a shadow inside a band of
 *            `epsilon` (REP-05, DET-09). When the two computers read their own
 *            position sensor, the noise of the two channels shows up in that
 *            comparison: too small an epsilon and the second is never
 *            certified, too large an epsilon and a value fault (INJ-10) hides
 *            inside the band. This mode runs the grid and prints one CSV row.
 *            The two architectures are measured: `independent` (each computer
 *            has its own sensor) and `shared` (the actuator publishes one
 *            measurement that both computers use, which is what the published
 *            software does).
 *
 *   phys     the tracking gap between the published rack model and the
 *            faithful one of core/src/plant_ext.c, phenomenon by phenomenon.
 *
 * The bench drives the very same core as the simulator (arbiter, two node
 * state machines, the rack, the control law): no re-implementation. It adds
 * only what the simulator does not have, the sensor noise of the two
 * channels, and it drops the link model (no loss, no delay) so that the
 * measurement is about epsilon and not about the channel.
 *
 * Usage:
 *   relais-physics --mode epsilon [--sigma S] [--epsilon E] [--bias B]
 *                  [--bias-at T] [--ticks N] [--seeds K]
 *                  [--noise independent|shared] [--header]
 *   relais-physics --mode phys [--ticks N] [--header]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relais/arbiter.h"
#include "relais/node.h"
#include "relais/plant_ext.h"

#define BENCH_MAX_PENDING 8
#define PHYS_CONFIGS 6

/* ------------------------------------------------------------- the world */

typedef struct {
  relais_params params;
  relais_traj traj;
  relais_plant plant;
  relais_arb arb;
  relais_node_sm nodes[2];
  relais_msg pending[BENCH_MAX_PENDING];
  size_t pending_count;
  uint64_t rng;
  double sigma;       /* position noise of one sensor channel, full scale */
  bool shared_noise;  /* one channel for both computers, or one each */
  relais_meas arb_ticks_meas; /* what the actuator published this tick */
  /* The order/shadow pair of the tick, read on the frames themselves. The
   * arbiter's have_order/have_shadow fields are declared but never written by
   * core/src/arbiter.c, so the bench does not rely on them (see the mission
   * report: the defect is described, not fixed out of scope). */
  float last_order;
  float last_shadow;
  bool have_pair;
} bench_t;

static uint64_t bench_rand(bench_t *b) {
  uint64_t x = b->rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  b->rng = x;
  return x * 0x2545f4914f6cdd1dull;
}

/* A normal-ish draw of mean 0 and standard deviation sigma: the sum of twelve
 * uniforms, which is deterministic and needs no libm beyond what we use. */
static double bench_gauss(bench_t *b, double sigma) {
  if (sigma <= 0.0) return 0.0;
  double sum = 0.0;
  for (int i = 0; i < 12; i++) sum += (double)(bench_rand(b) >> 11) / 9007199254740992.0;
  return (sum - 6.0) * sigma;
}

static float bench_setpoint(const bench_t *b, uint32_t tick) {
  double seconds = (double)(tick > 0 ? tick - 1 : 0) * relais_params_step_s(&b->params);
  return relais_traj_setpoint(&b->traj, seconds);
}

static void bench_init(bench_t *b, const relais_params *p, uint64_t seed, double sigma,
                       bool shared) {
  memset(b, 0, sizeof *b);
  b->params = *p;
  relais_traj_default(&b->traj);
  relais_arb_init(&b->arb, p);
  relais_plant_init(&b->plant, 0.0f, b->traj.seed);
  relais_node_init(&b->nodes[0], p, RELAIS_NODE_A);
  relais_node_init(&b->nodes[1], p, RELAIS_NODE_B);
  b->rng = seed != 0 ? seed : 0x9e3779b97f4a7c15ull;
  b->sigma = sigma;
  b->shared_noise = shared;
}

/* The two sensor channels of one tick. The velocity noise is four times the
 * position noise, because the rack gain is 4 position/s per unit of command:
 * a sensor that misreads the position by x misreads the speed by about 4x. */
static void bench_channels(bench_t *b, relais_meas *a, relais_meas *b_meas) {
  *a = b->arb_ticks_meas;
  *b_meas = b->arb_ticks_meas;
  if (!(b->sigma > 0.0)) return;
  if (b->shared_noise) {
    float np = (float)bench_gauss(b, b->sigma);
    float nv = (float)bench_gauss(b, b->sigma * 4.0);
    a->pos += np;
    b_meas->pos += np;
    a->vel += nv;
    b_meas->vel += nv;
    return;
  }
  a->pos += (float)bench_gauss(b, b->sigma);
  b_meas->pos += (float)bench_gauss(b, b->sigma);
  a->vel += (float)bench_gauss(b, b->sigma * 4.0);
  b_meas->vel += (float)bench_gauss(b, b->sigma * 4.0);
}

static void bench_queue(bench_t *b, const relais_msg *m) {
  if (b->pending_count < BENCH_MAX_PENDING) b->pending[b->pending_count++] = *m;
}

/* ETAT and SUIVI travel between the two computers inside the same tick, the
 * other frames go to the actuator for the next one. One function for both
 * directions, exactly as the simulator delivers them. */
static void bench_dispatch(bench_t *b, int from, int to, const relais_node_out *out) {
  (void)from;
  for (uint8_t i = 0; i < out->count; i++) {
    const relais_msg *m = &out->out[i];
    relais_node_out ignored;
    if (m->type == RELAIS_MSG_ETAT) relais_node_on_etat(&b->nodes[to], m, &ignored);
    else if (m->type == RELAIS_MSG_SUIVI) relais_node_on_suivi(&b->nodes[to], m, &ignored);
    else bench_queue(b, m);
  }
}

/* The order/shadow pair of the tick, read on the frames themselves. The
 * arbiter's have_order/have_shadow fields are declared but never written by
 * core/src/arbiter.c, so the bench does not rely on them (the defect is
 * described in the mission report, not fixed out of scope). */
static void bench_note_pair(bench_t *b, const relais_node_out *holder,
                            const relais_node_out *second) {
  b->have_pair = false;
  for (uint8_t i = 0; i < holder->count; i++)
    if (holder->out[i].type == RELAIS_MSG_ORDRE) b->last_order = holder->out[i].value;
  for (uint8_t i = 0; i < second->count; i++)
    if (second->out[i].type == RELAIS_MSG_OMBRE) {
      b->last_shadow = second->out[i].value;
      b->have_pair = true;
    }
}

/* One tick: the actuator decides on the frames of the previous tick, then the
 * holder computes, hands its state to the second inside the same tick, and the
 * second shadows it. This is the order of the simulator (spec 4.2, 4.8). */
static void bench_tick(bench_t *b, uint32_t tick, relais_arb_out *out) {
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  meas.pos = b->plant.pos;
  meas.vel = b->plant.vel;
  meas.setpoint = bench_setpoint(b, tick);
  relais_arb_step(&b->arb, (relais_tick)tick, b->pending, b->pending_count, &meas, out);
  b->pending_count = 0;
  float dt = (float)relais_params_step_s(&b->params);
  relais_plant_step(&b->plant, out->value,
                    relais_traj_disturbance(&b->traj, (double)tick * (double)dt), dt);
  out->statut.meas.pos = b->plant.pos;
  out->statut.meas.vel = b->plant.vel;
  out->statut.meas.applied = out->value;
  b->arb_ticks_meas = out->statut.meas;
  relais_meas chan[2];
  bench_channels(b, &chan[0], &chan[1]);
  int holder = out->statut.holder == RELAIS_NODE_B ? 1 : 0;
  int other = holder == 0 ? 1 : 0;
  relais_msg statut[2];
  relais_node_out outs[2];
  for (int i = 0; i < 2; i++) {
    statut[i] = out->statut;
    statut[i].meas = chan[i];
    relais_node_clear_out(&outs[i]);
  }
  relais_node_on_status(&b->nodes[holder], &statut[holder], &outs[holder]);
  bench_dispatch(b, holder, other, &outs[holder]);
  relais_node_on_status(&b->nodes[other], &statut[other], &outs[other]);
  bench_dispatch(b, other, holder, &outs[other]);
  bench_note_pair(b, &outs[holder], &outs[other]);
}

/* ---------------------------------------------------------- epsilon mode */

typedef struct {
  long runs;
  long certified_runs;   /* the second was certified at least once */
  long certified_final;  /* and still was at the end of the run */
  double certify_tick_sum;
  long detect_runs;
  double detect_delay_sum;
  double detect_delay_max;
  long false_alarm_runs; /* an alarm while no fault had been injected yet */
  double certified_frac; /* mean fraction of ticks where the second was certified */
  double gap_sum;        /* order/shadow gap, certified and fault-free ticks */
  double gap_max;
  long gap_over_epsilon;
  long compared;
  double startup_gap_max; /* worst gap before the first certification: the
                           * REP-04 rebuild transient, which is not noise */
} epsilon_result;

/* Optional trace on stderr, so stdout stays a clean CSV stream. */
static bool bench_trace = false;

static void epsilon_run(bench_t *b, const relais_params *p, uint32_t ticks, uint32_t bias_at,
                        float bias, epsilon_result *r) {
  relais_node_set_bias(&b->nodes[0], 0.0f);
  bool certified_seen = false;
  bool alarm_before_fault = false;
  double detect_delay = -1.0;
  long certified_ticks = 0;
  for (uint32_t tick = 1; tick <= ticks; tick++) {
    if (tick == bias_at) relais_node_set_bias(&b->nodes[0], bias); /* INJ-10 */
    relais_arb_out out;
    bench_tick(b, tick, &out);
    /* The gap is measured on the fault-free ticks only: after the injection
     * the gap IS the fault, and mixing the two would hide the noise floor.
     * The ticks before the first certification are kept apart: the second is
     * then running on a REP-04 rebuild, which is a transient and not noise. */
    if (b->have_pair && tick < bias_at) {
      double gap = fabs((double)b->last_order - (double)b->last_shadow);
      if (!certified_seen) {
        if (gap > r->startup_gap_max) r->startup_gap_max = gap;
      } else {
        r->gap_sum += gap;
        if (gap > r->gap_max) r->gap_max = gap;
        r->compared++;
        if (gap > (double)p->epsilon) r->gap_over_epsilon++;
      }
    }
    for (uint8_t e = 0; e < out.event_count; e++) {
      relais_event_kind kind = out.events[e].kind;
      bool alarm = kind == RELAIS_EV_DISAGREEMENT || kind == RELAIS_EV_DECERTIFIED;
      if (!alarm) continue;
      /* An alarm counts as a false one only once the second is certified:
       * before that the rebuild of REP-04 legitimately disagrees. */
      if (tick < bias_at && certified_seen) alarm_before_fault = true;
      else if (tick >= bias_at && detect_delay < 0.0) detect_delay = (double)(tick - bias_at);
    }
    if (bench_trace && (tick % 25u == 0 || tick < 40))
      fprintf(stderr, "tick=%u holder=%s mandate=%u misses=%u certified=%d node=%s "
                      "src=%s gap=%.6f\n", (unsigned)tick, relais_node_name(b->arb.holder),
              (unsigned)b->arb.mandate, (unsigned)b->arb.misses, (int)b->arb.certified,
              relais_node_name(b->arb.certified_node), relais_source_name(out.source),
              b->have_pair ? fabs((double)b->last_order - (double)b->last_shadow) : -1.0);
    if (b->arb.certified) certified_ticks++;
    if (b->arb.certified && !certified_seen) {
      certified_seen = true;
      r->certify_tick_sum += (double)tick;
    }
  }
  r->certified_frac += (double)certified_ticks / (double)(ticks > 0 ? ticks : 1);
  r->runs++;
  if (certified_seen) r->certified_runs++;
  if (b->arb.certified) r->certified_final++;
  if (alarm_before_fault) r->false_alarm_runs++;
  if (detect_delay >= 0.0) {
    r->detect_runs++;
    r->detect_delay_sum += detect_delay;
    if (detect_delay > r->detect_delay_max) r->detect_delay_max = detect_delay;
  }
}

/* ------------------------------------------------------------- phys mode */

typedef struct {
  const char *name;
  relais_phys_params params;
} phys_config;

/* The six configurations of the comparison: the published model, then each
 * phenomenon alone, then all of them together. */
static void phys_configs(phys_config *c) {
  relais_phys_params zero;
  relais_phys_params_default(&zero);
  relais_phys_params_demo(&c[5].params);
  c[5].name = "all";
  const char *names[5] = {"published", "friction", "play", "current", "stiffness"};
  for (int i = 0; i < 5; i++) {
    c[i].name = names[i];
    c[i].params = zero;
  }
  c[1].params.friction_static = c[5].params.friction_static;
  c[1].params.friction_kinetic = c[5].params.friction_kinetic;
  c[1].params.friction_viscous = c[5].params.friction_viscous;
  c[1].params.friction_velocity = c[5].params.friction_velocity;
  c[1].params.friction_band = c[5].params.friction_band;
  c[2].params.backlash = c[5].params.backlash;
  c[3].params.current_limit = c[5].params.current_limit;
  c[4].params.stiffness = c[5].params.stiffness;
}

/* One closed loop over the faithful rack; the published configuration is the
 * same call with every parameter at zero, so one driver serves all six. */
static void phys_run(const phys_config *cfg, const relais_params *p, const relais_traj *tr,
                     uint32_t ticks, double *max_err, double *rms_err, double *max_pos,
                     double *energy) {
  const float dt = (float)relais_params_step_s(p);
  relais_plant_ext pl;
  relais_plant_ext_init(&pl, 0.0f, &cfg->params);
  relais_ctl_state ctl;
  relais_ctl_init(&ctl, p);
  float previous = 0.0f;
  double se = 0.0;
  long counted = 0;
  *max_err = 0.0;
  *rms_err = 0.0;
  *max_pos = 0.0;
  for (uint32_t i = 0; i < ticks; i++) {
    double t = (double)i * (double)dt;
    relais_meas m;
    m.pos = pl.pos;
    m.vel = pl.vel;
    m.applied = previous;
    m.setpoint = relais_traj_setpoint(tr, t + (double)dt);
    previous = relais_ctl_step(&ctl, &m, p);
    relais_plant_ext_step(&pl, previous, relais_traj_disturbance(tr, t + (double)dt), dt);
    if ((double)i * (double)dt > 2.0) { /* settled */
      double err = fabs((double)m.setpoint - (double)pl.pos);
      se += err * err;
      counted++;
      if (err > *max_err) *max_err = err;
    }
    if (fabs((double)pl.pos) > *max_pos) *max_pos = fabs((double)pl.pos);
  }
  *rms_err = counted > 0 ? sqrt(se / (double)counted) : 0.0;
  *energy = (double)pl.energy / (double)(ticks > 0 ? ticks : 1);
}

static int mode_phys(uint32_t ticks, bool header, const char *traj_spec) {
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  char why[128];
  if (traj_spec[0] != '\0' && !relais_traj_parse(&tr, traj_spec, why, sizeof why)) {
    fprintf(stderr, "invalid trajectory: %s\n", why);
    return 2;
  }
  phys_config cfg[PHYS_CONFIGS];
  phys_configs(cfg);
  if (header)
    printf("trajectory,config,enabled,max_err,rms_err,max_pos,mean_kinetic_energy\n");
  for (int i = 0; i < PHYS_CONFIGS; i++) {
    double max_err, rms_err, max_pos, energy;
    phys_run(&cfg[i], &p, &tr, ticks, &max_err, &rms_err, &max_pos, &energy);
    printf("%s,%s,%d,%.6f,%.6f,%.6f,%.6f\n", traj_spec[0] != '\0' ? traj_spec : "default",
           cfg[i].name, (int)relais_phys_params_enabled(&cfg[i].params), max_err, rms_err,
           max_pos, energy);
  }
  return 0;
}

/* ------------------------------------------------------------------- main */

static double arg_double(int argc, char **argv, const char *key, double fallback) {
  for (int i = 1; i + 1 < argc; i++)
    if (strcmp(argv[i], key) == 0) return strtod(argv[i + 1], NULL);
  return fallback;
}

static uint32_t arg_u32(int argc, char **argv, const char *key, uint32_t fallback) {
  for (int i = 1; i + 1 < argc; i++)
    if (strcmp(argv[i], key) == 0) return (uint32_t)strtoul(argv[i + 1], NULL, 10);
  return fallback;
}

static const char *arg_str(int argc, char **argv, const char *key, const char *fallback) {
  for (int i = 1; i + 1 < argc; i++)
    if (strcmp(argv[i], key) == 0) return argv[i + 1];
  return fallback;
}

static int mode_epsilon(int argc, char **argv) {
  relais_params p;
  relais_params_default(&p);
  double sigma = arg_double(argc, argv, "--sigma", 0.0);
  double epsilon = arg_double(argc, argv, "--epsilon", (double)p.epsilon);
  double bias = arg_double(argc, argv, "--bias", 0.0);
  uint32_t bias_at = arg_u32(argc, argv, "--bias-at", 1000);
  uint32_t ticks = arg_u32(argc, argv, "--ticks", 3000);
  uint32_t seeds = arg_u32(argc, argv, "--seeds", 8);
  bool shared = strcmp(arg_str(argc, argv, "--noise", "independent"), "shared") == 0;
  bool header = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--header") == 0) header = true;
    if (strcmp(argv[i], "--trace") == 0) bench_trace = true;
  }
  p.epsilon = (float)epsilon;
  char why[128];
  if (!relais_params_valid(&p, why, sizeof why)) {
    fprintf(stderr, "invalid parameters: %s\n", why);
    return 2;
  }
  epsilon_result r;
  memset(&r, 0, sizeof r);
  uint64_t seed_base = (uint64_t)arg_double(argc, argv, "--seed-base", 1000.0);
  for (uint32_t s = 0; s < seeds; s++) {
    bench_t b;
    bench_init(&b, &p, seed_base + (uint64_t)s * 7919ull, sigma, shared);
    epsilon_run(&b, &p, ticks, bias_at, (float)bias, &r);
  }
  if (header)
    printf("noise,sigma,epsilon,bias,bias_at,seeds,certified_rate,certified_final_rate,"
           "certify_tick_mean,certified_fraction,detect_rate,detect_delay_mean,"
           "detect_delay_max,false_alarm_rate,gap_mean,gap_max,gap_over_epsilon_rate,"
           "startup_gap_max\n");
  double runs = (double)r.runs;
  printf("%s,%.6f,%.6f,%.6f,%u,%u,%.4f,%.4f,%.2f,%.4f,%.4f,%.3f,%.1f,%.4f,%.6f,%.6f,%.4f,"
         "%.6f\n",
         shared ? "shared" : "independent", sigma, epsilon, bias, (unsigned)bias_at,
         (unsigned)seeds, (double)r.certified_runs / runs, (double)r.certified_final / runs,
         r.certified_runs > 0 ? r.certify_tick_sum / (double)r.certified_runs : 0.0,
         r.certified_frac / runs, bias > 0.0 ? (double)r.detect_runs / runs : 1.0,
         r.detect_runs > 0 ? r.detect_delay_sum / (double)r.detect_runs : 0.0,
         r.detect_delay_max, (double)r.false_alarm_runs / runs,
         r.compared > 0 ? r.gap_sum / (double)r.compared : 0.0, r.gap_max,
         r.compared > 0 ? (double)r.gap_over_epsilon / (double)r.compared : 0.0,
         r.startup_gap_max);
  return 0;
}

int main(int argc, char **argv) {
  const char *mode = arg_str(argc, argv, "--mode", "epsilon");
  uint32_t ticks = arg_u32(argc, argv, "--ticks", 3000);
  bool header = false;
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "--header") == 0) header = true;
  if (strcmp(mode, "phys") == 0)
    return mode_phys(ticks, header, arg_str(argc, argv, "--trajectory", ""));
  if (strcmp(mode, "epsilon") == 0) return mode_epsilon(argc, argv);
  fprintf(stderr, "unknown mode: %s\n", mode);
  return 2;
}
