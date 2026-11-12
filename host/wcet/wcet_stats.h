/* Relais — descriptive statistics of a latency sample set (mission M40).
 *
 * One implementation shared by the WCET probe and its unit test, so the numbers
 * published by the C probe and by host/wcet/measure.py cannot drift apart.
 *
 * The percentile definition is EXACTLY the one of host/timing/wcet.py
 * (`percentile()`, linear interpolation on an ascending list), so a sample file
 * written by the probe and analysed by host/timing/wcet.py gives the same
 * p50/p90/p99/p99.9 as the probe's own summary (rule 3: zero duplication of the
 * definition, two languages, one formula).
 *
 * Requirements: PERF-01 (1 ms at p99.9, 2 ms worst case), PERF-07 (the worst
 * cases are explained), QNX-04 (the same measurement on the target).
 */
#ifndef RELAIS_WCET_STATS_H
#define RELAIS_WCET_STATS_H

#include <stdbool.h>
#include <stddef.h>

/* Segments of the critical path "STATUT received -> ORDRE emitted".
 * PATH is the whole window; the four others partition the work inside it. */
typedef enum {
  RELAIS_SEG_PATH = 0,  /* decode + node + encode: the published number */
  RELAIS_SEG_DECODE,    /* frame parse + CRC-32C/SipHash verify (relais-net rx) */
  RELAIS_SEG_NODE,      /* protocol state machine, law included (relais-core) */
  RELAIS_SEG_LAW,       /* relais_ctl_step only (relais-ctl) */
  RELAIS_SEG_ENCODE,    /* frame encode + MAC of every emitted frame (net tx) */
  RELAIS_SEG_COUNT
} relais_wcet_seg;

const char *relais_wcet_seg_name(relais_wcet_seg seg);

/* A growable sample set, in microseconds. Host-side code: allocation allowed
 * (core/ stays allocation free, rule 2). */
typedef struct {
  double *v;
  size_t n;
  size_t cap;
} relais_wcet_set;

bool relais_wcet_set_init(relais_wcet_set *s, size_t cap);
void relais_wcet_set_free(relais_wcet_set *s);
/* Append one sample. Returns false when the set is full: the probe counts the
 * refusals and publishes them, so a truncated run is never silent. */
bool relais_wcet_add(relais_wcet_set *s, double us);

typedef struct {
  size_t n;
  double min;
  double mean;
  double stdev;   /* sample standard deviation, 0 when n < 2 */
  double p50;
  double p90;
  double p99;
  double p99_9;
  double max;
} relais_wcet_desc;

/* Sorts the set in place, then computes the descriptors. False when empty. */
bool relais_wcet_describe(relais_wcet_set *s, relais_wcet_desc *out);
/* Linear-interpolation percentile of an ASCENDING list, 0 <= p <= 1. */
double relais_wcet_percentile(const double *sorted, size_t n, double p);
/* One JSON object ("n","min",...,"max"); returns the length written, <0 on
 * truncation. Used by the probe's summary and by the unit test. */
int relais_wcet_desc_json(const relais_wcet_desc *d, char *buf, size_t cap);

#endif /* RELAIS_WCET_STATS_H */
