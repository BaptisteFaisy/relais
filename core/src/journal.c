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

/* ------------------------------------------------------- chained journal (34) */
/* The chain key used when the caller gives none. It is a fixed, public value on
 * purpose: anybody holding the journal can then verify it, which is what an
 * offline verifier needs. A deployment that must also resist an attacker able to
 * rewrite the file supplies a secret key instead (--chain-key on the simulator,
 * the actuator and relais-verify); the fingerprint is then a MAC that cannot be
 * recomputed without the secret. See docs/SECURITY.md, "Journal chain". */
static const relais_key default_chain_key = {
  { 0x52, 0x4c, 0x53, 0x63, 0x68, 0x61, 0x69, 0x6e, /* "RLSchain" */
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37  /* "01234567" */ },
  true
};

#define CHAIN_LABEL "relais-chain-v1"

static const relais_key *chain_key_of(const relais_key *key) {
  return (key != NULL && key->enabled) ? key : &default_chain_key;
}

void relais_chain_genesis(relais_chain *c) {
  if (c == NULL) return;
  memset(c->bytes, 0, sizeof c->bytes);
}

/* chain(i) = SipHash-2-4(key, label || 0 || chain(i-1) || body(i)). The label
 * keeps this derivation apart from every other use of the same key, and the
 * previous fingerprint is hashed before the body, which is what makes a deleted
 * or inserted record break every record that follows it. */
void relais_chain_advance(relais_chain *c, const char *body, size_t len,
                          const relais_key *key) {
  if (c == NULL || body == NULL) return;
  relais_siphash h;
  relais_siphash_init(&h, chain_key_of(key));
  relais_siphash_update(&h, CHAIN_LABEL, strlen(CHAIN_LABEL));
  relais_siphash_update(&h, "", 1);
  relais_siphash_update(&h, c->bytes, sizeof c->bytes);
  relais_siphash_update(&h, body, len);
  relais_put_u64le(c->bytes, relais_siphash_final(&h));
}

int relais_chain_to_hex(const relais_chain *c, char *out, size_t cap) {
  if (c == NULL) return -1;
  return relais_hex_encode(c->bytes, RELAIS_CHAIN_LEN, out, cap);
}

bool relais_chain_from_hex(relais_chain *c, const char *hex) {
  return c != NULL && relais_hex_decode(hex, c->bytes, RELAIS_CHAIN_LEN);
}

bool relais_chain_equal(const relais_chain *a, const relais_chain *b) {
  if (a == NULL || b == NULL) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < RELAIS_CHAIN_LEN; i++) diff |= (uint8_t)(a->bytes[i] ^ b->bytes[i]);
  return diff == 0u;
}

/* The chain field is the last one of the record, so the last occurrence of its
 * name is the one that counts: a scenario name or a reject text could otherwise
 * carry the same characters and be mistaken for it. */
static const char *chain_field_at(const char *line) {
  const char *found = NULL;
  for (const char *at = strstr(line, RELAIS_CHAIN_FIELD); at != NULL;
       at = strstr(at + 1, RELAIS_CHAIN_FIELD))
    found = at;
  return found;
}

bool relais_chain_read(const char *line, relais_chain *out) {
  if (line == NULL || out == NULL) return false;
  const char *at = chain_field_at(line);
  if (at == NULL) return false;
  char hex[RELAIS_CHAIN_HEX + 1];
  at += strlen(RELAIS_CHAIN_FIELD);
  for (size_t i = 0; i < RELAIS_CHAIN_HEX; i++) {
    if (at[i] == '\0') return false;
    hex[i] = at[i];
  }
  hex[RELAIS_CHAIN_HEX] = '\0';
  if (at[RELAIS_CHAIN_HEX] != '"') return false; /* the field must close there */
  return relais_chain_from_hex(out, hex);
}

int relais_chain_body(const char *line, char *buf, size_t cap) {
  if (line == NULL || buf == NULL) return -1;
  const char *at = chain_field_at(line);
  if (at == NULL) return -1;
  /* The field is the last one: it is introduced by a comma and the object is
   * closed right after the 16 digits. Anything else is a line we did not write,
   * so it is refused rather than guessed at. */
  if (at == line || at[-1] != ',') return -1;
  const char *close = at + strlen(RELAIS_CHAIN_FIELD) + RELAIS_CHAIN_HEX;
  if (close[0] != '"' || close[1] != '}') return -1;
  size_t keep = (size_t)(at - line) - 1u; /* the body, without the comma */
  if (cap < keep + 2u) return -1;
  memcpy(buf, line, keep);
  buf[keep] = '}';
  buf[keep + 1u] = '\0';
  return (int)(keep + 1u);
}

