/* Relais — fuzz target for the journal (OBS-01, OBS-02, ROB-09, DET-04).
 *
 * The journal is the authority the verifier reads: a line the actuator writes
 * must be readable back, and a line an attacker writes must never be mistaken
 * for a record. Three things are fuzzed here:
 *   1. the six entry points (three classifiers, three parsers) on arbitrary
 *      text, truncated lines, embedded NULs and hostile JSON;
 *   2. the format/parse round trip, which is what makes the C verifier and the
 *      Python report agree (CONTRACT.md);
 *   3. the formatters on arbitrary records and into a deliberately small
 *      buffer, so a write past the caller's cap is caught.
 */
#include "relais/journal.h"

#include <stdio.h>

#include "fuzz_util.h"

#define LINE_CAP ((size_t)RELAIS_JOURNAL_LINE_MAX + 1u)
#define SMALL_CAP 48u /* forces the truncation branch of every formatter */

/* The parsers take a C string: the input is copied and terminated. A NUL inside
 * the input is kept, it is part of what a hostile file may contain. */
static size_t to_line(const uint8_t *data, size_t size, char *line) {
  size_t n = size < (size_t)RELAIS_JOURNAL_LINE_MAX ? size : (size_t)RELAIS_JOURNAL_LINE_MAX;
  memcpy(line, data, n);
  line[n] = '\0';
  return n;
}

static void fuzz_parsers(const char *line) {
  bool is_tick = relais_journal_is_tick(line);
  bool is_event = relais_journal_is_event(line);
  bool is_header = relais_journal_is_header(line);
  /* A line carries one kind at most: the verifier dispatches on it. */
  int kinds = (is_tick ? 1 : 0) + (is_event ? 1 : 0) + (is_header ? 1 : 0);
  fuzz_require(kinds <= 1);

  relais_jr_tick t;
  relais_jr_event e;
  relais_jr_header h;
  memset(&t, 0, sizeof t);
  memset(&e, 0, sizeof e);
  memset(&h, 0, sizeof h);
  /* Called on their own kind and on somebody else's: no parser may trust the
   * caller to have classified the line first. */
  bool tick_ok = relais_journal_parse_tick(line, &t);
  bool event_ok = relais_journal_parse_event(line, &e);
  bool header_ok = relais_journal_parse_header(line, &h);
  fuzz_require(!tick_ok || is_tick);
  fuzz_require(!event_ok || is_event);
  fuzz_require(!header_ok || is_header);
  /* DET-04: a parsed header carries usable parameters, never a default guess. */
  if (header_ok) {
    char why[64] = "";
    fuzz_require(relais_params_valid(&h.params, why, sizeof why));
  }
}

static void derive_tick(fuzz_cur *c, relais_jr_tick *r) {
  memset(r, 0, sizeof *r);
  r->tick = (relais_tick)fuzz_u32(c);
  r->ns = (uint64_t)fuzz_u32(c);
  r->mandate = fuzz_u32(c);
  r->holder = (relais_node)fuzz_below(c, 5u);
  r->source = (relais_source)(1u + fuzz_below(c, 4u)); /* NONE is refused back */
  r->value = fuzz_f32(c);
  r->pos = fuzz_f32(c);
  r->vel = fuzz_f32(c);
  r->setpoint = fuzz_f32(c);
  r->misses = fuzz_u8(c);
  r->lamp = (relais_lamp)fuzz_below(c, 3u);
  r->cause = (relais_cause)fuzz_below(c, 7u);
  r->certified = fuzz_u8(c) != 0u;
  r->certified_node = (relais_node)fuzz_below(c, 5u);
  r->stop_requested = fuzz_u8(c) != 0u;
}

static void check_tick_back(const relais_jr_tick *t, const relais_jr_tick *b) {
  fuzz_require(b->tick == t->tick && b->ns == t->ns && b->mandate == t->mandate);
  fuzz_require(b->holder == t->holder && b->source == t->source);
  fuzz_require(b->lamp == t->lamp && b->cause == t->cause && b->misses == t->misses);
  fuzz_require(b->certified == t->certified && b->certified_node == t->certified_node);
  fuzz_require(b->stop_requested == t->stop_requested);
  fuzz_require(fuzz_close(b->value, t->value) && fuzz_close(b->pos, t->pos));
  fuzz_require(fuzz_close(b->vel, t->vel) && fuzz_close(b->setpoint, t->setpoint));
}

