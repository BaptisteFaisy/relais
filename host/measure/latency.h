/* Relais — host-side latency measurement (mission M02).
 *
 * What it measures: the wall-clock duration of the machine's critical path,
 * "STATUT received -> ORDRE emitted" (PERF-01), the time spent inside the
 * control law (CMD-03) and the frame decode + authenticate step (SEC-05).
 *
 * Why it is NOT in core/: it reads a clock (CLOCK_MONOTONIC) and it writes
 * files, which the core forbids. core/ stays pure; this module is linked by the
 * host processes only (host/node, host/measure). The QNX target keeps its own
 * trace points (rl_trace, see host/timing/procedures-qnx.md, QNX-04).
 *
 * Constraints honoured here:
 *  - no dynamic allocation: every table is a fixed array (PERF-01, ROB-04);
 *  - nothing on the measured path but a clock read and a few integer stores;
 *  - a power-of-two logarithmic histogram, so one recorder covers nanoseconds
 *    to seconds with a bounded memory footprint (OBS-03);
 *  - exact percentiles while the raw window holds every sample, and a
 *    documented conservative estimate from the histogram once it is full;
 *  - a "worst cases" list that keeps the context (tick, source, role) of the
 *    slowest samples, which is what PERF-07 asks to explain.
 *
 * The sample line format is the one host/timing/wcet.py already consumes:
 *   {"kind":"latency","source":"path","role":"A","tick":12,"latency_us":412.318}
 * so M02's output feeds the existing WCET extrapolation without conversion.
 */
#ifndef RELAIS_HOST_LATENCY_H
#define RELAIS_HOST_LATENCY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Power-of-two buckets over microseconds: bucket 0 is "< 1 us", bucket k>=1 is
 * [2^(k-1), 2^k) us. The last bucket saturates (everything above it). */
#define RL_LAT_BUCKETS   24u
#define RL_LAT_NS_PER_US 1000u

/* Raw window: while every sample still fits here, percentiles are exact.
 * 65536 * 8 B = 512 KiB of .bss per recorder (three recorders per node), no
 * malloc; the 40 000-tick campaign of docs/LATENCY.md stays inside it, so every
 * published percentile is an exact one. Past it the histogram takes over and
 * rl_lat_stats.exact says so. */
#define RL_LAT_RAW_MAX   65536u

/* Worst-case keep list (PERF-07 explains the three first entries). */
#define RL_LAT_WORST_MAX 8u

#define RL_LAT_LINE_MAX  512u
#define RL_LAT_NAME_MAX  16u
/* A summary line carries the whole histogram: give it more room. */
#define RL_LAT_SUMMARY_MAX 1024u

/* Which segment of the critical path a sample measures. */
typedef enum {
  RL_LAT_SRC_PATH = 0,   /* STATUT received -> ORDRE emitted (PERF-01) */
  RL_LAT_SRC_LAW = 1,    /* inside the control law (CMD-03) */
  RL_LAT_SRC_DECODE = 2, /* frame decode + MAC check (SEC-05, DET-04) */
  RL_LAT_SRC_COUNT = 3
} rl_lat_source;

/* One measurement, with the context needed to explain it later (PERF-07). */
typedef struct {
  uint64_t ns;          /* duration, nanoseconds */
  uint64_t tick;        /* tick of the STATUT that started it */
  rl_lat_source source; /* which segment */
  char role;            /* 'A', 'B' or '-' when unknown */
} rl_lat_sample;

typedef struct {
  rl_lat_source source;      /* fixed at reset: one recorder per segment */
  char role;                 /* fixed at reset */
  uint64_t count;            /* samples recorded, including dropped raw ones */
  uint32_t raw_n;            /* samples still held in the raw window */
  bool raw_sorted;           /* raw window is sorted (cache for percentiles) */
  uint64_t total_ns;         /* sum, for the mean */
  uint64_t min_ns;
  uint64_t max_ns;
  uint32_t hist[RL_LAT_BUCKETS];
  uint64_t raw[RL_LAT_RAW_MAX];
  rl_lat_sample worst[RL_LAT_WORST_MAX];
  uint8_t worst_n;
} rl_lat_rec;