/* One body, one chained line: the shared shape of the three writers below. */
static int chain_line(const char *body, relais_chain *chain, const relais_key *key,
                      char *buf, size_t cap) {
  if (body == NULL || chain == NULL || buf == NULL) return -1;
  relais_chain_advance(chain, body, strlen(body), key);
  char hex[RELAIS_CHAIN_HEX + 1];
  if (relais_chain_to_hex(chain, hex, sizeof hex) != (int)RELAIS_CHAIN_HEX) return -1;
  size_t len = strlen(body);
  if (len < 2u || body[len - 1u] != '}') return -1;
  int n = snprintf(buf, cap, "%.*s,%s%s\"}", (int)(len - 1u), body, RELAIS_CHAIN_FIELD, hex);
  return (n > 0 && (size_t)n < cap) ? n : -1;
}

int relais_journal_chain_header(const relais_jr_header *h, relais_chain *chain,
                                const relais_key *key, char *buf, size_t cap) {
  char body[RELAIS_JOURNAL_LINE_MAX];
  if (relais_journal_format_header(h, body, sizeof body) <= 0) return -1;
  return chain_line(body, chain, key, buf, cap);
}

int relais_journal_chain_tick(const relais_jr_tick *r, relais_chain *chain,
                              const relais_key *key, char *buf, size_t cap) {
  char body[RELAIS_JOURNAL_LINE_MAX];
  if (relais_journal_format_tick(r, body, sizeof body) <= 0) return -1;
  return chain_line(body, chain, key, buf, cap);
}

int relais_journal_chain_event(const relais_jr_event *r, relais_chain *chain,
                               const relais_key *key, char *buf, size_t cap) {
  char body[RELAIS_JOURNAL_LINE_MAX];
  if (relais_journal_format_event(r, body, sizeof body) <= 0) return -1;
  return chain_line(body, chain, key, buf, cap);
}

void relais_chain_reader_init(relais_chain_reader *r) {
  if (r == NULL) return;
  memset(r, 0, sizeof *r);
  relais_chain_genesis(&r->expected);
}

relais_chain_status relais_chain_reader_step(relais_chain_reader *r, const char *line,
                                             const relais_key *key, char *why,
                                             size_t why_cap) {
  if (r == NULL || line == NULL) return RELAIS_CHAIN_BREAK;
  if (line[strspn(line, " \t\r\n")] == '\0') return RELAIS_CHAIN_ABSENT; /* not a record */
  r->records++;
  relais_chain got;
  if (!relais_chain_read(line, &got)) {
    r->absent++;
    if (why != NULL && why_cap > 0 && r->started)
      snprintf(why, why_cap, "record %zu carries no chain field", r->records);
    /* Before the chain starts this is an older journal, which stays readable.
     * After it started, a record without a fingerprint is a record that was
     * rewritten or inserted: the chain is broken. */
    return r->started ? RELAIS_CHAIN_BREAK : RELAIS_CHAIN_ABSENT;
  }
  char body[RELAIS_JOURNAL_CHAIN_LINE_MAX];
  const int body_len = relais_chain_body(line, body, sizeof body);
  if (body_len < 0) {
    if (why != NULL && why_cap > 0)
      snprintf(why, why_cap, "record %zu has a malformed chain field", r->records);
    return RELAIS_CHAIN_BREAK;
  }
  relais_chain want = r->expected;
  relais_chain_advance(&want, body, (size_t)body_len, key);
  if (!relais_chain_equal(&want, &got)) {
    if (why != NULL && why_cap > 0) {
      char a[RELAIS_CHAIN_HEX + 1], b[RELAIS_CHAIN_HEX + 1];
      relais_chain_to_hex(&want, a, sizeof a);
      relais_chain_to_hex(&got, b, sizeof b);
      snprintf(why, why_cap, "record %zu: chain expects %s, found %s", r->records, a, b);
    }
    return RELAIS_CHAIN_BREAK;
  }
  r->expected = got;
  r->chained++;
  r->started = true;
  return RELAIS_CHAIN_OK;
}