static void derive_event(fuzz_cur *c, relais_jr_event *e) {
  memset(e, 0, sizeof *e);
  e->tick = (relais_tick)fuzz_u32(c);
  e->ns = (uint64_t)fuzz_u32(c);
  e->kind = (relais_event_kind)(1u + fuzz_below(c, 15u)); /* NONE parses back as a failure */
  e->node = (relais_node)fuzz_below(c, 5u);
  e->mandate = fuzz_u32(c);
  e->detail = fuzz_u32(c);
  /* The text field is internal today and the formatter does not escape it: the
   * alphabet is kept JSON-safe here, and the gap is written down in
   * docs/ATTACK-SURFACE.md rather than papered over by the harness. */
  snprintf(e->text, sizeof e->text, "fuzz-%u", (unsigned)fuzz_u32(c));
}

static void round_trip(fuzz_cur *c) {
  char buf[RELAIS_JOURNAL_LINE_MAX];
  relais_jr_tick t;
  derive_tick(c, &t);
  fuzz_require(relais_journal_format_tick(&t, buf, sizeof buf) > 0);
  relais_jr_tick back;
  memset(&back, 0, sizeof back);
  fuzz_require(relais_journal_parse_tick(buf, &back));
  check_tick_back(&t, &back);

  relais_jr_event e;
  derive_event(c, &e);
  fuzz_require(relais_journal_format_event(&e, buf, sizeof buf) > 0);
  relais_jr_event eback;
  memset(&eback, 0, sizeof eback);
  fuzz_require(relais_journal_parse_event(buf, &eback));
  fuzz_require(eback.tick == e.tick && eback.ns == e.ns && eback.kind == e.kind);
  fuzz_require(eback.node == e.node && eback.mandate == e.mandate);
  fuzz_require(eback.detail == e.detail && strcmp(eback.text, e.text) == 0);

  /* A header always carries the default parameters: they are the only ones the
   * formatter can write and still have them read back as valid. */
  relais_jr_header h;
  memset(&h, 0, sizeof h);
  relais_params_default(&h.params);
  h.mutant = (int)fuzz_u8(c);
  h.scenario_id = fuzz_u32(c);
  snprintf(h.scenario, sizeof h.scenario, "fuzz-%u", (unsigned)fuzz_u32(c));
  h.holder_a = (relais_node)fuzz_below(c, 5u);
  h.holder_b = (relais_node)fuzz_below(c, 5u);
  fuzz_require(relais_journal_format_header(&h, buf, sizeof buf) > 0);
  relais_jr_header hback;
  memset(&hback, 0, sizeof hback);
  fuzz_require(relais_journal_parse_header(buf, &hback));
  fuzz_require(hback.mutant == h.mutant && hback.scenario_id == h.scenario_id);
  fuzz_require(strcmp(hback.scenario, h.scenario) == 0);
  fuzz_require(hback.params.patience == h.params.patience);
  fuzz_require(hback.params.testament_len == h.params.testament_len);
}

/* Arbitrary records, arbitrary enums, and a buffer too small on purpose: the
 * formatter must either refuse (-1) or stay inside cap, never write past it. */
static void fuzz_formatters(fuzz_cur *c) {
  char small[SMALL_CAP];
  relais_jr_tick t;
  derive_tick(c, &t);
  t.holder = (relais_node)fuzz_u8(c);       /* out of the enum on purpose */
  t.source = (relais_source)fuzz_u8(c);
  t.lamp = (relais_lamp)fuzz_u8(c);
  t.cause = (relais_cause)fuzz_u8(c);
  int n = relais_journal_format_tick(&t, small, sizeof small);
  fuzz_require(n == -1 || (n > 0 && (size_t)n < sizeof small));

  relais_jr_event e;
  derive_event(c, &e);
  e.kind = (relais_event_kind)fuzz_u8(c);
  e.node = (relais_node)fuzz_u8(c);
  n = relais_journal_format_event(&e, small, sizeof small);
  fuzz_require(n == -1 || (n > 0 && (size_t)n < sizeof small));

  /* NULL is refused, not crashed on: the actuator calls these on every tick. */
  fuzz_require(relais_journal_format_tick(NULL, small, sizeof small) == -1);
  fuzz_require(relais_journal_format_event(&e, NULL, 0) == -1);
  fuzz_require(!relais_journal_parse_tick(NULL, &t));
  fuzz_require(!relais_journal_is_event(NULL));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char line[LINE_CAP];
  (void)to_line(data, size, line);
  fuzz_parsers(line);

  fuzz_cur cur;
  fuzz_cur_init(&cur, data, size);
  round_trip(&cur);
  fuzz_formatters(&cur);
  return 0;
}
