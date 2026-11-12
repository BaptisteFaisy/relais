/* Relais — non-interference proof by test (spec 7.2, ideas 3 / 5 / 8, MAN-02,
 * MAN-03, REL-05, DET-03).
 *
 * The property: the second channel can never influence the value the actuator
 * applies unless the actuator has delivered it a mandate. We prove it by
 * running TWO worlds in lockstep that are identical except that, in world 2,
 * the second sends arbitrary shadows (random values, extremes included). As
 * long as the second is neither holder nor certified, the applied value and
 * the mandate must be IDENTICAL bit for bit in both worlds. Any divergence is
 * a counterexample, written in the scenario format.
 *
 * The harness also covers a deposed machine: once it lost the mandate, world 2
 * lets the former holder keep emitting arbitrary orders; the actuator must
 * drop them, so value and mandate must not move (MAN-02, I1, I3).
 *
 * Self-contained: it drives relais_arb_step directly, no clock, no file, no
 * socket, no allocation. Usage: noninterference [--runs N] [--ticks N]
 * [--counterexample FILE]. Exit 0 when every run agrees, 1 on a divergence.
 */
#include "relais/arbiter.h"
#include "relais/control.h"
#include "relais/params.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deterministic xorshift so a run is reproducible from its seed. */
typedef struct {
  uint64_t s;
} ni_rng;

static void ni_rng_seed(ni_rng *r, uint64_t seed) {
  r->s = seed != 0 ? seed : 0x9e3779b97f4a7c15ull;
}
static uint64_t ni_rng_next(ni_rng *r) {
  uint64_t x = r->s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  r->s = x;
  return x * 0x2545f4914f6cdd1dull;
}
static double ni_rng_unit(ni_rng *r) {
  return (double)(ni_rng_next(r) >> 11) / 9007199254740992.0;
}

/* One arbitrary value: extremes plus a random draw inside and outside the
 * command bound, so a second that could influence would show it at once. */
static float arbitrary_value(ni_rng *r, const relais_params *p) {
  static const float extremes[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e6f, -1e6f,
                                   3.4028235e38f, -3.4028235e38f};
  size_t n = sizeof extremes / sizeof extremes[0];
  double draw = ni_rng_unit(r);
  if (draw < 0.4) return extremes[(size_t)(draw * 250.0) % n];
  return (float)(draw * 4.0 - 2.0) * p->max_amplitude;
}

/* A holder order carrying the current mandate and targeting this tick. */
static void msg_order(relais_msg *m, relais_node who, relais_tick tick,
                      relais_mandate mandate, float value) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ORDRE;
  m->sender = who;
  m->tick = tick;
  m->mandate = mandate;
  m->value = value;
  m->has_order = true;
  m->testament_len = 0;
}
static void msg_shadow(relais_msg *m, relais_node who, relais_tick tick, float value) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_OMBRE;
  m->sender = who;
  m->tick = tick;
  m->value = value;
}
static void msg_demande(relais_msg *m, relais_node who, relais_tick tick, float value) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_DEMANDE;
  m->sender = who;
  m->tick = tick;
  m->value = value;
  m->has_order = true;
}
static void msg_cession(relais_msg *m, relais_node who, relais_mandate mandate) {
  relais_msg_clear(m);
  m->type = RELAIS_MSG_CESSION;
  m->sender = who;
  m->mandate = mandate;
  m->reason = 1;
}

/* A divergence, reported in the scenario format. */
typedef struct {
  bool found;
  const char *scenario;
  uint64_t seed;
  relais_tick tick;
  float w1_value, w2_value;
  relais_mandate w1_mandate, w2_mandate;
  relais_node w1_holder, w2_holder;
  relais_source w1_source, w2_source;
  float injected; /* the arbitrary value world 2 fed the actuator */
} ni_counter;

static void counter_record(ni_counter *c, const char *scenario, uint64_t seed,
                           relais_tick tick, const relais_arb_out *a,
                           const relais_arb_out *b, float injected) {
  if (c->found) return; /* keep the first divergence only */
  c->found = true;
  c->scenario = scenario;
  c->seed = seed;
  c->tick = tick;
  c->w1_value = a->value;
  c->w2_value = b->value;
  c->w1_mandate = a->mandate;
  c->w2_mandate = b->mandate;
  c->w1_holder = a->holder;
  c->w2_holder = b->holder;
  c->w1_source = a->source;
  c->w2_source = b->source;
  c->injected = injected;
}

/* True when the reference world (w1) leaves the second neither holder nor
 * certified at the start of this tick: the window where the property bites. */
