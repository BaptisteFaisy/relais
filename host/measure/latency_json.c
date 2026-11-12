/* Relais — JSON (de)serialisation of the M02 latency samples.
 *
 * One sample per line, keys in a fixed order, in the spirit of the actuator's
 * journal (core/src/journal.c): a reviewer can grep, and host/timing/wcet.py
 * can consume the file as-is because the value key is "latency_us"
 * (PERF-01, OBS-03).
 *
 *   {"kind":"latency","source":"path","role":"A","tick":12,"latency_us":412.318}
 *   {"kind":"latency_summary","source":"path","role":"A","n":4000,...}
 *
 * No allocation: formatting fills caller buffers, parsing reads in place.
 */
#include "latency.h"

#include <stdlib.h>
#include <string.h>

static const char *const g_source_names[RL_LAT_SRC_COUNT] = {"path", "law",
                                                             "decode"};

const char *rl_lat_source_name(rl_lat_source s) {
  if ((unsigned)s >= (unsigned)RL_LAT_SRC_COUNT) return "unknown";
  return g_source_names[(unsigned)s];
}

bool rl_lat_source_parse(const char *name, rl_lat_source *out) {
  if (name == NULL || out == NULL) return false;
  for (unsigned i = 0; i < (unsigned)RL_LAT_SRC_COUNT; i++) {
    if (strcmp(name, g_source_names[i]) == 0) {
      *out = (rl_lat_source)i;
      return true;
    }
  }
  return false;
}

/* ---- writing ---- */

int rl_lat_format_sample(const rl_lat_sample *s, char *buf, size_t cap) {
  if (s == NULL || buf == NULL || cap == 0) return 0;
  int n = snprintf(buf, cap,
                   "{\"kind\":\"latency\",\"source\":\"%s\",\"role\":\"%c\","
                   "\"tick\":%llu,\"latency_us\":%.3f}",
                   rl_lat_source_name(s->source), s->role != '\0' ? s->role : '-',
                   (unsigned long long)s->tick,
                   (double)s->ns / (double)RL_LAT_NS_PER_US);
  if (n < 0 || (size_t)n >= cap) return 0;
  return n;
}

/* The histogram as a JSON array of counts, built once, no allocation. */
static int format_hist(const rl_lat_rec *r, char *buf, size_t cap) {
  size_t used = 0;
  for (unsigned b = 0; b < RL_LAT_BUCKETS; b++) {
    int n = snprintf(buf + used, cap - used, "%s%u", b == 0 ? "" : ",",
                     (unsigned)r->hist[b]);
    if (n < 0 || (size_t)n >= cap - used) return 0;
    used += (size_t)n;
  }
  return (int)used;
}

int rl_lat_format_summary(const rl_lat_rec *r, rl_lat_stats *st, char *buf,
                          size_t cap) {
  if (r == NULL || st == NULL || buf == NULL || cap == 0) return 0;
  char hist[RL_LAT_BUCKETS * 12u];
  if (format_hist(r, hist, sizeof hist) <= 0) return 0;
  double k = (double)RL_LAT_NS_PER_US;
  int n = snprintf(buf, cap,
                   "{\"kind\":\"latency_summary\",\"source\":\"%s\",\"role\":\"%c\","
                   "\"n\":%llu,\"exact\":%s,\"min_us\":%.3f,\"mean_us\":%.3f,"
                   "\"p50_us\":%.3f,\"p90_us\":%.3f,\"p99_us\":%.3f,"
                   "\"p999_us\":%.3f,\"max_us\":%.3f,\"hist\":[%s]}",
                   rl_lat_source_name(r->source), r->role != '\0' ? r->role : '-',
                   (unsigned long long)st->count, st->exact ? "true" : "false",
                   (double)st->min_ns / k, (double)st->mean_ns / k,
                   (double)st->p50_ns / k, (double)st->p90_ns / k,
                   (double)st->p99_ns / k, (double)st->p999_ns / k,
                   (double)st->max_ns / k, hist);
  if (n < 0 || (size_t)n >= cap) return 0;
  return n;
}

