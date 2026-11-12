/* Relais — the adaptive control law (CMD-03, CMD-09, REP-01, mission M30).
 *
 *   generalisation  with flat gains the adaptive law IS the published law,
 *                   bit for bit, over 100 000 steps;
 *   CMD-03          two instances fed the same inputs give the same bits;
 *   REP-01/02       the replica round trips exactly, a bad one is refused,
 *                   and its size is published against the published law's;
 *   CMD-06          the amplitude and the slew bounds still hold;
 *   scheduling      the gains follow the speed inside their range, at a
 *                   bounded rate (the robustness guard);
 *   CMD-09          installed on the two machines, the law still produces
 *                   bit-identical orders and shadows, and the actuator still
 *                   certifies the second;
 *   sensitivity     the order/shadow gap when the replica is stale, which is
 *                   the protocol cost the mission asks to measure.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fixtures_control.h"
#include "relais/adaptive.h"
#include "test_util.h"

static void adaptive_random(relais_adaptive *a, relais_adaptive_params *ap, bool demo) {
  if (demo) relais_adaptive_params_demo(ap);
  else relais_adaptive_params_default(ap);
  relais_adaptive_init(a, ap);
}

/* ------------------------------------------- the published law, bit for bit */

static void test_flat_gains_equal_the_published_law(void) {
  t_section("M30 with flat gains the adaptive law is the published one, bit for bit");
  relais_params p;
  relais_params_default(&p);
  relais_adaptive_params ap;
  relais_adaptive a;
  adaptive_random(&a, &ap, false);
  relais_ctl_state plain;
  relais_ctl_init(&plain, &p);
  t_rng rng;
  t_rng_seed(&rng, 0xADA9u);
  long mismatches = 0;
  for (long i = 0; i < 100000; i++) {
    relais_meas m;
    meas_random(&m, &rng, 0.0f);
    float va = relais_adaptive_step(&a, &m, &p);
    float vb = relais_ctl_step(&plain, &m, &p);
    if (memcmp(&va, &vb, sizeof va) != 0) mismatches++;
  }
  T_EQ_U((uint64_t)mismatches, 0u);
  T_EQ_U(a.state.changes, 0u); /* flat gains never move */
  T_NEAR(a.state.kp, 60.0, 1e-9);
}

/* ----------------------------------------------------------------- CMD-03 */

static void test_adaptive_determinism(void) {
  t_section("M30 CMD-03 two adaptive instances, same inputs, same bits");
  relais_params p;
  relais_params_default(&p);
  relais_adaptive_params ap;
  relais_adaptive_params_demo(&ap);
  relais_adaptive a, b;
  relais_adaptive_init(&a, &ap);
  relais_adaptive_init(&b, &ap);
  t_rng rng;
  t_rng_seed(&rng, 0x5C4EDu);
  long mismatches = 0;
  for (long i = 0; i < 100000; i++) {
    relais_meas m;
    meas_random(&m, &rng, 0.0f);
    float va = relais_adaptive_step(&a, &m, &p);
    float vb = relais_adaptive_step(&b, &m, &p);
    if (memcmp(&va, &vb, sizeof va) != 0) mismatches++;
  }
  T_EQ_U((uint64_t)mismatches, 0u);
  T_CHECK(memcmp(&a, &b, sizeof a) == 0); /* the whole state, not only the value */
}

/* ------------------------------------------------------- REP-01 and REP-02 */

