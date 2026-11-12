/* Relais — unit tests of the M02 latency recorder (host/measure/latency.*).
 *
 * Covers: exact percentiles on known sets, the power-of-two histogram, the
 * worst-case keep list (PERF-07), the JSON sample round trip shared with
 * host/timing/wcet.py, and the boundary cases (0 sample, 1 sample, all equal,
 * raw-window overflow).
 *
 * Run: ./build/test_latency   (make test runs it with the others)
 */
#include <stdio.h>
#include <string.h>

#include "latency.h"
#include "test_util.h"

#define US(x) ((uint64_t)(x) * (uint64_t)RL_LAT_NS_PER_US)

static void fill(rl_lat_rec *r, rl_lat_source src, const uint64_t *ns,
                 unsigned n) {
  rl_lat_reset(r, src, 'A');
  for (unsigned i = 0; i < n; i++) rl_lat_record(r, (uint64_t)i + 1, ns[i]);
}

/* 1..100 us: every quantile is known by hand (nearest rank). */
static void test_percentiles_exact(void) {
  t_section("percentiles, exact window");
  rl_lat_rec r;
  uint64_t ns[100];
  for (unsigned i = 0; i < 100; i++) ns[i] = US(i + 1);
  fill(&r, RL_LAT_SRC_PATH, ns, 100);

  rl_lat_stats st;
  rl_lat_compute_stats(&r, &st);
  T_CHECK(!st.empty);
  T_CHECK(st.exact);
  T_EQ_U(st.count, 100);
  T_EQ_U(st.min_ns, US(1));
  T_EQ_U(st.max_ns, US(100));
  T_EQ_U(st.p50_ns, US(50));  /* rank ceil(0.50*100) = 50 */
  T_EQ_U(st.p90_ns, US(90));  /* rank 90 */
  T_EQ_U(st.p99_ns, US(99));  /* rank 99 */
  T_EQ_U(st.p999_ns, US(100));/* rank ceil(0.999*100) = 100 */
  T_EQ_U(st.mean_ns, 50500);  /* 50.5 us */

  uint64_t v = 0;
  T_CHECK(rl_lat_percentile(&r, 0.25, &v));
  T_EQ_U(v, US(25));
  T_CHECK(rl_lat_percentile(&r, 1.0, &v));
  T_EQ_U(v, US(100));
}

static void test_boundaries(void) {
  t_section("boundaries: 0, 1, all equal");
  rl_lat_rec r;
  rl_lat_stats st;
  uint64_t v = 12345;

  rl_lat_reset(&r, RL_LAT_SRC_LAW, 'B');
  rl_lat_compute_stats(&r, &st);
  T_CHECK(st.empty);
  T_EQ_U(st.count, 0);
  T_CHECK(!rl_lat_percentile(&r, 0.5, &v));
  T_EQ_U(v, 12345); /* untouched */
  T_EQ_U(rl_lat_worst_count(&r), 0);
  T_CHECK(rl_lat_worst_at(&r, 0) == NULL);

  rl_lat_record(&r, 7, US(42));
  rl_lat_compute_stats(&r, &st);
  T_CHECK(!st.empty);
  T_CHECK(st.exact);
  T_EQ_U(st.p50_ns, US(42));
  T_EQ_U(st.p999_ns, US(42));
  T_EQ_U(st.min_ns, US(42));
  T_EQ_U(st.max_ns, US(42));
  T_EQ_U(rl_lat_worst_count(&r), 1);

  uint64_t same[16];
  for (unsigned i = 0; i < 16; i++) same[i] = US(5);
  fill(&r, RL_LAT_SRC_DECODE, same, 16);
  rl_lat_compute_stats(&r, &st);
  T_EQ_U(st.count, 16);
  T_EQ_U(st.p50_ns, US(5));
  T_EQ_U(st.p99_ns, US(5));
  T_EQ_U(st.p999_ns, US(5));
  T_EQ_U(st.min_ns, US(5));
  T_EQ_U(st.max_ns, US(5));
  T_EQ_U(st.mean_ns, US(5));
}

