/* Relais — protocol parameters (spec 6.3) and the tick-based logic (CMD-07).
 *
 * Requirements covered here:
 *   CMD-01  T is the tick period, 1..50 ms, 5 ms by default;
 *   6.3     the documented defaults, and the consistency rule N >= p + 2 + 3,
 *           with the motive spelled out in `why` when it does not hold;
 *   CMD-07  slow motion multiplies T by slow_factor and nothing else: every
 *           protocol delay stays expressed in ticks. The proof is a run of the
 *           arbiter at two different periods over the same frames, which must
 *           yield the same event stream.
 */
#include <string.h>

#include "test_util.h"

/* ---------------------------------------------------------------- defaults */

static void test_defaults(void) {
  t_section("6.3 documented defaults");
  relais_params p;
  char why[64];
  why[0] = 'x';
  relais_params_default(&p);
  T_EQ_U(p.period_ms, 5u);        /* CMD-01: 5 ms */
  T_EQ_U(p.patience, 1u);         /* p */
  T_EQ_U(p.testament_len, 8u);    /* N */
  T_EQ_U(p.certify_ticks, 3u);
  T_EQ_U(p.bail_ticks, 4u);
  T_NEAR(p.epsilon, 0.005, 1e-9);  /* 0.5 % of full scale */
  T_NEAR(p.delta, 0.02, 1e-9);     /* 2 % of full scale */
  T_EQ_U(p.degraded_ticks, 6000u); /* 30 s at 5 ms */
  T_EQ_U(p.degraded_ticks * p.period_ms, 30000u);
  T_NEAR(p.max_amplitude, RELAIS_FULL_SCALE, 1e-9);
  T_CHECK(p.max_slew > 0.0f);
  T_CHECK(!p.slow_motion);
  T_CHECK(p.slow_factor > 1u);
  T_NEAR(p.delta, 4.0 * p.epsilon, 1e-9); /* 2 % is four times 0.5 % */
  T_CHECK(relais_params_valid(&p, why, sizeof why));
  T_EQ_U(why[0], 0u); /* cleared on success */
  T_NEAR(relais_params_period_s(&p), 0.005, 1e-12);
  /* A NULL block has the defaults of nothing: it is simply refused. */
  relais_params_default(NULL); /* must not crash */
}

/* ------------------------------------------------------- consistency rule */

static void test_consistency_rule(void) {
  t_section("6.3 consistency rule N >= p + 2 + 3");
  relais_params p;
  char why[128];
  relais_params_default(&p);

  p.patience = 1;
  p.testament_len = 8;
  T_CHECK(relais_params_valid(&p, why, sizeof why)); /* 8 >= 1 + 5 */
  p.patience = 3;
  T_CHECK(relais_params_valid(&p, why, sizeof why)); /* 8 >= 3 + 5, boundary */
  p.patience = 2;
  p.testament_len = 6;
  T_CHECK(!relais_params_valid(&p, why, sizeof why)); /* 6 < 7 */
  T_CHECK(strstr(why, "N < p + 2 + 3") != NULL);      /* the motive is named */

  /* N = 5 with p = 1 is refused, exactly as the spec reader expects. */
  relais_params q;
  relais_params_default(&q);
  q.testament_len = 5;
  q.patience = 1;
  T_CHECK(!relais_params_valid(&q, why, sizeof why));
  T_CHECK(strstr(why, "N < p + 2 + 3") != NULL);

  /* N = 8 with p = 4: the rule needs 9, and the range of p is capped too. */
  q.testament_len = 8;
  q.patience = 4;
  T_CHECK(!relais_params_valid(&q, why, sizeof why));
  T_CHECK(strstr(why, "p > 3") != NULL);

  /* A NULL block is refused with a motive, never dereferenced. */
  T_CHECK(!relais_params_valid(NULL, why, sizeof why));
  T_CHECK(why[0] != '\0');
}

/* -------------------------------------------------------------- the bounds */

/* One out-of-domain value per case; each must be refused with a motive. */
static void set_out_of_bounds(relais_params *p, unsigned field) {
  switch (field) {
    case 0: p->period_ms = 0; break;
    case 1: p->period_ms = 51; break;
    case 2: p->bail_ticks = 1; break;
    case 3: p->delta = 0.0f; break;
    case 4: p->epsilon = 0.0f; break;
    case 5: p->max_amplitude = 0.0f; break;
    case 6: p->max_slew = 0.0f; break;
    case 7: p->degraded_ticks = 0; break;
    case 8: p->slow_factor = 0; break;
    case 9: p->certify_ticks = 0; break;
    default: p->testament_len = 0; break;
  }
}

