/* Relais — latency report reader (mission M02, PERF-01/PERF-07/PERF-08).
 *
 * Reads the JSONL samples written by `relais-node --latency FILE` (or by any
 * producer of the same format, e.g. host/timing/synth_samples.py) and prints
 * the distribution: n, min, p50, p90, p99, p99.9, max, mean, a power-of-two
 * ASCII histogram, and the worst cases with their context (tick, source, role).
 *
 * Samples are grouped by (source, role) so one file holding both computers and
 * all three measured segments produces one table. No allocation: a fixed table
 * of recorders, one line buffer, one pass.
 *
 * Usage:
 *   relais-latency [--worst N] [--json] FILE.jsonl [FILE...]
 *   relais-latency --help
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "latency.h"

#define MAX_GROUPS 8u
#define LINE_BYTES 1024u
#define BAR_WIDTH  40u

typedef struct {
  rl_lat_rec rec;
  bool used;
} group_t;

static group_t g_groups[MAX_GROUPS];

static group_t *group_for(rl_lat_source source, char role) {
  for (unsigned i = 0; i < MAX_GROUPS; i++) {
    if (!g_groups[i].used) continue;
    if (g_groups[i].rec.source == source && g_groups[i].rec.role == role)
      return &g_groups[i];
  }
  for (unsigned i = 0; i < MAX_GROUPS; i++) {
    if (g_groups[i].used) continue;
    rl_lat_reset(&g_groups[i].rec, source, role);
    g_groups[i].used = true;
    return &g_groups[i];
  }
  return NULL; /* more groups than the fixed table: reported, not silently lost */
}

static void usage(void) {
  fprintf(stderr,
          "usage: relais-latency [--worst N] [--json] FILE.jsonl [FILE...]\n"
          "  --worst N   print the N worst cases per group (default 3, PERF-07)\n"
          "  --json      print one latency_summary line per group instead of\n"
          "              the human table (same keys as the journal)\n");
}

/* One pass over one file: every sample goes to its group. */
static bool read_file(const char *path, unsigned long *lines,
                      unsigned long *samples, unsigned long *dropped,
                      unsigned long *foreign) {
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    fprintf(stderr, "relais-latency: cannot open %s\n", path);
    return false;
  }
  char line[LINE_BYTES];
  while (fgets(line, (int)sizeof line, f) != NULL) {
    (*lines)++;
    rl_lat_sample s;
    if (!rl_lat_parse_sample(line, &s)) continue; /* header, summary, junk */
    if (!rl_lat_line_source_known(line)) (*foreign)++;
    group_t *g = group_for(s.source, s.role);
    if (g == NULL) {
      (*dropped)++;
      continue;
    }
    rl_lat_record(&g->rec, s.tick, s.ns);
    (*samples)++;
  }
  fclose(f);
  return true;
}

static void print_us(uint64_t ns) { printf("%9.3f", (double)ns / 1000.0); }

static void print_table(void) {
  printf("\n%-7s %-5s %8s %9s %9s %9s %9s %9s %9s %9s %-6s\n", "source",
         "role", "n", "min", "p50", "p90", "p99", "p99.9", "max", "mean",
         "exact");
  printf("  (microseconds; nearest-rank percentiles over the raw window)\n");
  for (unsigned i = 0; i < MAX_GROUPS; i++) {
    if (!g_groups[i].used) continue;
    rl_lat_stats st;
    rl_lat_compute_stats(&g_groups[i].rec, &st);
    printf("%-7s %-5c %8llu", rl_lat_source_name(g_groups[i].rec.source),
           g_groups[i].rec.role, (unsigned long long)st.count);
    print_us(st.min_ns);
    print_us(st.p50_ns);
    print_us(st.p90_ns);
    print_us(st.p99_ns);
    print_us(st.p999_ns);
    print_us(st.max_ns);
    print_us(st.mean_ns);
    printf(" %-6s\n", st.exact ? "yes" : "est.");
  }
}