static void test_histogram(void) {
  t_section("power-of-two histogram");
  T_EQ_U(rl_lat_bucket_of(0), 0);
  T_EQ_U(rl_lat_bucket_of(999), 0);
  T_EQ_U(rl_lat_bucket_of(1000), 1);
  T_EQ_U(rl_lat_bucket_of(1999), 1);
  T_EQ_U(rl_lat_bucket_of(2000), 2);
  T_EQ_U(rl_lat_bucket_of(4000), 3);
  T_EQ_U(rl_lat_bucket_of(US(1) << 20), 21);
  T_EQ_U(rl_lat_bucket_of(UINT64_MAX), RL_LAT_BUCKETS - 1); /* saturates */

  rl_lat_rec r;
  uint64_t ns[6] = {500, US(1), US(3), US(3), US(9), US(1 << 22)};
  fill(&r, RL_LAT_SRC_PATH, ns, 6);
  T_EQ_U(rl_lat_bucket_hits(&r, 0), 1);  /* 0.5 us */
  T_EQ_U(rl_lat_bucket_hits(&r, 1), 1);  /* 1 us */
  T_EQ_U(rl_lat_bucket_hits(&r, 2), 2);  /* 3 us, twice */
  T_EQ_U(rl_lat_bucket_hits(&r, 3), 0);
  T_EQ_U(rl_lat_bucket_hits(&r, 4), 1);  /* 9 us */
  T_EQ_U(rl_lat_bucket_hits(&r, RL_LAT_BUCKETS), 0); /* out of range */
  uint64_t total = 0;
  for (unsigned b = 0; b < RL_LAT_BUCKETS; b++) total += rl_lat_bucket_hits(&r, b);
  T_EQ_U(total, 6);

  char label[32];
  T_CHECK(rl_lat_bucket_label(0, label, sizeof label) > 0);
  T_CHECK(strcmp(label, "<1us") == 0);
  T_CHECK(rl_lat_bucket_label(1, label, sizeof label) > 0);
  T_CHECK(strcmp(label, "1-2us") == 0);
  T_CHECK(rl_lat_bucket_label(3, label, sizeof label) > 0);
  T_CHECK(strcmp(label, "4-8us") == 0);
  T_CHECK(rl_lat_bucket_label(RL_LAT_BUCKETS - 1, label, sizeof label) > 0);
  T_CHECK(strncmp(label, ">=", 2) == 0);
  T_EQ_U(rl_lat_bucket_upper_ns(0), 1000);
  T_EQ_U(rl_lat_bucket_upper_ns(3), US(8));
}

/* The raw window overflows: percentiles must stay available, be flagged as
 * estimates, and never under-estimate the true quantile. */
static void test_capacity_overflow(void) {
  t_section("capacity overflow");
  rl_lat_rec r;
  rl_lat_reset(&r, RL_LAT_SRC_PATH, 'A');
  const uint64_t n = RL_LAT_RAW_MAX + 100;
  for (uint64_t i = 1; i <= n; i++) rl_lat_record(&r, i, US((i % 50) + 1));

  rl_lat_stats st;
  rl_lat_compute_stats(&r, &st);
  T_EQ_U(st.count, n);
  T_CHECK(!st.exact);
  T_EQ_U(st.min_ns, US(1));
  T_EQ_U(st.max_ns, US(50));
  uint64_t total = 0;
  for (unsigned b = 0; b < RL_LAT_BUCKETS; b++) total += rl_lat_bucket_hits(&r, b);
  T_EQ_U(total, n); /* nothing is lost when the raw window is full */
  /* p50 of a uniform 1..50 us spread is 25 us; the bucket upper bound is 32. */
  T_CHECK(st.p50_ns >= US(25));
  T_CHECK(st.p50_ns <= US(32));
  T_CHECK(st.p999_ns >= US(50));
}

static void test_worst_cases(void) {
  t_section("worst cases (PERF-07)");
  rl_lat_rec r;
  rl_lat_reset(&r, RL_LAT_SRC_PATH, 'B');
  for (uint64_t i = 1; i <= 50; i++) rl_lat_record(&r, i, US(10));
  rl_lat_record(&r, 51, US(900));
  rl_lat_record(&r, 52, US(1500));
  rl_lat_record(&r, 53, US(1200));
  for (uint64_t i = 54; i <= 80; i++) rl_lat_record(&r, i, US(11));

  T_EQ_U(rl_lat_worst_count(&r), RL_LAT_WORST_MAX);
  const rl_lat_sample *w0 = rl_lat_worst_at(&r, 0);
  const rl_lat_sample *w1 = rl_lat_worst_at(&r, 1);
  const rl_lat_sample *w2 = rl_lat_worst_at(&r, 2);
  T_CHECK(w0 != NULL && w1 != NULL && w2 != NULL);
  T_EQ_U(w0->ns, US(1500));
  T_EQ_U(w0->tick, 52);
  T_EQ_U(w1->ns, US(1200));
  T_EQ_U(w1->tick, 53);
  T_EQ_U(w2->ns, US(900));
  T_EQ_U(w2->tick, 51);
  T_CHECK(w0->role == 'B');
  T_CHECK(w0->source == RL_LAT_SRC_PATH);
  /* The list stays sorted descending and never grows past its capacity. */
  uint64_t previous = UINT64_MAX;
  for (uint8_t i = 0; i < rl_lat_worst_count(&r); i++) {
    const rl_lat_sample *w = rl_lat_worst_at(&r, i);
    T_CHECK(w != NULL);
    T_CHECK(w->ns <= previous);
    previous = w->ns;
  }
  T_CHECK(rl_lat_worst_at(&r, rl_lat_worst_count(&r)) == NULL);
}