static bool second_is_inert(const relais_arb *w1, relais_node second) {
  return w1->holder != second && !w1->certified;
}

/* Step both worlds one tick and, inside the comparison window, require the
 * applied value and the mandate to be identical bit for bit. */
static bool step_and_compare(relais_arb *w1, relais_arb *w2, relais_tick tick,
                             const relais_msg *m1, size_t n1, const relais_msg *m2,
                             size_t n2, const relais_meas *meas, ni_counter *c,
                             const char *scenario, uint64_t seed, float injected,
                             bool in_window) {
  relais_arb_out a1, a2;
  relais_arb_step(w1, tick, m1, n1, meas, &a1);
  relais_arb_step(w2, tick, m2, n2, meas, &a2);
  if (!in_window) return true;
  if (a1.value == a2.value && a1.mandate == a2.mandate) return true;
  counter_record(c, scenario, seed, tick, &a1, &a2, injected);
  return false;
}

static void counter_write(const ni_counter *c, FILE *f) {
  if (f == NULL || !c->found) return;
  fprintf(f,
          "{\"kind\":\"noninterference-counterexample\",\"scenario\":\"%s\","
          "\"seed\":%llu,\"tick\":%llu,\"injected\":%.6g,"
          "\"world1\":{\"value\":%.6g,\"mandate\":%u,\"holder\":\"%s\",\"source\":\"%s\"},"
          "\"world2\":{\"value\":%.6g,\"mandate\":%u,\"holder\":\"%s\",\"source\":\"%s\"}}\n",
          c->scenario, (unsigned long long)c->seed, (unsigned long long)c->tick,
          (double)c->injected, (double)c->w1_value, (unsigned)c->w1_mandate,
          relais_node_name(c->w1_holder), relais_source_name(c->w1_source),
          (double)c->w2_value, (unsigned)c->w2_mandate, relais_node_name(c->w2_holder),
          relais_source_name(c->w2_source));
}

/* Scenario 1: the second sends arbitrary shadows. The reference world feeds a
 * shadow that never matches the order (so the second never certifies and the
 * comparison window is the whole run); world 2 replaces it with random noise. */
static bool run_shadow_scenario(const relais_params *p, uint64_t seed,
                                relais_tick ticks, ni_counter *c) {
  relais_arb w1, w2;
  relais_arb_init(&w1, p);
  relais_arb_init(&w2, p);
  ni_rng rng;
  ni_rng_seed(&rng, seed);
  const relais_node second = RELAIS_NODE_B;
  bool ok = true;

  for (relais_tick tick = 1; tick <= ticks && ok; tick++) {
    float order = relais_clamp_value((float)(sin((double)tick * 0.1) * 0.6 *
                                             (double)p->max_amplitude), p);
    bool inert = second_is_inert(&w1, second);
    float injected = arbitrary_value(&rng, p);
    /* World 1: a shadow that never matches, so certification never fires.
     * World 2: the arbitrary shadow under test. */
    relais_msg m1[2], m2[2];
    msg_shadow(&m1[1], second, tick, order + 10.0f * p->epsilon + 0.5f);
    msg_shadow(&m2[1], second, tick, injected);
    relais_meas meas = {0};
    meas.pos = (float)(0.3 * sin((double)tick * 0.05));
    meas.vel = 0.01f * (float)tick;
    meas.setpoint = order;

    if (tick == 1) {
      msg_demande(&m1[0], RELAIS_NODE_A, tick, order);
      msg_demande(&m2[0], RELAIS_NODE_A, tick, order);
    } else {
      msg_order(&m1[0], RELAIS_NODE_A, tick, w1.mandate, order);
      msg_order(&m2[0], RELAIS_NODE_A, tick, w2.mandate, order);
    }
    ok = step_and_compare(&w1, &w2, tick, m1, 2, m2, 2, &meas, c,
                          "shadow-arbitrary", seed, injected, inert);
  }
  return ok;
}

/* One tick of the deposed-machine scenario: the message lists of both worlds
 * plus the injected value and whether this tick falls in the comparison
 * window (the former holder emitting as an impostor). */
typedef struct {
  relais_msg m1[RELAIS_IN_MSG_MAX], m2[RELAIS_IN_MSG_MAX];
  size_t n1, n2;
  float injected;
  bool in_window;
} deposed_tick_t;