/* Descriptive statistics of one recorder. */
typedef struct {
  uint64_t count;
  bool empty;   /* count == 0: every other field is meaningless */
  bool exact;   /* percentiles came from the full raw window */
  uint64_t min_ns;
  uint64_t mean_ns;
  uint64_t p50_ns;
  uint64_t p90_ns;
  uint64_t p99_ns;
  uint64_t p999_ns; /* p99.9, the quantile PERF-01 budgets at 1 ms */
  uint64_t max_ns;
} rl_lat_stats;

/* ---- clock ---- */
/* Monotonic nanoseconds. Returns 0 when the clock is unavailable. */
uint64_t rl_lat_now_ns(void);

/* ---- recorder ---- */
void rl_lat_reset(rl_lat_rec *r, rl_lat_source source, char role);
/* Record one duration. Cost: one bucket increment, one raw store, at most
 * RL_LAT_WORST_MAX comparisons. No allocation, no syscall, no I/O. */
void rl_lat_record(rl_lat_rec *r, uint64_t tick, uint64_t ns);
/* Fill a sample with the identity of the recorder (source, role). */
void rl_lat_take(const rl_lat_rec *r, uint64_t tick, uint64_t ns,
                 rl_lat_sample *out);

/* Percentile by nearest rank: p in ]0,1]. Exact while the raw window holds
 * every sample; otherwise the upper bound of the histogram bucket that
 * contains the rank (conservative, and stats.exact says so). */
bool rl_lat_percentile(rl_lat_rec *r, double p, uint64_t *out_ns);
/* p50/p90/p99/p99.9/min/max/mean in one pass (sorts the raw window once). */
void rl_lat_compute_stats(rl_lat_rec *r, rl_lat_stats *out);

/* ---- histogram ---- */
unsigned rl_lat_bucket_of(uint64_t ns);
uint32_t rl_lat_bucket_hits(const rl_lat_rec *r, unsigned bucket);
/* Human label of a bucket: "<1us", "1-2us", "2-4us", ... ">=4194304us". */
int rl_lat_bucket_label(unsigned bucket, char *buf, size_t cap);
/* Upper bound of a bucket, in ns: what a histogram-derived percentile returns
 * (conservative). The last bucket saturates. */
uint64_t rl_lat_bucket_upper_ns(unsigned bucket);

/* ---- worst cases (PERF-07) ---- */
uint8_t rl_lat_worst_count(const rl_lat_rec *r);
const rl_lat_sample *rl_lat_worst_at(const rl_lat_rec *r, uint8_t i);

/* ---- serialisation, journal spirit: one line, stable keys ---- */
int rl_lat_format_sample(const rl_lat_sample *s, char *buf, size_t cap);
int rl_lat_format_summary(const rl_lat_rec *r, rl_lat_stats *st, char *buf,
                          size_t cap);
/* Format and write one sample line to an already open stream. The stream must
 * have been buffered by the caller (rl_lat_open does it): no allocation. */
bool rl_lat_write_sample(FILE *out, const rl_lat_sample *s);
/* Record one duration and append its line when `out` is open. The write happens
 * after the measurement, so it stays off the measured interval; the caller
 * decides when to flush (a buffered stream loses nothing on a clean exit). */
bool rl_lat_record_to(rl_lat_rec *r, FILE *out, uint64_t tick, uint64_t ns);
/* Open a sample file for append-free writing, fully buffered with a static
 * buffer. Returns NULL on error. */
FILE *rl_lat_open(const char *path);

/* ---- reading back (host/measure/relais-latency) ---- */
/* Parse one JSONL sample line. Returns false when the line carries no
 * "latency_us" (headers, summaries, malformed lines). */
bool rl_lat_parse_sample(const char *line, rl_lat_sample *out);
/* True when the line names one of our segments; false when its "source" label
 * is foreign (e.g. "net_rx" from host/timing/synth_samples.py) and was grouped
 * as RL_LAT_SRC_PATH by default. Lets the reader say so instead of hiding it. */
bool rl_lat_line_source_known(const char *line);

/* Names, one place, used by the writer and the reader. */
const char *rl_lat_source_name(rl_lat_source s);
bool rl_lat_source_parse(const char *name, rl_lat_source *out);

#endif /* RELAIS_HOST_LATENCY_H */