static void test_adaptive_replica(void) {
  t_section("M30 REP-01/REP-02 the adaptive replica round trips and is refused when bad");
  relais_params p;
  relais_params_default(&p);
  relais_adaptive_params ap;
  relais_adaptive_params_demo(&ap);
  relais_adaptive a, b;
  relais_adaptive_init(&a, &ap);
  relais_adaptive_init(&b, &ap);
  size_t size = relais_adaptive_state_size();
  T_CHECK(size <= RELAIS_CTL_STATE_MAX); /* REP-01 */
  t_rng rng;
  t_rng_seed(&rng, 0x8E91Cu);
  for (long i = 0; i < 5000; i++) {
    relais_meas m;
    meas_random(&m, &rng, 0.0f);
    relais_adaptive_step(&a, &m, &p);
  }
  uint8_t buf[RELAIS_CTL_STATE_MAX];
  uint8_t len = relais_adaptive_export(&a, buf, sizeof buf);
  T_EQ_U(len, size);
  T_CHECK(relais_adaptive_import(&b, buf, len));
  T_CHECK(memcmp(&a.state, &b.state, sizeof a.state) == 0);
  /* REP-02: an unknown version or a wrong length is refused. */
  uint8_t bad[RELAIS_CTL_STATE_MAX];
  memcpy(bad, buf, len);
  bad[0] = (uint8_t)(bad[0] + 1);
  T_CHECK(!relais_adaptive_import(&b, bad, len));
  memcpy(bad, buf, len);
  T_CHECK(!relais_adaptive_import(&b, bad, len - 1));
  T_CHECK(!relais_adaptive_import(&b, buf, len + 1));
  T_CHECK(relais_adaptive_export(&a, buf, 4) == 0); /* too small a buffer */
  /* The published size is measured, not assumed: the two laws are compared. */
  relais_ctl_state plain;
  relais_ctl_init(&plain, &p);
  uint8_t plain_buf[RELAIS_CTL_STATE_MAX];
  uint8_t plain_len = relais_ctl_export(&plain, plain_buf, sizeof plain_buf);
  T_CHECK(plain_len > 0);
  printf("  replica: published law %u bytes, adaptive law %u bytes (+%u, +%.0f %%)\n",
         (unsigned)plain_len, (unsigned)size, (unsigned)(size - plain_len),
         100.0 * ((double)size / (double)plain_len - 1.0));
}

/* ----------------------------------------------------------------- CMD-06 */

static void test_adaptive_bounds(void) {
  t_section("M30 CMD-06 the adaptive law respects the amplitude and slew bounds");
  relais_params p;
  relais_params_default(&p);
  relais_adaptive_params ap;
  relais_adaptive_params_demo(&ap);
  relais_adaptive a;
  relais_adaptive_init(&a, &ap);
  t_rng rng;
  t_rng_seed(&rng, 0xB0ADu);
  long amplitude = 0, slew = 0;
  float previous = 0.0f;
  for (long i = 0; i < 100000; i++) {
    relais_meas m;
    meas_random(&m, &rng, previous);
    float v = relais_adaptive_step(&a, &m, &p);
    if (fabsf(v) > p.max_amplitude + 1e-6f) amplitude++;
    if (fabsf(v - previous) > p.max_slew + 1e-6f) slew++;
    if (!isfinite(v)) amplitude++;
    previous = v;
  }
  T_EQ_U((uint64_t)amplitude, 0u);
  T_EQ_U((uint64_t)slew, 0u);
}

/* ------------------------------------------------------------- scheduling */

static void test_gain_scheduling_is_guarded(void) {
  t_section("M30 the gains follow the speed, inside their range, at a bounded rate");
  relais_params p;
  relais_params_default(&p);
  relais_adaptive_params ap;
  relais_adaptive_params_demo(&ap);
  relais_adaptive a;
  relais_adaptive_init(&a, &ap);
  t_rng rng;
  t_rng_seed(&rng, 0x5C4ED2u);
  long out_of_range = 0, over_slew = 0;
  float previous_kp = a.state.kp;
  float previous_kd = a.state.kd;
  for (long i = 0; i < 20000; i++) {
    relais_meas m;
    meas_random(&m, &rng, 0.0f);
    relais_adaptive_step(&a, &m, &p);
    if (a.state.kp < ap.kp_low - 1e-6f || a.state.kp > ap.kp_high + 1e-6f) out_of_range++;
    if (a.state.kd < ap.kd_low - 1e-6f || a.state.kd > ap.kd_high + 1e-6f) out_of_range++;
    /* 1e-3 covers the float rounding of gains of order 70, and still catches
     * a guard that would not be applied. */
    if (fabsf(a.state.kp - previous_kp) > ap.gain_slew + 1e-3f) over_slew++;
    if (fabsf(a.state.kd - previous_kd) > ap.gain_slew + 1e-3f) over_slew++;
    previous_kp = a.state.kp;
    previous_kd = a.state.kd;
  }
  T_EQ_U((uint64_t)out_of_range, 0u);
  T_EQ_U((uint64_t)over_slew, 0u);
  T_CHECK(a.state.changes > 0); /* the schedule really moved the gains */
  /* At rest the gain is the low one, at the reference speed it is the high one. */
  relais_adaptive rest;
  relais_adaptive_init(&rest, &ap);
  relais_meas m;
  meas_set(&m, 0.0f, 0.0f, 0.0f, 0.0f);
  relais_adaptive_step(&rest, &m, &p);
  T_NEAR(rest.state.kp, ap.kp_low, 1e-6);
  relais_adaptive fast;
  relais_adaptive_init(&fast, &ap);
  fast.state.kp = ap.kp_high; /* pretend the guard already got there */
  fast.state.kd = ap.kd_high;
  meas_set(&m, 0.0f, ap.vel_ref, 0.0f, 0.0f);
  relais_adaptive_step(&fast, &m, &p);
  T_NEAR(fast.state.kp, ap.kp_high, 1e-6);
}

