/* Relais — unit tests of the WCET statistics module (mission M40).
 *
 * The percentiles published by host/wcet/probe.c must be exact and must match
 * the definition of host/timing/wcet.py (`percentile()`, linear interpolation):
 * these checks pin that definition down on known sets, plus the edge cases
 * (empty set, one sample, all identical, a full set). Requirements: PERF-01,
 * PERF-07 (the published number must be trustworthy), QNX-04.
 */
#include <stdio.h>
#include <string.h>

#include "test_util.h"
#include "wcet_stats.h"

static void fill(relais_wcet_set *s, const double *v, size_t n) {
  for (size_t i = 0; i < n; i++) T_CHECK(relais_wcet_add(s, v[i]));
}

static void test_segment_names(void) {
  t_section("M40 segment names are stable (JSON keys of the summary)");
  T_CHECK(strcmp(relais_wcet_seg_name(RELAIS_SEG_PATH), "path") == 0);
  T_CHECK(strcmp(relais_wcet_seg_name(RELAIS_SEG_DECODE), "decode") == 0);
  T_CHECK(strcmp(relais_wcet_seg_name(RELAIS_SEG_NODE), "node") == 0);
  T_CHECK(strcmp(relais_wcet_seg_name(RELAIS_SEG_LAW), "law") == 0);
  T_CHECK(strcmp(relais_wcet_seg_name(RELAIS_SEG_ENCODE), "encode") == 0);
  T_CHECK(RELAIS_SEG_COUNT == 5);
}

static void test_percentile_matches_timing_definition(void) {
  t_section("M40 percentile = linear interpolation, as host/timing/wcet.py");
  double ten[10] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90};
  T_NEAR(relais_wcet_percentile(ten, 10, 0.0), 0.0, 1e-9);
  T_NEAR(relais_wcet_percentile(ten, 10, 1.0), 90.0, 1e-9);
  T_NEAR(relais_wcet_percentile(ten, 10, 0.5), 45.0, 1e-9);
  T_NEAR(relais_wcet_percentile(ten, 10, 0.9), 81.0, 1e-9);
  T_NEAR(relais_wcet_percentile(ten, 10, 0.99), 89.1, 1e-9);
  T_NEAR(relais_wcet_percentile(ten, 10, 0.999), 89.91, 1e-9);
  /* Clamping outside [0,1] and the degenerate sizes. */
  T_NEAR(relais_wcet_percentile(ten, 10, -0.5), 0.0, 1e-9);
  T_NEAR(relais_wcet_percentile(ten, 10, 1.5), 90.0, 1e-9);
  double one[1] = {7.0};
  T_NEAR(relais_wcet_percentile(one, 1, 0.5), 7.0, 1e-9);
  T_NEAR(relais_wcet_percentile(NULL, 0, 0.5), 0.0, 1e-9);
}

static void test_describe_known_set(void) {
  t_section("M40 describe: exact statistics on a known set");
  relais_wcet_set s;
  T_CHECK(relais_wcet_set_init(&s, 8));
  const double v[4] = {1.0, 2.0, 2.0, 3.0};
  fill(&s, v, 4);
  relais_wcet_desc d;
  T_CHECK(relais_wcet_describe(&s, &d));
  T_EQ_U(d.n, 4);
  T_NEAR(d.min, 1.0, 1e-9);
  T_NEAR(d.max, 3.0, 1e-9);
  T_NEAR(d.mean, 2.0, 1e-9);
  T_NEAR(d.p50, 2.0, 1e-9);
  /* Sample standard deviation of {1,2,2,3}: sqrt(2/3). */
  T_NEAR(d.stdev, 0.8164965809, 1e-6);
  relais_wcet_set_free(&s);
}

static void test_edge_cases(void) {
  t_section("M40 edge cases: empty, single, identical, full set");
  relais_wcet_set s;
  relais_wcet_desc d;
  T_CHECK(relais_wcet_set_init(&s, 4));
  T_CHECK(!relais_wcet_describe(&s, &d)); /* empty: no descriptor invented */
  T_CHECK(relais_wcet_add(&s, 5.0));
  T_CHECK(relais_wcet_describe(&s, &d));
  T_NEAR(d.stdev, 0.0, 1e-12); /* one sample: dispersion is 0, not NaN */
  T_NEAR(d.p99_9, 5.0, 1e-12);
  relais_wcet_set_free(&s);

  T_CHECK(relais_wcet_set_init(&s, 3));
  const double same[3] = {2.5, 2.5, 2.5};
  fill(&s, same, 3);
  T_CHECK(!relais_wcet_add(&s, 9.0)); /* full: refused, never silent overflow */
  T_CHECK(relais_wcet_describe(&s, &d));
  T_NEAR(d.min, 2.5, 1e-12);
  T_NEAR(d.max, 2.5, 1e-12);
  T_NEAR(d.stdev, 0.0, 1e-12);
  relais_wcet_set_free(&s);
  T_CHECK(!relais_wcet_set_init(&s, 0)); /* zero capacity is rejected */
}

static void test_describe_sorts(void) {
  t_section("M40 describe sorts in place: percentiles on a shuffled input");
  relais_wcet_set s;
  T_CHECK(relais_wcet_set_init(&s, 100));
  /* 1..100 shuffled by a fixed permutation: p99 must be 99.01-ish. */
  for (int i = 0; i < 100; i++) {
    int k = (i * 37 + 11) % 100; /* bijection on [0,99] */
    T_CHECK(relais_wcet_add(&s, (double)(k + 1)));
  }
  relais_wcet_desc d;
  T_CHECK(relais_wcet_describe(&s, &d));
  T_EQ_U(d.n, 100);
  T_NEAR(d.min, 1.0, 1e-12);
  T_NEAR(d.max, 100.0, 1e-12);
  T_NEAR(d.p50, 50.5, 1e-9);
  T_NEAR(d.p99, 99.01, 1e-9);
  relais_wcet_set_free(&s);
}

static void test_json_summary(void) {
  t_section("M40 JSON descriptor: keys of the published summary");
  relais_wcet_desc d = {.n = 3, .min = 1.0, .mean = 2.0, .stdev = 1.0,
                        .p50 = 2.0, .p90 = 2.5, .p99 = 2.9, .p99_9 = 2.99,
                        .max = 3.0};
  char buf[512];
  int n = relais_wcet_desc_json(&d, buf, sizeof buf);
  T_CHECK(n > 0);
  T_CHECK(strstr(buf, "\"n\":3") != NULL);
  T_CHECK(strstr(buf, "\"p99_9\":2.9900") != NULL);
  T_CHECK(strstr(buf, "\"max\":3.0000") != NULL);
  T_CHECK(buf[n - 1] == '}'); /* snprintf length excludes the terminator */
  T_CHECK(buf[n] == '\0');
  T_CHECK(relais_wcet_desc_json(&d, buf, 8) < 0); /* truncation is reported */
  T_CHECK(relais_wcet_desc_json(NULL, buf, sizeof buf) < 0);
}

int main(void) {
  test_segment_names();
  test_percentile_matches_timing_definition();
  test_describe_known_set();
  test_edge_cases();
  test_describe_sorts();
  test_json_summary();
  return t_summary();
}