static void deposed_build(deposed_tick_t *d, relais_tick tick, relais_tick grant_b,
                          relais_tick deposed, const relais_params *p,
                          relais_mandate mand1, relais_mandate mand2, float order,
                          ni_rng *rng) {
  d->n1 = 0;
  d->n2 = 0;
  d->injected = 0.0f;
  d->in_window = false;
  if (tick == 1) { /* A is granted the mandate, carrying its first order. */
    msg_demande(&d->m1[d->n1++], RELAIS_NODE_A, tick, order);
    msg_demande(&d->m2[d->n2++], RELAIS_NODE_A, tick, order);
  } else if (tick < grant_b) { /* A holds; its order is valid in both worlds. */
    msg_order(&d->m1[d->n1++], RELAIS_NODE_A, tick, mand1, order);
    msg_order(&d->m2[d->n2++], RELAIS_NODE_A, tick, mand2, order);
  } else if (tick == grant_b) { /* A cedes; mandate frees at the next tick. */
    msg_cession(&d->m1[d->n1++], RELAIS_NODE_A, mand1);
    msg_cession(&d->m2[d->n2++], RELAIS_NODE_A, mand2);
    msg_order(&d->m1[d->n1++], RELAIS_NODE_A, tick, mand1, order);
    msg_order(&d->m2[d->n2++], RELAIS_NODE_A, tick, mand2, order);
  } else if (tick == deposed) { /* B requests; its demande carries first order. */
    msg_demande(&d->m1[d->n1++], RELAIS_NODE_B, tick, order);
    msg_demande(&d->m2[d->n2++], RELAIS_NODE_B, tick, order);
  } else { /* B holds; world 2 lets the deposed A keep emitting. */
    msg_order(&d->m1[d->n1++], RELAIS_NODE_B, tick, mand1, order);
    msg_order(&d->m2[d->n2++], RELAIS_NODE_B, tick, mand2, order);
    d->injected = arbitrary_value(rng, p);
    msg_order(&d->m2[d->n2++], RELAIS_NODE_A, tick, mand2, d->injected);
    d->in_window = true;
  }
}

static bool run_deposed_scenario(const relais_params *p, uint64_t seed,
                                 relais_tick ticks, ni_counter *c) {
  relais_arb w1, w2;
  relais_arb_init(&w1, p);
  relais_arb_init(&w2, p);
  ni_rng rng;
  ni_rng_seed(&rng, seed ^ 0xdeadbeefull);
  relais_tick grant_b = ticks / 4; /* B is granted the mandate here */
  relais_tick deposed = grant_b + 1; /* A starts emitting as an impostor */
  bool ok = true;

  for (relais_tick tick = 1; tick <= ticks && ok; tick++) {
    float order = relais_clamp_value((float)(0.5 * cos((double)tick * 0.07) *
                                             (double)p->max_amplitude), p);
    deposed_tick_t d;
    deposed_build(&d, tick, grant_b, deposed, p, w1.mandate, w2.mandate, order, &rng);
    relais_meas meas = {0};
    meas.pos = order;
    meas.setpoint = order;
    ok = step_and_compare(&w1, &w2, tick, d.m1, d.n1, d.m2, d.n2, &meas, c,
                          "deposed-emits", seed, d.injected, d.in_window);
  }
  return ok;
}

int main(int argc, char **argv) {
  uint32_t runs = 200;
  uint32_t ticks = 400;
  const char *counter_file = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc)
      runs = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc)
      ticks = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--counterexample") == 0 && i + 1 < argc)
      counter_file = argv[++i];
  }
  if (ticks < 8) ticks = 8; /* the deposed scenario needs a few phases */

  relais_params params;
  relais_params_default(&params);
  char why[128] = "";
  if (!relais_params_valid(&params, why, sizeof why)) {
    fprintf(stderr, "parametres invalides: %s\n", why);
    return 2;
  }

  ni_counter counter = {0};
  uint32_t divergences = 0;
  for (uint32_t run = 0; run < runs; run++) {
    uint64_t seed = 1000ull + run * 7919ull;
    if (!run_shadow_scenario(&params, seed, ticks, &counter)) divergences++;
    if (!run_deposed_scenario(&params, seed, ticks, &counter)) divergences++;
  }

  FILE *cf = counter_file != NULL ? fopen(counter_file, "w") : NULL;
  counter_write(&counter, cf != NULL ? cf : stdout);
  if (cf != NULL) fclose(cf);

  if (divergences > 0) {
    printf("NONINTERFERENCE-FAIL: %u divergence(s) over %u runs "
           "(second influenced the applied value without a mandate)\n",
           divergences, runs);
    return 1;
  }
  printf("NONINTERFERENCE-OK: %u runs x 2 scenarios, %u ticks each, 0 divergence "
         "(second never influenced value or mandate without a mandate)\n",
         runs, ticks);
  return 0;
}