/* ---------------------------------------------------- closed loop stability */

static void test_adaptive_closed_loop(void) {
  t_section("M30 the scheduled law closes the loop on the rack, 100 000 steps");
  relais_params p;
  relais_params_default(&p);
  relais_traj tr;
  relais_traj_default(&tr);
  const float dt = (float)relais_params_period_s(&p);
  relais_plant pl;
  relais_plant_init(&pl, 0.0f, 0u);
  relais_adaptive_params ap;
  relais_adaptive_params_demo(&ap);
  relais_adaptive a;
  relais_adaptive_init(&a, &ap);
  float previous = 0.0f;
  long non_finite = 0;
  float max_pos = 0.0f, max_err = 0.0f;
  for (long i = 0; i < 100000; i++) {
    double t = (double)i * (double)dt;
    relais_meas m;
    m.pos = pl.pos;
    m.vel = pl.vel;
    m.applied = previous;
    m.setpoint = relais_traj_setpoint(&tr, t + (double)dt);
    previous = relais_adaptive_step(&a, &m, &p);
    relais_plant_step(&pl, previous, 0.0f, dt);
    if (!isfinite(previous) || !isfinite(pl.pos) || !isfinite(pl.vel)) non_finite++;
    if (fabsf(pl.pos) > max_pos) max_pos = fabsf(pl.pos);
    if (t > 1.0 && fabsf(m.setpoint - pl.pos) > max_err) max_err = fabsf(m.setpoint - pl.pos);
  }
  T_EQ_U((uint64_t)non_finite, 0u);
  T_CHECK(max_pos < 2.0f);
  T_CHECK(max_err < 0.1f * tr.amp); /* the schedule keeps the tracking inside 10 % */
  printf("  scheduled law: max_err=%.6f (%.1f %% of the amplitude), gain changes=%u\n",
         (double)max_err, (double)(100.0f * max_err / tr.amp), (unsigned)a.state.changes);
}

/* ---------------------------------------------- the law inside the protocol */

typedef struct {
  bool certified;
  uint32_t certify_tick;
  double max_gap;
  double sum_gap;
  long compared;
  long bit_exact;
  long gap_ticks; /* ticks where the gap was above epsilon */
  size_t replica_len;
} protocol_stats;

static const relais_msg *find_msg(const relais_node_out *out, relais_msg_type type) {
  for (uint8_t i = 0; i < out->count; i++)
    if (out->out[i].type == type) return &out->out[i];
  return NULL;
}

/* The little world the protocol measurements run in: the two machines, the
 * actuator, the rack, and the ring that delays the state link. */
typedef struct {
  relais_params params;
  relais_traj traj;
  relais_plant pl;
  relais_arb arb;
  relais_node_sm nodes[2];
  relais_adaptive laws[2];
  relais_msg ring[8];
  bool ring_full[8];
} protocol_world;

