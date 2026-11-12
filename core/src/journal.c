/* Relais — the actuator's journal (spec 6.4): one record per tick, one per
 * event, JSON lines. This journal is the authority of the campaign: the
 * verifier, the report and the acceptance criteria all read it.
 *
 * Formatting and parsing live here once, and share the name tables of
 * types.c, so the C verifier and the Python report cannot drift apart.
 * The exact shape is fixed in CONTRACT.md.
 */
#include "relais/journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Field readers over one JSON line: no dependency, no allocation. */
static const char *find_field(const char *line, const char *key) {
  char pattern[40];
  int n = snprintf(pattern, sizeof pattern, "\"%s\":", key);
  if (n <= 0 || (size_t)n >= sizeof pattern) return NULL;
  const char *at = strstr(line, pattern);
  return at != NULL ? at + strlen(pattern) : NULL;
}

static bool read_number(const char *line, const char *key, double *out) {
  const char *at = find_field(line, key);
  if (at == NULL) return false;
  char *end = NULL;
  double value = strtod(at, &end);
  if (end == at) return false;
  *out = value;
  return true;
}

static bool read_bool(const char *line, const char *key, bool *out) {
  const char *at = find_field(line, key);
  if (at == NULL) return false;
  if (strncmp(at, "true", 4) == 0) { *out = true; return true; }
  if (strncmp(at, "false", 5) == 0) { *out = false; return true; }
  return false;
}

static bool read_name(const char *line, const char *key, char *out, size_t cap) {
  const char *at = find_field(line, key);
  if (at == NULL || *at != '"' || cap == 0) return false;
  size_t i = 0;
  for (at++; *at != '\0' && *at != '"' && i + 1 < cap; at++) out[i++] = *at;
  out[i] = '\0';
  return *at == '"';
}

/* The names below are the exact strings types.c writes; they are matched back
 * here so a journal produced by one build is read by another. */
static relais_node node_of(const char *name) {
  if (strcmp(name, "A") == 0) return RELAIS_NODE_A;
  if (strcmp(name, "B") == 0) return RELAIS_NODE_B;
  if (strcmp(name, "C") == 0) return RELAIS_NODE_C;
  if (strcmp(name, "actuator") == 0) return RELAIS_NODE_ACTUATOR;
  return RELAIS_NODE_NONE;
}

static relais_source source_of(const char *name) {
  if (strcmp(name, "holder") == 0) return RELAIS_SRC_HOLDER;
  if (strcmp(name, "shadow") == 0) return RELAIS_SRC_SHADOW;
  if (strcmp(name, "testament") == 0) return RELAIS_SRC_TESTAMENT;
  if (strcmp(name, "fallback") == 0) return RELAIS_SRC_FALLBACK;
  return RELAIS_SRC_NONE;
}

static relais_lamp lamp_of(const char *name) {
  if (strcmp(name, "yellow") == 0) return RELAIS_LAMP_YELLOW;
  if (strcmp(name, "red") == 0) return RELAIS_LAMP_RED;
  return RELAIS_LAMP_OFF;
}

static relais_cause cause_of(const char *name) {
  if (strcmp(name, "node_missing") == 0) return RELAIS_CAUSE_NODE_MISSING;
  if (strcmp(name, "link_lost") == 0) return RELAIS_CAUSE_LINK_LOST;
  if (strcmp(name, "second_uncertified") == 0) return RELAIS_CAUSE_SECOND_UNCERTIFIED;
  if (strcmp(name, "disagreement") == 0) return RELAIS_CAUSE_DISAGREEMENT;
  if (strcmp(name, "fallback") == 0) return RELAIS_CAUSE_FALLBACK;
  if (strcmp(name, "stop_requested") == 0) return RELAIS_CAUSE_STOP_REQUESTED;
  return RELAIS_CAUSE_NONE;
}

static relais_event_kind event_of(const char *name) {
  for (int k = RELAIS_EV_NONE; k <= RELAIS_EV_AUTH_BANNED; k++)
    if (strcmp(relais_event_name((relais_event_kind)k), name) == 0)
      return (relais_event_kind)k;
  return RELAIS_EV_NONE;
}

static bool is_kind(const char *line, const char *kind) {
  char name[16] = "";
  if (!read_name(line, "kind", name, sizeof name)) return false;
  return strcmp(name, kind) == 0;
}

bool relais_journal_is_tick(const char *line) {
  return line != NULL && is_kind(line, "tick");
}

bool relais_journal_is_event(const char *line) {
  return line != NULL && is_kind(line, "event");
}

bool relais_journal_is_header(const char *line) {
  return line != NULL && is_kind(line, "header");
}

int relais_journal_format_header(const relais_jr_header *h, char *buf, size_t cap) {
  if (h == NULL || buf == NULL) return -1;
  const relais_params *p = &h->params;
  int n = snprintf(buf, cap,
                   "{\"kind\":\"header\",\"version\":%u,\"mutant\":%d,"
                   "\"scenario\":\"%s\",\"scenario_id\":%u,"
                   "\"params\":{\"T\":%u,\"p\":%u,\"N\":%u,\"certify\":%u,\"bail\":%u,"
                   "\"eps\":%.6g,\"delta\":%.6g,\"degraded\":%u}}",
                   RELAIS_VERSION, h->mutant, h->scenario, (unsigned)h->scenario_id,
                   (unsigned)p->period_ms, (unsigned)p->patience, (unsigned)p->testament_len,
                   (unsigned)p->certify_ticks, (unsigned)p->bail_ticks, (double)p->epsilon,
                   (double)p->delta, (unsigned)p->degraded_ticks);
  return (n > 0 && (size_t)n < cap) ? n : -1;
}