/* ASCII histogram: one bar per non-empty power-of-two bucket. */
static void print_histogram(const rl_lat_rec *r) {
  uint32_t peak = 0;
  for (unsigned b = 0; b < RL_LAT_BUCKETS; b++)
    if (rl_lat_bucket_hits(r, b) > peak) peak = rl_lat_bucket_hits(r, b);
  printf("\nhistogram %s/%c (power-of-two buckets, peak %llu samples)\n",
         rl_lat_source_name(r->source), r->role, (unsigned long long)peak);
  if (peak == 0) return;
  for (unsigned b = 0; b < RL_LAT_BUCKETS; b++) {
    uint32_t hits = rl_lat_bucket_hits(r, b);
    if (hits == 0) continue;
    char label[32];
    rl_lat_bucket_label(b, label, sizeof label);
    unsigned long long width =
        ((unsigned long long)hits * BAR_WIDTH + peak - 1) / peak;
    printf("  %14s %8llu |", label, (unsigned long long)hits);
    for (unsigned long long k = 0; k < width; k++) putchar('#');
    putchar('\n');
  }
}

static void print_worst(const rl_lat_rec *r, unsigned count) {
  uint8_t kept = rl_lat_worst_count(r);
  printf("\nworst cases %s/%c (PERF-07: where the time went)\n",
         rl_lat_source_name(r->source), r->role);
  if (kept == 0) {
    printf("  none\n");
    return;
  }
  for (uint8_t i = 0; i < kept && i < count; i++) {
    const rl_lat_sample *w = rl_lat_worst_at(r, i);
    if (w == NULL) break;
    printf("  %u. %10.3f us at tick %-8llu segment %-6s role %c\n",
           (unsigned)i + 1, (double)w->ns / 1000.0, (unsigned long long)w->tick,
           rl_lat_source_name(w->source), w->role);
  }
}

static void print_json(void) {
  for (unsigned i = 0; i < MAX_GROUPS; i++) {
    if (!g_groups[i].used) continue;
    rl_lat_stats st;
    rl_lat_compute_stats(&g_groups[i].rec, &st);
    char buf[RL_LAT_SUMMARY_MAX];
    if (rl_lat_format_summary(&g_groups[i].rec, &st, buf, sizeof buf) > 0)
      printf("%s\n", buf);
  }
}

int main(int argc, char **argv) {
  unsigned worst = 3;
  bool json = false;
  unsigned files = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      usage();
      return 0;
    } else if (strcmp(a, "--json") == 0) {
      json = true;
    } else if (strcmp(a, "--worst") == 0 && i + 1 < argc) {
      unsigned long v = strtoul(argv[++i], NULL, 10);
      worst = v > RL_LAT_WORST_MAX ? (unsigned)RL_LAT_WORST_MAX : (unsigned)v;
    } else if (a[0] == '-' && a[1] != '\0') {
      usage();
      return 2;
    } else {
      unsigned long lines = 0, samples = 0, dropped = 0, foreign = 0;
      if (!read_file(a, &lines, &samples, &dropped, &foreign)) return 2;
      printf("== %s: %lu lines, %lu samples", a, lines, samples);
      if (lines != samples) printf(" (%lu not samples)", lines - samples);
      if (dropped != 0) printf(" (%lu dropped: group table full)", dropped);
      if (foreign != 0)
        printf(" (%lu with a foreign source label, grouped as path)", foreign);
      printf("\n");
      files++;
    }
  }
  if (files == 0) {
    usage();
    return 2;
  }
  if (json) {
    print_json();
    return 0;
  }
  print_table();
  for (unsigned i = 0; i < MAX_GROUPS; i++) {
    if (!g_groups[i].used) continue;
    print_histogram(&g_groups[i].rec);
    print_worst(&g_groups[i].rec, worst);
  }
  return 0;
}