static void protocol_world_init(protocol_world *w, bool use_adaptive) {
  memset(w, 0, sizeof *w);
  relais_params_default(&w->params);
  relais_traj_default(&w->traj);
  relais_plant_init(&w->pl, 0.0f, 0u);
  relais_arb_init(&w->arb, &w->params);
  relais_node_init(&w->nodes[0], &w->params, RELAIS_NODE_A);
  relais_node_init(&w->nodes[1], &w->params, RELAIS_NODE_B);
  if (!use_adaptive) return;
  relais_adaptive_params ap;
  relais_adaptive_params_demo(&ap);
  for (int i = 0; i < 2; i++) {
    /* One law context per machine: sharing one would make the two agree for
     * the wrong reason, and the replication under test would prove nothing. */
    relais_adaptive_init(&w->laws[i], &ap);
    relais_law vtable;
    relais_adaptive_law(&vtable, &w->laws[i]);
    relais_node_set_law(&w->nodes[i], &vtable);
  }
}

/* The actuator must hold a mandate before it can compare an order with a
 * shadow: certification is a property of the holder's neighbour. */
static void protocol_grant(protocol_world *w) {
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  relais_msg demande;
  t_demande(&demande, RELAIS_NODE_A, 0, 1, true, 0.0f, &w->params);
  relais_arb_out grant;
  relais_arb_step(&w->arb, 1, &demande, 1, &meas, &grant);
}

/* The order/shadow comparison of one tick: the bit-exactness of the
 * replication (CMD-03) and the width of the gap the actuator sees. */
static void protocol_measure(protocol_stats *st, const relais_node_out *holder,
                             const relais_node_out *second, float epsilon) {
  const relais_msg *order = find_msg(holder, RELAIS_MSG_ORDRE);
  const relais_msg *shadow = find_msg(second, RELAIS_MSG_OMBRE);
  if (order == NULL || shadow == NULL) return;
  st->compared++;
  double gap = fabs((double)order->value - (double)shadow->value);
  st->sum_gap += gap;
  if (gap > st->max_gap) st->max_gap = gap;
  if (memcmp(&order->value, &shadow->value, sizeof(float)) == 0) st->bit_exact++;
  if (gap > (double)epsilon) st->gap_ticks++;
}

static void protocol_tick(protocol_world *w, uint32_t tick, uint32_t delay,
                          protocol_stats *st) {
  const float dt = (float)relais_params_step_s(&w->params);
  relais_meas meas;
  meas.pos = w->pl.pos;
  meas.vel = w->pl.vel;
  meas.applied = 0.0f;
  meas.setpoint = relais_traj_setpoint(&w->traj, (double)tick * (double)dt);
  relais_msg statut;
  t_statut(&statut, tick, w->arb.mandate, RELAIS_NODE_A, RELAIS_SRC_HOLDER, 0, &meas);
  statut.holder = RELAIS_NODE_A;
  statut.certified = w->arb.certified;
  statut.certified_node = w->arb.certified_node;
  relais_node_out out_a, out_b;
  relais_node_on_status(&w->nodes[0], &statut, &out_a);
  /* The ETAT of the holder goes into a ring; the second gets the one that is
   * `delay` ticks old, which is how a slowed state link behaves. */
  const relais_msg *etat = find_msg(&out_a, RELAIS_MSG_ETAT);
  if (etat != NULL) {
    w->ring[tick % 8u] = *etat;
    w->ring_full[tick % 8u] = true;
    st->replica_len = etat->ctl_state_len;
  }
  if (tick > delay && w->ring_full[(tick - delay) % 8u]) {
    relais_node_out ignored;
    relais_node_on_etat(&w->nodes[1], &w->ring[(tick - delay) % 8u], &ignored);
  }
  relais_node_on_status(&w->nodes[1], &statut, &out_b);
  protocol_measure(st, &out_a, &out_b, w->params.epsilon);
  relais_msg in[2];
  size_t n = 0;
  const relais_msg *order = find_msg(&out_a, RELAIS_MSG_ORDRE);
  const relais_msg *shadow = find_msg(&out_b, RELAIS_MSG_OMBRE);
  if (order != NULL) in[n++] = *order;
  if (shadow != NULL) in[n++] = *shadow;
  relais_arb_out res;
  relais_arb_step(&w->arb, (relais_tick)tick + 1, in, n, &meas, &res);
  if (order != NULL) relais_plant_step(&w->pl, order->value, 0.0f, dt);
}