static void test_json_round_trip(void) {
  t_section("JSON sample round trip");
  rl_lat_sample s;
  s.ns = 412318;
  s.tick = 1234;
  s.source = RL_LAT_SRC_LAW;
  s.role = 'A';
  char line[RL_LAT_LINE_MAX];
  int n = rl_lat_format_sample(&s, line, sizeof line);
  T_CHECK(n > 0);
  T_CHECK(strstr(line, "\"kind\":\"latency\"") != NULL);
  T_CHECK(strstr(line, "\"source\":\"law\"") != NULL);
  T_CHECK(strstr(line, "\"latency_us\":412.318") != NULL);

  rl_lat_sample back;
  T_CHECK(rl_lat_parse_sample(line, &back));
  T_EQ_U(back.ns, s.ns);
  T_EQ_U(back.tick, s.tick);
  T_CHECK(back.source == RL_LAT_SRC_LAW);
  T_CHECK(back.role == 'A');

  /* The line format host/timing/wcet.py documents, keys in another order. */
  T_CHECK(rl_lat_parse_sample(
      "{\"latency_us\": 412, \"tick\": 7, \"source\": \"net_rx\", \"role\": \"A\"}",
      &back));
  T_EQ_U(back.ns, US(412));
  T_EQ_U(back.tick, 7);
  T_CHECK(back.role == 'A');

  /* Not a sample: no latency_us. */
  T_CHECK(!rl_lat_parse_sample("{\"kind\":\"tick\",\"tick\":3}", &back));
  T_CHECK(!rl_lat_parse_sample("garbage", &back));
  T_CHECK(!rl_lat_parse_sample("", &back));
  T_CHECK(!rl_lat_parse_sample(NULL, &back));
  T_CHECK(!rl_lat_parse_sample("{\"latency_us\": -1}", &back));
}

static void test_summary_and_file(void) {
  t_section("summary line and file round trip");
  rl_lat_rec r;
  uint64_t ns[5] = {US(1), US(2), US(3), US(4), US(900)};
  fill(&r, RL_LAT_SRC_PATH, ns, 5);
  rl_lat_stats st;
  rl_lat_compute_stats(&r, &st);
  char sum[RL_LAT_SUMMARY_MAX];
  int n = rl_lat_format_summary(&r, &st, sum, sizeof sum);
  T_CHECK(n > 0);
  T_CHECK(strstr(sum, "\"kind\":\"latency_summary\"") != NULL);
  T_CHECK(strstr(sum, "\"n\":5") != NULL);
  T_CHECK(strstr(sum, "\"exact\":true") != NULL);
  T_CHECK(strstr(sum, "\"max_us\":900.000") != NULL);
  T_CHECK(strstr(sum, "\"hist\":[") != NULL);
  T_CHECK(sum[n - 1] == '}');

  const char *path = "build/test_latency_samples.jsonl";
  FILE *f = rl_lat_open(path);
  T_CHECK(f != NULL);
  if (f != NULL) {
    for (unsigned i = 0; i < 5; i++) {
      rl_lat_sample s;
      s.ns = ns[i];
      s.tick = (uint64_t)i + 1;
      s.source = RL_LAT_SRC_PATH;
      s.role = 'A';
      T_CHECK(rl_lat_write_sample(f, &s));
    }
    T_CHECK(fclose(f) == 0);
  }
  f = fopen(path, "r");
  T_CHECK(f != NULL);
  if (f != NULL) {
    char line[RL_LAT_LINE_MAX];
    unsigned read_back = 0;
    uint64_t total_ns = 0;
    while (fgets(line, (int)sizeof line, f) != NULL) {
      rl_lat_sample s;
      if (!rl_lat_parse_sample(line, &s)) continue;
      read_back++;
      total_ns += s.ns;
    }
    fclose(f);
    T_EQ_U(read_back, 5);
    T_EQ_U(total_ns, US(1) + US(2) + US(3) + US(4) + US(900));
  }
  remove(path);
}

static void test_clock(void) {
  t_section("monotonic clock");
  uint64_t a = rl_lat_now_ns();
  uint64_t b = rl_lat_now_ns();
  T_CHECK(a > 0);
  T_CHECK(b >= a); /* CLOCK_MONOTONIC never goes back */
  T_CHECK(b - a < US(10000)); /* two back-to-back reads stay cheap */
}

int main(void) {
  test_percentiles_exact();
  test_boundaries();
  test_histogram();
  test_capacity_overflow();
  test_worst_cases();
  test_json_round_trip();
  test_summary_and_file();
  test_clock();
  return t_summary();
}