int relais_journal_format_tick(const relais_jr_tick *r, char *buf, size_t cap) {
  if (r == NULL || buf == NULL) return -1;
  int n = snprintf(buf, cap,
                   "{\"kind\":\"tick\",\"tick\":%llu,\"ns\":%llu,\"mandate\":%u,"
                   "\"holder\":\"%s\",\"source\":\"%s\",\"value\":%.6g,\"pos\":%.6g,"
                   "\"vel\":%.6g,\"setpoint\":%.6g,\"misses\":%u,\"lamp\":\"%s\","
                   "\"cause\":\"%s\",\"certified\":%s,\"certified_node\":\"%s\","
                   "\"stop\":%s}",
                   (unsigned long long)r->tick, (unsigned long long)r->ns,
                   (unsigned)r->mandate, relais_node_name(r->holder),
                   relais_source_name(r->source), (double)r->value, (double)r->pos,
                   (double)r->vel, (double)r->setpoint, (unsigned)r->misses,
                   relais_lamp_name(r->lamp), relais_cause_name(r->cause),
                   r->certified ? "true" : "false", relais_node_name(r->certified_node),
                   r->stop_requested ? "true" : "false");
  return (n > 0 && (size_t)n < cap) ? n : -1;
}

int relais_journal_format_event(const relais_jr_event *r, char *buf, size_t cap) {
  if (r == NULL || buf == NULL) return -1;
  int n = snprintf(buf, cap,
                   "{\"kind\":\"event\",\"tick\":%llu,\"ns\":%llu,\"event\":\"%s\","
                   "\"node\":\"%s\",\"mandate\":%u,\"detail\":%u,\"text\":\"%s\"}",
                   (unsigned long long)r->tick, (unsigned long long)r->ns,
                   relais_event_name(r->kind), relais_node_name(r->node),
                   (unsigned)r->mandate, (unsigned)r->detail, r->text);
  return (n > 0 && (size_t)n < cap) ? n : -1;
}

bool relais_journal_parse_header(const char *line, relais_jr_header *h) {
  if (line == NULL || h == NULL || !relais_journal_is_header(line)) return false;
  memset(h, 0, sizeof *h);
  relais_params_default(&h->params);
  double v = 0.0;
  if (read_number(line, "mutant", &v)) h->mutant = (int)v;
  if (read_number(line, "scenario_id", &v)) h->scenario_id = (uint32_t)v;
  read_name(line, "scenario", h->scenario, sizeof h->scenario);
  if (read_number(line, "T", &v)) h->params.period_ms = (uint32_t)v;
  if (read_number(line, "p", &v)) h->params.patience = (uint8_t)v;
  if (read_number(line, "N", &v)) h->params.testament_len = (uint8_t)v;
  if (read_number(line, "certify", &v)) h->params.certify_ticks = (uint8_t)v;
  if (read_number(line, "bail", &v)) h->params.bail_ticks = (uint8_t)v;
  if (read_number(line, "eps", &v)) h->params.epsilon = (float)v;
  if (read_number(line, "delta", &v)) h->params.delta = (float)v;
  if (read_number(line, "degraded", &v)) h->params.degraded_ticks = (uint32_t)v;
  return relais_params_valid(&h->params, NULL, 0);
}

bool relais_journal_parse_tick(const char *line, relais_jr_tick *r) {
  if (line == NULL || r == NULL || !relais_journal_is_tick(line)) return false;
  memset(r, 0, sizeof *r);
  double v = 0.0;
  char name[24] = "";
  if (!read_number(line, "tick", &v)) return false;
  r->tick = (relais_tick)v;
  if (read_number(line, "ns", &v)) r->ns = (uint64_t)v;
  if (read_number(line, "mandate", &v)) r->mandate = (relais_mandate)v;
  if (read_number(line, "misses", &v)) r->misses = (uint8_t)v;
  if (read_number(line, "value", &v)) r->value = (float)v;
  if (read_number(line, "pos", &v)) r->pos = (float)v;
  if (read_number(line, "vel", &v)) r->vel = (float)v;
  if (read_number(line, "setpoint", &v)) r->setpoint = (float)v;
  if (read_name(line, "holder", name, sizeof name)) r->holder = node_of(name);
  if (read_name(line, "source", name, sizeof name)) r->source = source_of(name);
  if (read_name(line, "lamp", name, sizeof name)) r->lamp = lamp_of(name);
  if (read_name(line, "cause", name, sizeof name)) r->cause = cause_of(name);
  if (read_name(line, "certified_node", name, sizeof name)) r->certified_node = node_of(name);
  if (!read_bool(line, "certified", &r->certified)) return false;
  if (!read_bool(line, "stop", &r->stop_requested)) return false;
  return r->source != RELAIS_SRC_NONE || r->tick == 0;
}

bool relais_journal_parse_event(const char *line, relais_jr_event *r) {
  if (line == NULL || r == NULL || !relais_journal_is_event(line)) return false;
  memset(r, 0, sizeof *r);
  double v = 0.0;
  char name[32] = "";
  if (!read_number(line, "tick", &v)) return false;
  r->tick = (relais_tick)v;
  if (read_number(line, "ns", &v)) r->ns = (uint64_t)v;
  if (read_number(line, "mandate", &v)) r->mandate = (relais_mandate)v;
  if (read_number(line, "detail", &v)) r->detail = (uint32_t)v;
  if (!read_name(line, "event", name, sizeof name)) return false;
  r->kind = event_of(name);
  if (r->kind == RELAIS_EV_NONE) return false;
  if (read_name(line, "node", name, sizeof name)) r->node = node_of(name);
  read_name(line, "text", r->text, sizeof r->text);
  return true;
}