/* One static buffer, so one stream at a time per process: relais-node opens
 * exactly one sample file. Full buffering keeps the write cost off the
 * measured path (a fwrite of ~90 bytes hits memory, not the kernel). */
static char g_lat_stream_buf[8192];

FILE *rl_lat_open(const char *path) {
  if (path == NULL || path[0] == '\0') return NULL;
  FILE *f = fopen(path, "w");
  if (f == NULL) return NULL;
  if (setvbuf(f, g_lat_stream_buf, _IOFBF, sizeof g_lat_stream_buf) != 0) {
    fclose(f);
    return NULL;
  }
  return f;
}

bool rl_lat_write_sample(FILE *out, const rl_lat_sample *s) {
  char line[RL_LAT_LINE_MAX];
  if (out == NULL) return false;
  int n = rl_lat_format_sample(s, line, sizeof line - 1);
  if (n <= 0) return false;
  line[n] = '\n';
  return fwrite(line, 1, (size_t)n + 1, out) == (size_t)n + 1;
}

bool rl_lat_record_to(rl_lat_rec *r, FILE *out, uint64_t tick, uint64_t ns) {
  rl_lat_sample s;
  if (r == NULL) return false;
  rl_lat_record(r, tick, ns); /* the measurement is closed before any I/O */
  if (out == NULL) return true;
  rl_lat_take(r, tick, ns, &s);
  return rl_lat_write_sample(out, &s);
}

/* ---- reading ---- */

/* Locate `"key":` and return a pointer just after the colon, skipping the
 * blanks a pretty-printing writer may insert (json.dumps uses ": "). */
static bool find_key(const char *line, const char *key, const char **after) {
  char pat[RL_LAT_NAME_MAX + 8u];
  int n = snprintf(pat, sizeof pat, "\"%s\":", key);
  if (n < 0 || (size_t)n >= sizeof pat) return false;
  const char *p = strstr(line, pat);
  if (p == NULL) return false;
  p += (size_t)n;
  while (*p == ' ' || *p == '\t') p++;
  *after = p;
  return true;
}

static bool find_num(const char *line, const char *key, double *out) {
  const char *p = NULL;
  if (!find_key(line, key, &p)) return false;
  char *end = NULL;
  double v = strtod(p, &end);
  if (end == p) return false;
  *out = v;
  return true;
}

static bool find_str(const char *line, const char *key, char *out, size_t cap) {
  const char *p = NULL;
  if (cap == 0 || !find_key(line, key, &p) || *p != '"') return false;
  p++;
  size_t i = 0;
  while (*p != '\0' && *p != '"' && i + 1 < cap) out[i++] = *p++;
  out[i] = '\0';
  return i > 0;
}

bool rl_lat_line_source_known(const char *line) {
  char name[RL_LAT_NAME_MAX];
  rl_lat_source src = RL_LAT_SRC_PATH;
  if (line == NULL || !find_str(line, "source", name, sizeof name)) return true;
  return rl_lat_source_parse(name, &src);
}

bool rl_lat_parse_sample(const char *line, rl_lat_sample *out) {
  double us = 0.0;
  if (line == NULL || out == NULL) return false;
  if (!find_num(line, "latency_us", &us) || !(us >= 0.0)) return false;
  memset(out, 0, sizeof *out);
  out->ns = (uint64_t)(us * (double)RL_LAT_NS_PER_US + 0.5);
  double tick = 0.0;
  if (find_num(line, "tick", &tick) && tick >= 0.0) out->tick = (uint64_t)tick;
  char name[RL_LAT_NAME_MAX];
  if (find_str(line, "source", name, sizeof name))
    (void)rl_lat_source_parse(name, &out->source);
  char role[4];
  if (find_str(line, "role", role, sizeof role) && role[0] != '\0')
    out->role = role[0];
  if (out->role == '\0') out->role = '-';
  return true;
}