static void test_bounds_refused(void) {
  t_section("every bound outside its domain is refused");
  char why[128];
  for (unsigned field = 0; field <= 10u; field++) {
    relais_params p;
    relais_params_default(&p);
    set_out_of_bounds(&p, field);
    why[0] = '\0';
    T_CHECK(!relais_params_valid(&p, why, sizeof why));
    T_CHECK(why[0] != '\0'); /* the motive is spelled out, not left empty */
  }
  /* A NULL motive buffer is allowed on both paths, and does not crash. */
  relais_params ok, bad;
  relais_params_default(&ok);
  T_CHECK(relais_params_valid(&ok, NULL, 0));
  relais_params_default(&bad);
  bad.period_ms = 0;
  T_CHECK(!relais_params_valid(&bad, NULL, 0));
  /* The edges of the period range are accepted. */
  relais_params edge;
  relais_params_default(&edge);
  edge.period_ms = 1;
  T_CHECK(relais_params_valid(&edge, why, sizeof why));
  edge.period_ms = 50;
  T_CHECK(relais_params_valid(&edge, why, sizeof why));
}

/* -------------------------------------------------------------- the parser */

static void test_parse_accepted(void) {
  t_section("parse the documented override list");
  relais_params p;
  char why[128];
  relais_params_default(&p);
  const char *spec = "T=10,p=2,N=8,certify=4,bail=5,eps=0.01,delta=0.03,"
                     "degraded=3000,amp=1.5,slew=0.1,slow=10";
  T_CHECK(relais_params_parse(&p, spec, why, sizeof why));
  T_EQ_U(p.period_ms, 10u);
  T_EQ_U(p.patience, 2u);
  T_EQ_U(p.testament_len, 8u);
  T_EQ_U(p.certify_ticks, 4u);
  T_EQ_U(p.bail_ticks, 5u);
  T_NEAR(p.epsilon, 0.01, 1e-6);
  T_NEAR(p.delta, 0.03, 1e-6);
  T_EQ_U(p.degraded_ticks, 3000u);
  T_NEAR(p.max_amplitude, 1.5, 1e-6);
  T_NEAR(p.max_slew, 0.1, 1e-6);
  T_EQ_U(p.slow_factor, 10u);
  T_CHECK(p.slow_motion); /* slow=10 > 1 turns the mode on */
  /* An empty spec keeps whatever the block already holds. */
  relais_params q;
  relais_params_default(&q);
  T_CHECK(relais_params_parse(&q, "", why, sizeof why));
  T_EQ_U(q.period_ms, 5u);
  T_CHECK(relais_params_parse(&q, NULL, why, sizeof why) == false ||
          relais_params_valid(&q, why, sizeof why));
}

static void test_parse_refused(void) {
  t_section("parse refusals: unknown key, key without value, oversized spec");
  relais_params p;
  char why[128];
  relais_params_default(&p);
  T_CHECK(!relais_params_parse(&p, "T=5,foo=1", why, sizeof why));
  T_CHECK(strstr(why, "inconnue") != NULL);
  relais_params_default(&p);
  T_CHECK(!relais_params_parse(&p, "T=5,p", why, sizeof why));
  T_CHECK(strstr(why, "sans valeur") != NULL);
  /* A spec longer than the parser's buffer is refused, not truncated. */
  char big[600];
  memset(big, 'x', sizeof big - 1u);
  big[sizeof big - 1u] = '\0';
  relais_params_default(&p);
  T_CHECK(!relais_params_parse(&p, big, why, sizeof why));
  T_CHECK(strstr(why, "trop long") != NULL);
  /* A spec that parses but breaks the consistency rule is refused. */
  relais_params_default(&p);
  T_CHECK(!relais_params_parse(&p, "N=5,p=1", why, sizeof why));
  T_CHECK(strstr(why, "N < p + 2 + 3") != NULL);
  /* A NULL block is refused outright. */
  T_CHECK(!relais_params_parse(NULL, "T=5", why, sizeof why));
}

/* ------------------------------------------------------------------ CMD-07 */

#define TRACE_TICKS 40u
#define TRACE_W 24u /* bytes recorded per tick */

/* A scripted stream of frames, built once and replayed to both runs so that
 * the only variable between them is the period. */
static relais_msg SCRIPT[TRACE_TICKS];
static bool HAVE[TRACE_TICKS];