/* Drive the holder, the second and the arbiter, the ETAT being delivered
 * `delay` ticks late. One implementation serves the published law and the
 * adaptive one, and every staleness of the study (no duplicated harness). */
static void run_protocol(bool use_adaptive, uint32_t delay, uint32_t ticks,
                         protocol_stats *st) {
  protocol_world w;
  protocol_world_init(&w, use_adaptive);
  protocol_grant(&w);
  memset(st, 0, sizeof *st);
  for (uint32_t tick = 2; tick <= ticks; tick++) {
    protocol_tick(&w, tick, delay, st);
    if (w.arb.certified && st->certify_tick == 0) st->certify_tick = tick;
  }
  st->certified = w.arb.certified;
}

static void test_adaptive_law_in_the_protocol(void) {
  t_section("M30 CMD-09/REP-05 the adaptive law is still certified, bit for bit");
  protocol_stats plain, adaptive;
  run_protocol(false, 0, 300, &plain);
  run_protocol(true, 0, 300, &adaptive);
  T_CHECK(plain.certified);
  T_CHECK(adaptive.certified);
  T_EQ_U((uint64_t)adaptive.compared, (uint64_t)plain.compared);
  /* CMD-03: the replication holds at the bit, the certification is earned. */
  T_EQ_U((uint64_t)adaptive.bit_exact, (uint64_t)adaptive.compared);
  T_EQ_U((uint64_t)adaptive.gap_ticks, 0u);
  T_CHECK(adaptive.max_gap == 0.0);
  T_CHECK(adaptive.certify_tick > 0);
  T_CHECK(adaptive.replica_len > plain.replica_len); /* REP-01: a bigger replica */
  printf("  certified at tick %u (published law) and %u (adaptive law), %ld orders compared\n",
         (unsigned)plain.certify_tick, (unsigned)adaptive.certify_tick, adaptive.compared);
  printf("  replica: %u bytes published, %u bytes adaptive (+%.0f %%)\n",
         (unsigned)plain.replica_len, (unsigned)adaptive.replica_len,
         100.0 * ((double)adaptive.replica_len / (double)plain.replica_len - 1.0));
}

static void test_stale_replica_sensitivity(void) {
  t_section("M30 a stale replica widens the order/shadow gap, and the cost is measured");
  for (uint32_t delay = 1; delay <= 3; delay++) {
    protocol_stats plain, adaptive;
    run_protocol(false, delay, 300, &plain);
    run_protocol(true, delay, 300, &adaptive);
    T_CHECK(plain.max_gap > 0.0);    /* a stale replica is visible with both laws */
    T_CHECK(adaptive.max_gap > 0.0);
    double plain_mean = plain.sum_gap / (double)(plain.compared > 0 ? plain.compared : 1);
    double adaptive_mean = adaptive.sum_gap / (double)(adaptive.compared > 0 ? adaptive.compared : 1);
    printf("  ETAT %u tick(s) late: published mean gap %.6f, max %.6f, %ld/%ld ticks over "
           "epsilon | adaptive mean gap %.6f, max %.6f, %ld/%ld | certified %d/%d\n",
           (unsigned)delay, plain_mean, plain.max_gap, plain.gap_ticks, plain.compared,
           adaptive_mean, adaptive.max_gap, adaptive.gap_ticks, adaptive.compared,
           (int)plain.certified, (int)adaptive.certified);
    T_CHECK(adaptive_mean >= plain_mean * 0.5); /* same order, measured, not assumed */
  }
}

int main(void) {
  test_flat_gains_equal_the_published_law();
  test_adaptive_determinism();
  test_adaptive_replica();
  test_adaptive_bounds();
  test_gain_scheduling_is_guarded();
  test_adaptive_closed_loop();
  test_adaptive_law_in_the_protocol();
  test_stale_replica_sensitivity();
  return t_summary();
}
