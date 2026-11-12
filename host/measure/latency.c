/* Relais — host-side latency recorder (mission M02): clock, power-of-two
 * histogram, exact percentiles, worst-case keep list.
 *
 * Requirements: PERF-01 (path latency distribution), PERF-07 (the three worst
 * cases with their context), PERF-08 (same tool at a lowered period),
 * OBS-03 (bounded memory, no allocation), ROB-04 (no malloc on the path).
 *
 * JSON (de)serialisation lives in latency_json.c; the recorder itself never
 * touches a file, so it stays usable from a signal-free, allocation-free path.
 */
#include "latency.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- clock ---- */

uint64_t rl_lat_now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- recorder ---- */

void rl_lat_reset(rl_lat_rec *r, rl_lat_source source, char role) {
  if (r == NULL) return;
  memset(r, 0, sizeof *r);
  r->source = source;
  r->role = role != '\0' ? role : '-';
}

unsigned rl_lat_bucket_of(uint64_t ns) {
  if (ns < RL_LAT_NS_PER_US) return 0; /* sub-microsecond: bucket "<1us" */
  unsigned b = 1;
  uint64_t lo = RL_LAT_NS_PER_US;
  while (b + 1 < RL_LAT_BUCKETS && ns >= lo * 2) {
    lo *= 2;
    b++;
  }
  return b;
}

uint64_t rl_lat_bucket_upper_ns(unsigned bucket) {
  if (bucket == 0) return RL_LAT_NS_PER_US;
  if (bucket >= RL_LAT_BUCKETS) bucket = RL_LAT_BUCKETS - 1;
  return (uint64_t)RL_LAT_NS_PER_US << bucket;
}

uint32_t rl_lat_bucket_hits(const rl_lat_rec *r, unsigned bucket) {
  if (r == NULL || bucket >= RL_LAT_BUCKETS) return 0;
  return r->hist[bucket];
}

int rl_lat_bucket_label(unsigned bucket, char *buf, size_t cap) {
  if (buf == NULL || cap == 0) return 0;
  if (bucket >= RL_LAT_BUCKETS) bucket = RL_LAT_BUCKETS - 1;
  int n;
  if (bucket == 0)
    n = snprintf(buf, cap, "<1us");
  else if (bucket + 1 == RL_LAT_BUCKETS)
    n = snprintf(buf, cap, ">=%lluus", (unsigned long long)(1ull << (bucket - 1)));
  else
    n = snprintf(buf, cap, "%llu-%lluus", (unsigned long long)(1ull << (bucket - 1)),
                 (unsigned long long)(1ull << bucket));
  if (n < 0 || (size_t)n >= cap) return 0;
  return n;
}

/* Keep the RL_LAT_WORST_MAX slowest samples, descending, with their context:
 * this is the material PERF-07 asks for ("where did the time go"). */
static void worst_insert(rl_lat_rec *r, uint64_t tick, uint64_t ns) {
  rl_lat_sample s;
  rl_lat_take(r, tick, ns, &s);
  unsigned i = 0;
  while (i < r->worst_n && r->worst[i].ns >= ns) i++;
  if (i == RL_LAT_WORST_MAX) return; /* at least MAX slower samples are kept */
  if (i == r->worst_n) {             /* append: the list is not full here */
    r->worst[r->worst_n++] = s;
    return;
  }
  unsigned last = r->worst_n < RL_LAT_WORST_MAX ? r->worst_n : RL_LAT_WORST_MAX - 1;
  for (unsigned j = last; j > i; j--) r->worst[j] = r->worst[j - 1];
  r->worst[i] = s;
  if (r->worst_n < RL_LAT_WORST_MAX) r->worst_n++;
}

void rl_lat_take(const rl_lat_rec *r, uint64_t tick, uint64_t ns,
                 rl_lat_sample *out) {
  if (out == NULL) return;
  out->ns = ns;
  out->tick = tick;
  out->source = r != NULL ? r->source : RL_LAT_SRC_PATH;
  out->role = r != NULL ? r->role : '-';
}

void rl_lat_record(rl_lat_rec *r, uint64_t tick, uint64_t ns) {
  if (r == NULL) return;
  r->count++;
  r->total_ns += ns;
  if (r->count == 1 || ns < r->min_ns) r->min_ns = ns;
  if (ns > r->max_ns) r->max_ns = ns;
  unsigned b = rl_lat_bucket_of(ns);
  if (r->hist[b] < UINT32_MAX) r->hist[b]++;
  if (r->raw_n < RL_LAT_RAW_MAX) {
    r->raw[r->raw_n++] = ns;
    r->raw_sorted = false;
  }
  worst_insert(r, tick, ns);
}

/* ---- percentiles ---- */

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

/* Nearest rank over the raw window: exact, and only used while the window
 * still holds every sample. */
static bool exact_percentile(rl_lat_rec *r, uint64_t rank, uint64_t *out_ns) {
  if (!r->raw_sorted) {
    qsort(r->raw, r->raw_n, sizeof r->raw[0], cmp_u64);
    r->raw_sorted = true;
  }
  if (rank > r->raw_n) rank = r->raw_n;
  *out_ns = r->raw[rank - 1];
  return true;
}

/* Histogram fallback: the upper bound of the bucket holding the rank, which
 * over-estimates the quantile. Conservative by construction. */
static bool hist_percentile(const rl_lat_rec *r, uint64_t rank, uint64_t *out_ns) {
  uint64_t cum = 0;
  for (unsigned b = 0; b < RL_LAT_BUCKETS; b++) {
    cum += r->hist[b];
    if (cum >= rank) {
      *out_ns = rl_lat_bucket_upper_ns(b);
      return true;
    }
  }
  return false;
}

bool rl_lat_percentile(rl_lat_rec *r, double p, uint64_t *out_ns) {
  if (r == NULL || out_ns == NULL || r->count == 0) return false;
  if (!(p > 0.0)) p = 1.0 / (double)r->count; /* smallest usable quantile */
  if (p > 1.0) p = 1.0;
  uint64_t rank = (uint64_t)ceil(p * (double)r->count);
  if (rank < 1) rank = 1;
  if (rank > r->count) rank = r->count;
  if (r->raw_n == r->count) return exact_percentile(r, rank, out_ns);
  return hist_percentile(r, rank, out_ns);
}

void rl_lat_compute_stats(rl_lat_rec *r, rl_lat_stats *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof *out);
  if (r == NULL) return;
  out->count = r->count;
  if (r->count == 0) {
    out->empty = true;
    return;
  }
  out->empty = false;
  out->exact = r->raw_n == r->count;
  out->min_ns = r->min_ns;
  out->max_ns = r->max_ns;
  out->mean_ns = r->total_ns / r->count;
  (void)rl_lat_percentile(r, 0.50, &out->p50_ns);
  (void)rl_lat_percentile(r, 0.90, &out->p90_ns);
  (void)rl_lat_percentile(r, 0.99, &out->p99_ns);
  (void)rl_lat_percentile(r, 0.999, &out->p999_ns);
}

/* ---- worst cases ---- */

uint8_t rl_lat_worst_count(const rl_lat_rec *r) { return r == NULL ? 0 : r->worst_n; }

const rl_lat_sample *rl_lat_worst_at(const rl_lat_rec *r, uint8_t i) {
  if (r == NULL || i >= r->worst_n) return NULL;
  return &r->worst[i];
}
