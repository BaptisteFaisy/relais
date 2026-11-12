/* Relais — descriptive statistics of a latency sample set (mission M40).
 * See wcet_stats.h for the contract and the percentile definition shared with
 * host/timing/wcet.py. Requirements: PERF-01, PERF-07, QNX-04.
 */
#include "wcet_stats.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const SEG_NAMES[RELAIS_SEG_COUNT] = {
    "path", "decode", "node", "law", "encode"};

const char *relais_wcet_seg_name(relais_wcet_seg seg) {
  if ((int)seg < 0 || (int)seg >= RELAIS_SEG_COUNT) return "unknown";
  return SEG_NAMES[(int)seg];
}

bool relais_wcet_set_init(relais_wcet_set *s, size_t cap) {
  if (s == NULL || cap == 0) return false;
  s->v = (double *)malloc(cap * sizeof(double));
  if (s->v == NULL) return false;
  s->n = 0;
  s->cap = cap;
  return true;
}

void relais_wcet_set_free(relais_wcet_set *s) {
  if (s == NULL) return;
  free(s->v);
  s->v = NULL;
  s->n = 0;
  s->cap = 0;
}

bool relais_wcet_add(relais_wcet_set *s, double us) {
  if (s == NULL || s->v == NULL || s->n >= s->cap) return false;
  s->v[s->n++] = us;
  return true;
}

double relais_wcet_percentile(const double *sorted, size_t n, double p) {
  if (sorted == NULL || n == 0) return 0.0;
  if (p <= 0.0) return sorted[0];
  if (p >= 1.0) return sorted[n - 1];
  double rank = p * (double)(n - 1);
  size_t lo = (size_t)floor(rank);
  size_t hi = (size_t)ceil(rank);
  if (lo == hi) return sorted[lo];
  double frac = rank - (double)lo;
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  if (x < y) return -1;
  if (x > y) return 1;
  return 0;
}

bool relais_wcet_describe(relais_wcet_set *s, relais_wcet_desc *out) {
  if (s == NULL || out == NULL || s->n == 0) return false;
  qsort(s->v, s->n, sizeof(double), cmp_double);
  double sum = 0.0;
  for (size_t i = 0; i < s->n; i++) sum += s->v[i];
  out->n = s->n;
  out->min = s->v[0];
  out->max = s->v[s->n - 1];
  out->mean = sum / (double)s->n;
  double acc = 0.0;
  for (size_t i = 0; i < s->n; i++) {
    double d = s->v[i] - out->mean;
    acc += d * d;
  }
  /* Sample standard deviation (n-1), the dispersion published with the table. */
  out->stdev = s->n >= 2 ? sqrt(acc / (double)(s->n - 1)) : 0.0;
  out->p50 = relais_wcet_percentile(s->v, s->n, 0.50);
  out->p90 = relais_wcet_percentile(s->v, s->n, 0.90);
  out->p99 = relais_wcet_percentile(s->v, s->n, 0.99);
  out->p99_9 = relais_wcet_percentile(s->v, s->n, 0.999);
  return true;
}

int relais_wcet_desc_json(const relais_wcet_desc *d, char *buf, size_t cap) {
  if (d == NULL || buf == NULL || cap == 0) return -1;
  int n = snprintf(buf, cap,
                   "{\"n\":%zu,\"min\":%.4f,\"mean\":%.4f,\"stdev\":%.4f,"
                   "\"p50\":%.4f,\"p90\":%.4f,\"p99\":%.4f,\"p99_9\":%.4f,"
                   "\"max\":%.4f}",
                   d->n, d->min, d->mean, d->stdev, d->p50, d->p90, d->p99,
                   d->p99_9, d->max);
  if (n < 0 || (size_t)n >= cap) return -1;
  return n;
}
