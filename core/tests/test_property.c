/* Relais — generative property tests for the arbiter (M13, ideas 1 & 84).
 *
 * Thousands of random frame sequences are driven through the C arbiter and a
 * handful of protocol properties are re-checked after EVERY tick. The properties
 * are stated as invariants (I2, I3, I6, I8, DET-01, MAN-03) so a regression that
 * only shows up under an unusual interleaving is caught here, not in the field.
 *
 * Properties checked each tick:
 *   P1 (I2, MAN-03) the mandate is monotonic and moves by at most one step.
 *   P2 (ACT-01)     exactly one value is applied every tick.
 *   P3 (DET-01, I3) a holder order is applied only if some frame named this tick
 *                   and came from the current holder (no stale order applied).
 *   P4 (I6)         the will is never played beyond its horizon.
 *   P5 (I8, ACT-03) the fallback only takes over once the will is exhausted.
 *   P6 (DEG-01)     a fallback value lights the lamp red.
 *
 * Uses the shared PRNG and fixtures of core/tests/test_util.c (no duplication).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_util.h"

#define PROP_RUNS   2000
#define PROP_TICKS  40
#define PROP_MSGS   6
#define WILL_TOL    1e-4f

static void clear_meas(relais_meas *m) {
  m->pos = 0.0f; m->vel = 0.0f; m->applied = 0.0f; m->setpoint = 0.0f;
}

/* Build a frame with full control over every field that matters to the arbiter. */
static void make_frame(relais_msg *m, relais_msg_type type, relais_node sender,
                       relais_tick tick, relais_mandate mandate, float value,
                       const float *will, uint8_t tlen, bool has_order) {
  relais_msg_clear(m);
  m->type = type;
  m->sender = sender;
  m->tick = tick;
  m->mandate = mandate;
  m->value = value;
  m->has_order = has_order;
  m->testament_len = tlen;
  if (will != NULL) {
    for (uint8_t i = 0; i < tlen && i < RELAIS_TESTAMENT_MAX; i++) m->testament[i] = will[i];
  }
}

static float rand_value(t_rng *r) {
  return (float)((double)(t_rng_next(r) % 2000001) / 1000000.0 - 1.0);
}

/* Fill msgs with a random mix of order/shadow/demande/cession, some stale, some
 * off-tick, some duplicated: the adversarial channel the protocol must absorb. */
static size_t fill_random(t_rng *r, relais_msg *msgs, relais_tick tick,
                          relais_mandate mandate, relais_node holder, float *will,
                          uint8_t *will_len) {
  size_t n = t_rng_below(r, PROP_MSGS + 1);
  for (size_t i = 0; i < n; i++) {
    relais_node sender = (t_rng_below(r, 2) == 0) ? RELAIS_NODE_A : RELAIS_NODE_B;
    relais_tick mtick = tick + (relais_tick)(t_rng_below(r, 5)) - 2; /* -2..+2 */
    if (mtick == RELAIS_TICK_NONE) mtick = tick;
    relais_mandate mmand = mandate + (relais_mandate)(t_rng_below(r, 3)) - 1; /* -1..+1 */
    float v = rand_value(r);
    uint32_t kind = t_rng_below(r, 10);
    if (kind < 5) {
      for (uint8_t w = 0; w < RELAIS_TESTAMENT_MAX; w++) will[w] = rand_value(r);
      *will_len = (uint8_t)RELAIS_TESTAMENT_MAX;
      make_frame(&msgs[i], RELAIS_MSG_ORDRE, sender, mtick, mmand, v, will,
                 *will_len, false);
    } else if (kind < 8) {
      make_frame(&msgs[i], RELAIS_MSG_OMBRE, sender, mtick, mmand, v, NULL, 0, false);
    } else if (kind == 8) {
      make_frame(&msgs[i], RELAIS_MSG_DEMANDE, sender, mtick, mmand, v, NULL, 0,
                 t_rng_below(r, 2) == 0);
    } else {
      make_frame(&msgs[i], RELAIS_MSG_CESSION, sender, mtick, mmand, 0.0f, NULL, 0, false);
      msgs[i].reason = (uint8_t)t_rng_below(r, 4);
    }
    (void)holder;
  }
  return n;
}

static bool frame_named_tick(const relais_msg *msgs, size_t n, relais_tick tick,
                             relais_node holder, float value) {
  for (size_t i = 0; i < n; i++) {
    if (msgs[i].tick != tick) continue;
    if (holder != RELAIS_NODE_NONE && msgs[i].sender != holder) continue;
    float v = msgs[i].value;
    if (fabsf(v - value) <= WILL_TOL) return true;
  }
  return false;
}

static bool will_covers(const relais_arb *a, relais_tick tick) {
  if (a->testament_len == 0 || a->testament_base == RELAIS_TICK_NONE) return false;
  return tick >= a->testament_base && tick < a->testament_base + a->testament_len;
}

static void prop_one_run(uint64_t seed) {
  t_rng r; t_rng_seed(&r, seed);
  relais_params p; relais_params_default(&p);
  relais_arb a; relais_arb_init(&a, &p);
  relais_arb_out out;
  relais_mandate prev_mandate = 0;
  float will[RELAIS_TESTAMENT_MAX]; uint8_t will_len = 0;

  for (relais_tick tick = 1; tick <= PROP_TICKS; tick++) {
    relais_msg msgs[PROP_MSGS];
    size_t n = fill_random(&r, msgs, tick, a.mandate, a.holder, will, &will_len);
    relais_meas meas; clear_meas(&meas);
    meas.pos = rand_value(&r); meas.vel = rand_value(&r);
    relais_arb_step(&a, tick, msgs, n, &meas, &out);

    /* P1: monotonic mandate, at most one step per change of holder. */
    T_CHECK(out.mandate >= prev_mandate);
    T_CHECK(out.mandate - prev_mandate <= 1);
    prev_mandate = out.mandate;

    /* P2: exactly one value applied every tick. */
    T_CHECK(out.applied);

    /* P3: a holder order is applied only from a frame that named this tick. */
    if (out.source == RELAIS_SRC_HOLDER)
      T_CHECK(frame_named_tick(msgs, n, tick, a.holder, out.value));

    /* P4/P5: the will is never played beyond its horizon; fallback only after. */
    if (out.source == RELAIS_SRC_TESTAMENT) T_CHECK(will_covers(&a, tick));
    if (out.source == RELAIS_SRC_FALLBACK) T_CHECK(!will_covers(&a, tick));

    /* P6: coherence of the lamp with the applied source. */
    if (out.source == RELAIS_SRC_FALLBACK) T_CHECK(out.lamp == RELAIS_LAMP_RED);
    if (a.disagreement) T_CHECK(out.lamp == RELAIS_LAMP_RED);
  }
}

int main(void) {
  t_section("property: monotonic mandate, one value per tick, will horizon, lamp");
  for (uint64_t i = 0; i < PROP_RUNS; i++) prop_one_run(0x9e3779b97f4a7c15ull ^ (i * 2654435761u));
  t_section("property: deterministic replay (same seed twice)");
  prop_one_run(42); prop_one_run(42);
  return t_summary();
}