static void build_script(void) {
  relais_params p;
  relais_params_default(&p);
  for (unsigned t = 0; t < TRACE_TICKS; t++) {
    HAVE[t] = false;
    relais_msg_clear(&SCRIPT[t]);
  }
  /* Tick 0: B asks for the mandate with its first order embedded. */
  t_demande(&SCRIPT[0], RELAIS_NODE_B, 0u, 0u, true, 0.25f, &p);
  HAVE[0] = true;
  /* Ticks 1..9: fresh orders of the holder for the tick being applied. */
  for (unsigned t = 1; t < 10u; t++) {
    t_order(&SCRIPT[t], RELAIS_NODE_B, 1u, t, 0.25f + 0.01f * (float)t, &p);
    HAVE[t] = true;
  }
  /* Ticks 10..19: silence, so the mandate is revoked and the will is played. */
  /* Tick 20: A asks, with its first order. */
  t_demande(&SCRIPT[20], RELAIS_NODE_A, 1u, 20u, true, -0.125f, &p);
  HAVE[20] = true;
  for (unsigned t = 21; t < 30u; t++) {
    t_order(&SCRIPT[t], RELAIS_NODE_A, 2u, t, -0.125f, &p);
    HAVE[t] = true;
  }
  /* Ticks 30..39: silence again, the will runs out and the fallback takes
   * over: that transition is where a clock-based delay would show up. */
}

/* Record one tick of the arbiter: source, mandate, holder, misses, value bits
 * and the kinds of the events produced. */
static void record_tick(uint8_t *dst, const relais_arb_out *o) {
  dst[0] = (uint8_t)o->source;
  dst[1] = (uint8_t)o->holder;
  dst[2] = o->misses;
  dst[3] = o->event_count;
  for (unsigned i = 0; i < 4u; i++) dst[4u + i] = (uint8_t)(o->mandate >> (i * 8u));
  memcpy(dst + 8u, &o->value, 4u); /* bit pattern, not a rounded value */
  for (unsigned i = 0; i < RELAIS_EVENTS_MAX && 12u + i < TRACE_W; i++)
    dst[12u + i] = (uint8_t)o->events[i].kind;
}

static void run_trace(const relais_params *p, uint8_t *trace) {
  relais_arb a;
  relais_arb_init(&a, p);
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  for (unsigned t = 0; t < TRACE_TICKS; t++) {
    relais_arb_out out;
    meas.pos = 0.01f * (float)t;
    relais_arb_step(&a, t, HAVE[t] ? &SCRIPT[t] : NULL, HAVE[t] ? 1u : 0u, &meas, &out);
    record_tick(&trace[(size_t)t * TRACE_W], &out);
  }
}

static bool same_event_stream(const relais_params *pa, const relais_params *pb) {
  static uint8_t ta[TRACE_TICKS * TRACE_W], tb[TRACE_TICKS * TRACE_W];
  run_trace(pa, ta);
  run_trace(pb, tb);
  if (memcmp(ta, tb, sizeof ta) != 0) return false;
  /* A trace of nothing but zeroes would prove nothing: make sure the run
   * actually applied values and produced events. */
  bool saw_value = false, saw_event = false;
  for (unsigned t = 0; t < TRACE_TICKS; t++) {
    uint32_t bits;
    memcpy(&bits, &ta[(size_t)t * TRACE_W + 8u], 4u);
    if (bits != 0u) saw_value = true;
    if (ta[(size_t)t * TRACE_W + 3u] != 0u) saw_event = true;
  }
  return saw_value && saw_event;
}

static void test_cmd07_period_and_tick_logic(void) {
  t_section("CMD-07 slow motion changes T only, the logic stays in ticks");
  relais_params p;
  relais_params_default(&p);
  T_EQ_U(relais_params_period_ms(&p), 5u);
  p.slow_motion = true;
  T_EQ_U(relais_params_period_ms(&p), 5u * p.slow_factor);
  T_NEAR(relais_params_period_s(&p), (double)(5u * p.slow_factor) / 1000.0, 1e-12);
  p.period_ms = 20;
  T_EQ_U(relais_params_period_ms(&p), 20u * p.slow_factor);
  p.slow_motion = false;
  T_EQ_U(relais_params_period_ms(&p), 20u);
  T_EQ_U(relais_params_period_ms(NULL), 0u);
  T_NEAR(relais_params_period_s(NULL), 0.0, 0.0);

  build_script();
  relais_params fast, slow, other;
  relais_params_default(&fast);
  slow = fast;
  slow.slow_motion = true;
  slow.slow_factor = 10;
  T_CHECK(relais_params_period_ms(&slow) != relais_params_period_ms(&fast));
  /* Same frames, ten times the period, same decisions tick by tick. */
  T_CHECK(same_event_stream(&fast, &slow));
  /* The trace has teeth: another patience moves the revocation in ticks. */
  other = fast;
  other.patience = 3;
  other.testament_len = 8;
  T_CHECK(relais_params_valid(&other, NULL, 0));
  T_CHECK(!same_event_stream(&fast, &other));
}

int main(void) {
  test_defaults();
  test_consistency_rule();
  test_bounds_refused();
  test_parse_accepted();
  test_parse_refused();
  test_cmd07_period_and_tick_logic();
  return t_summary();
}
