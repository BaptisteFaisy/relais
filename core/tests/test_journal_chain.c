/* Relais — unit tests of the chained journal (idea 34, OBS-07).
 *
 * What is proved here:
 *   a record carries the fingerprint of the record before it plus its own
 *   content, so editing, deleting, inserting or reordering a record afterwards
 *   is detected and the first break is named;
 *   a journal written before the chain existed, with no "chain" field at all,
 *   is still read: the field is an addition, not a new format (non-regression);
 *   the chain is keyed, and a journal chained under a secret key cannot be
 *   recomputed by anybody who does not hold it.
 *
 * Everything runs on buffers the test owns: no file, no clock, no allocation.
 */
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#define LINE_CAP RELAIS_JOURNAL_CHAIN_LINE_MAX
#define LINES 12

static const char *SECRET_CHAIN_KEY = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
static const char *OTHER_CHAIN_KEY = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";

static relais_jr_tick make_tick(relais_tick tick, float value) {
  relais_jr_tick r;
  memset(&r, 0, sizeof r);
  r.tick = tick;
  r.ns = 0;
  r.mandate = 1;
  r.holder = RELAIS_NODE_A;
  r.source = RELAIS_SRC_HOLDER;
  r.value = value;
  r.pos = value;
  r.vel = 0.0f;
  r.setpoint = value;
  r.lamp = RELAIS_LAMP_OFF;
  r.cause = RELAIS_CAUSE_NONE;
  r.certified = true;
  r.certified_node = RELAIS_NODE_B;
  return r;
}

static relais_jr_event make_event(relais_tick tick) {
  relais_jr_event e;
  memset(&e, 0, sizeof e);
  e.tick = tick;
  e.kind = RELAIS_EV_MANDATE_GRANTED;
  e.node = RELAIS_NODE_A;
  e.mandate = 1;
  snprintf(e.text, sizeof e.text, "%s", relais_event_name(e.kind));
  return e;
}

/* A journal of the shape the actuator writes: a header, then the events of a
 * tick before its record. Returns the number of lines. */
static size_t build_journal(char lines[][LINE_CAP], size_t cap, const relais_key *key,
                            uint32_t ticks) {
  relais_params params;
  relais_params_default(&params);
  relais_jr_header h;
  memset(&h, 0, sizeof h);
  h.params = params;
  h.mutant = 0;
  h.scenario_id = 1;
  snprintf(h.scenario, sizeof h.scenario, "chain-test");
  relais_chain chain;
  relais_chain_genesis(&chain);
  size_t n = 0;
  if (n < cap && relais_journal_chain_header(&h, &chain, key, lines[n], LINE_CAP) > 0) n++;
  for (uint32_t t = 1; t <= ticks && n + 1 < cap; t++) {
    const relais_jr_event e = make_event((relais_tick)t);
    if (relais_journal_chain_event(&e, &chain, key, lines[n], LINE_CAP) > 0) n++;
    const relais_jr_tick r = make_tick((relais_tick)t, 0.25f);
    if (relais_journal_chain_tick(&r, &chain, key, lines[n], LINE_CAP) > 0) n++;
  }
  return n;
}

/* Walk lines with the reader; return the index of the first break, or n. */
static size_t first_break(char lines[][LINE_CAP], size_t n, const relais_key *key,
                          char *why, size_t why_cap, relais_chain_reader *out) {
  relais_chain_reader reader;
  relais_chain_reader_init(&reader);
  for (size_t i = 0; i < n; i++) {
    if (relais_chain_reader_step(&reader, lines[i], key, why, why_cap) == RELAIS_CHAIN_BREAK) {
      if (out != NULL) *out = reader;
      return i;
    }
  }
  if (out != NULL) *out = reader;
  return n;
}

void test_chain_fingerprint_is_a_pure_function_of_the_record(void) {
  t_section("idea 34 the fingerprint covers the previous one and the content");
  const char *record = "{\"kind\":\"tick\",\"tick\":1}";
  relais_chain a, b;
  relais_chain_genesis(&a);
  for (size_t i = 0; i < RELAIS_CHAIN_LEN; i++) T_EQ_U(a.bytes[i], 0u);
  relais_chain_advance(&a, record, strlen(record), NULL);
  relais_chain_genesis(&b);
  relais_chain_advance(&b, record, strlen(record), NULL);
  T_CHECK(relais_chain_equal(&a, &b)); /* same input, same fingerprint */

  /* One byte changed anywhere gives another fingerprint. */
  const char *body = "{\"kind\":\"tick\",\"tick\":1234,\"value\":0.5}";
  const size_t len = strlen(body);
  relais_chain base, other;
  relais_chain_genesis(&base);
  relais_chain_advance(&base, body, len, NULL);
  long different = 0;
  for (size_t at = 0; at < len; at++) {
    char edited[64];
    memcpy(edited, body, len + 1u);
    edited[at] = (char)(edited[at] + 1);
    relais_chain_genesis(&other);
    relais_chain_advance(&other, edited, len, NULL);
    if (!relais_chain_equal(&base, &other)) different++;
  }
  T_EQ_U((uint64_t)different, (uint64_t)len);

  /* The order of two records matters: a chain is a sequence, not a set. */
  relais_chain ab, ba;
  relais_chain_genesis(&ab);
  relais_chain_advance(&ab, "first", 5, NULL);
  relais_chain_advance(&ab, "second", 6, NULL);
  relais_chain_genesis(&ba);
  relais_chain_advance(&ba, "second", 6, NULL);
  relais_chain_advance(&ba, "first", 5, NULL);
  T_CHECK(!relais_chain_equal(&ab, &ba));
  /* A NULL key means the built-in public one, and giving it explicitly is the
   * same thing: one derivation, not two. */
  relais_key def;
  T_CHECK(relais_key_from_hex(&def, "524c53636861696e3031323334353637"));
  relais_chain explicit_key;
  relais_chain_genesis(&explicit_key);
  relais_chain_advance(&explicit_key, body, len, &def);
  T_CHECK(relais_chain_equal(&base, &explicit_key));
}

void test_chain_hex_form_round_trips(void) {
  t_section("idea 34 the hexadecimal form of a fingerprint");
  relais_chain c, back;
  relais_chain_genesis(&c);
  relais_chain_advance(&c, "abc", 3, NULL);
  char hex[RELAIS_CHAIN_HEX + 1];
  T_EQ_U(relais_chain_to_hex(&c, hex, sizeof hex), (uint64_t)RELAIS_CHAIN_HEX);
  T_CHECK(strlen(hex) == RELAIS_CHAIN_HEX);
  T_CHECK(relais_chain_from_hex(&back, hex));
  T_CHECK(relais_chain_equal(&c, &back));
  /* Refusals: a short buffer, a short string, a non-hexadecimal character. */
  T_EQ_U(relais_chain_to_hex(&c, hex, RELAIS_CHAIN_HEX), -1);
  T_CHECK(!relais_chain_from_hex(&back, "0123456789abcde"));
  T_CHECK(!relais_chain_from_hex(&back, "0123456789abcdeg"));
  T_CHECK(!relais_chain_from_hex(NULL, hex));
  T_EQ_U(relais_chain_to_hex(NULL, hex, sizeof hex), -1);
  T_CHECK(!relais_chain_equal(&c, NULL));
}

/* A chained line is still a journal line: the readers that do not know the
 * "chain" key parse it exactly as before. */
void test_chain_field_does_not_break_the_existing_readers(void) {
  t_section("idea 34 a chained record is still a readable record");
  char lines[LINES][LINE_CAP];
  const size_t n = build_journal(lines, LINES, NULL, 4);
  T_CHECK(n >= 9);
  T_CHECK(relais_journal_is_header(lines[0]));
  T_CHECK(relais_journal_is_event(lines[1]));
  T_CHECK(relais_journal_is_tick(lines[2]));
  relais_jr_header h;
  relais_jr_tick r;
  relais_jr_event e;
  T_CHECK(relais_journal_parse_header(lines[0], &h));
  T_EQ_U(h.params.period_ms, 5u);
  T_CHECK(strcmp(h.scenario, "chain-test") == 0);
  T_CHECK(relais_journal_parse_event(lines[1], &e));
  T_EQ_U(e.kind, RELAIS_EV_MANDATE_GRANTED);
  T_CHECK(relais_journal_parse_tick(lines[2], &r));
  T_EQ_U(r.tick, 1u);
  T_EQ_U(r.source, RELAIS_SRC_HOLDER);
  T_CHECK(r.certified);
  T_EQ_U(r.certified_node, RELAIS_NODE_B);
  /* The field is the last one, 16 digits, and nothing follows it. */
  relais_chain c;
  T_CHECK(relais_chain_read(lines[2], &c));
  T_CHECK(strstr(lines[2], RELAIS_CHAIN_FIELD) != NULL);
  const size_t len = strlen(lines[2]);
  T_CHECK(len > RELAIS_CHAIN_HEX + 3u);
  T_CHECK(lines[2][len - 1u] == '}' && lines[2][len - 2u] == '"');
  /* The body the fingerprint covers is the record the plain formatter writes. */
  char body[LINE_CAP];
  const int body_len = relais_chain_body(lines[2], body, sizeof body);
  T_CHECK(body_len > 0);
  relais_jr_tick same = make_tick(1, 0.25f);
  char plain[LINE_CAP];
  const int plain_len = relais_journal_format_tick(&same, plain, sizeof plain);
  T_EQ_U((uint64_t)body_len, (uint64_t)plain_len);
  T_CHECK(strcmp(body, plain) == 0);
}

/* Change one digit of a record without touching its fingerprint: the forgery the
 * chain exists to reveal. */
static void alter_value(char *line) {
  char *at = strstr(line, "\"value\":");
  T_CHECK(at != NULL);
  if (at == NULL) return;
  at += 8;
  *at = (char)(*at == '0' ? '1' : '0');
}

void test_chain_locates_the_first_break(void) {
  t_section("idea 34 an edit, a deletion, an insertion is located");
  char lines[LINES][LINE_CAP];
  char tampered[LINES][LINE_CAP];
  const size_t n = build_journal(lines, LINES, NULL, 4);
  char why[192] = "";
  relais_chain_reader reader;
  T_EQ_U(first_break(lines, n, NULL, why, sizeof why, &reader), (uint64_t)n);
  T_EQ_U(reader.records, (uint64_t)n);
  T_EQ_U(reader.chained, (uint64_t)n);
  T_EQ_U(reader.absent, 0u);

  /* One digit of one record changed: the break is that record, and the message
   * names both the fingerprint expected and the one found. */
  memcpy(tampered, lines, sizeof tampered);
  alter_value(tampered[4]);
  why[0] = '\0';
  T_EQ_U(first_break(tampered, n, NULL, why, sizeof why, NULL), 4u);
  T_CHECK(strstr(why, "record 5") != NULL);
  T_CHECK(strstr(why, "chain expects") != NULL);

  /* A record deleted: the next one no longer continues the chain. */
  memcpy(tampered, lines, sizeof tampered);
  for (size_t i = 4; i + 1 < n; i++) memcpy(tampered[i], tampered[i + 1], LINE_CAP);
  T_EQ_U(first_break(tampered, n - 1u, NULL, why, sizeof why, NULL), 4u);

  /* A record inserted (here, a copy of the previous one): same verdict. */
  memcpy(tampered, lines, sizeof tampered);
  for (size_t i = n - 1u; i > 4u; i--) memcpy(tampered[i], tampered[i - 1u], LINE_CAP);
  memcpy(tampered[4], lines[3], LINE_CAP);
  T_EQ_U(first_break(tampered, n, NULL, why, sizeof why, NULL), 4u);

  /* Two records swapped: the chain is a sequence, the order is part of it. */
  memcpy(tampered, lines, sizeof tampered);
  memcpy(tampered[2], lines[3], LINE_CAP);
  memcpy(tampered[3], lines[2], LINE_CAP);
  T_EQ_U(first_break(tampered, n, NULL, why, sizeof why, NULL), 2u);

  /* The last record edited is caught too: no part of the file is exempt. */
  memcpy(tampered, lines, sizeof tampered);
  alter_value(tampered[n - 1u]);
  T_EQ_U(first_break(tampered, n, NULL, why, sizeof why, NULL), (uint64_t)(n - 1u));
}

/* Non-regression: a journal written before the chain existed has no "chain"
 * field anywhere. It is still parsed, and the reader says there was nothing to
 * verify instead of crying tamper. */
void test_journal_without_chain_still_reads(void) {
  t_section("idea 34 an older journal without a chain is still read");
  relais_params params;
  relais_params_default(&params);
  relais_jr_header h;
  memset(&h, 0, sizeof h);
  h.params = params;
  h.scenario_id = 3;
  snprintf(h.scenario, sizeof h.scenario, "legacy");
  char lines[LINES][LINE_CAP];
  size_t n = 0;
  T_CHECK(relais_journal_format_header(&h, lines[n++], LINE_CAP) > 0);
  for (uint32_t t = 1; t <= 4 && n + 1 < LINES; t++) {
    const relais_jr_event e = make_event((relais_tick)t);
    T_CHECK(relais_journal_format_event(&e, lines[n++], LINE_CAP) > 0);
    const relais_jr_tick r = make_tick((relais_tick)t, 0.25f);
    T_CHECK(relais_journal_format_tick(&r, lines[n++], LINE_CAP) > 0);
  }
  char why[192] = "untouched";
  relais_chain_reader reader;
  T_EQ_U(first_break(lines, n, NULL, why, sizeof why, &reader), (uint64_t)n);
  T_EQ_U(reader.chained, 0u);
  T_EQ_U(reader.absent, (uint64_t)n);
  T_CHECK(!reader.started);
  T_CHECK(strcmp(why, "untouched") == 0); /* no break was reported */
  T_CHECK(strstr(lines[2], RELAIS_CHAIN_FIELD) == NULL);
  relais_jr_tick r;
  T_CHECK(relais_journal_parse_tick(lines[2], &r)); /* still a readable record */
  T_EQ_U(r.tick, 1u);
  T_CHECK(!relais_chain_read(lines[2], &reader.expected));

  /* A journal that started a chain and then lost the field is a break: after the
   * first fingerprint, every record must carry one. */
  char mixed[LINES][LINE_CAP];
  const size_t m = build_journal(mixed, LINES, NULL, 2);
  memcpy(mixed[3], lines[3], LINE_CAP); /* an unchained record inside a chain */
  why[0] = '\0';
  T_EQ_U(first_break(mixed, m, NULL, why, sizeof why, NULL), 3u);
  T_CHECK(strstr(why, "carries no chain field") != NULL);
}

/* The chain is keyed. With the built-in public key anybody holding the file can
 * recompute a chain after editing it; with a secret key nobody can, which is why
 * the writers and relais-verify both accept one. */
void test_chain_key_is_part_of_the_fingerprint(void) {
  t_section("idea 34 a secret chain key cannot be recomputed without it");
  relais_key secret, other, disabled;
  T_CHECK(relais_key_from_hex(&secret, SECRET_CHAIN_KEY));
  T_CHECK(relais_key_from_hex(&other, OTHER_CHAIN_KEY));
  relais_key_clear(&disabled);
  char lines[LINES][LINE_CAP];
  const size_t n = build_journal(lines, LINES, &secret, 3);
  char why[192] = "";
  T_EQ_U(first_break(lines, n, &secret, why, sizeof why, NULL), (uint64_t)n);
  /* Verified with the public default, or with another key: broken from record 1,
   * because the very first fingerprint already depends on the key. */
  T_EQ_U(first_break(lines, n, NULL, why, sizeof why, NULL), 0u);
  T_EQ_U(first_break(lines, n, &other, why, sizeof why, NULL), 0u);
  /* A key that is not enabled is no key: the built-in one is used. */
  char plain[LINES][LINE_CAP];
  const size_t m = build_journal(plain, LINES, NULL, 3);
  T_EQ_U(m, (uint64_t)n);
  T_EQ_U(first_break(plain, m, &disabled, why, sizeof why, NULL), (uint64_t)m);
  T_EQ_U(first_break(lines, n, &disabled, why, sizeof why, NULL), 0u);
  /* And the two journals differ record by record: the key changes every value. */
  long differing = 0;
  for (size_t i = 0; i < n; i++)
    if (strcmp(lines[i], plain[i]) != 0) differing++;
  T_EQ_U((uint64_t)differing, (uint64_t)n);
}

/* Build a line the formatter would never produce: a body, the chain field of a
 * fingerprint computed over that body, then a caller-chosen ending. */
static int build_hand_made_line(char *out, size_t cap, const char *body, const char *ending,
                                const relais_key *key) {
  const size_t body_len = strlen(body);
  if (body_len < 2u || body[body_len - 1u] != '}') return -1;
  relais_chain c;
  relais_chain_genesis(&c);
  relais_chain_advance(&c, body, body_len, key); /* the fingerprint covers it all */
  char hex[RELAIS_CHAIN_HEX + 1];
  if (relais_chain_to_hex(&c, hex, sizeof hex) != (int)RELAIS_CHAIN_HEX) return -1;
  size_t at = body_len - 1u;
  if (cap < at + strlen(RELAIS_CHAIN_FIELD) + RELAIS_CHAIN_HEX + strlen(ending) + 2u)
    return -1;
  memcpy(out, body, at);
  memcpy(out + at, ",", 1u);
  at++;
  memcpy(out + at, RELAIS_CHAIN_FIELD, strlen(RELAIS_CHAIN_FIELD));
  at += strlen(RELAIS_CHAIN_FIELD);
  memcpy(out + at, hex, RELAIS_CHAIN_HEX);
  at += RELAIS_CHAIN_HEX;
  memcpy(out + at, ending, strlen(ending) + 1u);
  return (int)(at + strlen(ending));
}

/* What a reader meets in a real file: line endings, blank lines, and a field
 * that was not written by the formatter. */
void test_chain_reader_tolerates_a_real_file(void) {
  t_section("idea 34 the reader meets line endings and odd fields");
  char lines[LINES][LINE_CAP];
  const size_t n = build_journal(lines, LINES, NULL, 3);
  char with_endings[LINES + 2][LINE_CAP];
  relais_chain_reader reader;
  relais_chain_reader_init(&reader);
  /* A file read line by line keeps the newline: the chain ignores it. */
  size_t at = 0;
  for (size_t i = 0; i < n; i++) {
    const size_t l = strlen(lines[i]);
    T_CHECK(l + 2u <= LINE_CAP);
    memcpy(with_endings[at], lines[i], l);
    with_endings[at][l] = '\n';
    with_endings[at][l + 1u] = '\0';
    at++;
    if (i == 1) memcpy(with_endings[at++], "\n", 2u); /* a blank line */
  }
  char why[192] = "";
  size_t blanks = 0;
  for (size_t i = 0; i < at; i++) {
    const relais_chain_status st =
        relais_chain_reader_step(&reader, with_endings[i], NULL, why, sizeof why);
    T_CHECK(st != RELAIS_CHAIN_BREAK);
    if (st == RELAIS_CHAIN_ABSENT) blanks++;
  }
  T_EQ_U(reader.chained, (uint64_t)n);
  T_EQ_U(blanks, 1u); /* the blank line is not a record */
  T_EQ_U(reader.records, (uint64_t)n);
  T_CHECK(relais_chain_reader_step(NULL, with_endings[0], NULL, NULL, 0) ==
          RELAIS_CHAIN_BREAK);
  T_CHECK(relais_chain_reader_step(&reader, NULL, NULL, NULL, 0) == RELAIS_CHAIN_BREAK);

  /* A chain field that is not the last one is refused rather than guessed at:
   * the fingerprint would not cover what follows it. */
  char odd[LINE_CAP];
  const char *body = "{\"kind\":\"tick\",\"tick\":1,\"stop\":false}";
  T_CHECK(build_hand_made_line(odd, sizeof odd, body, "\",\"extra\":1}", NULL) > 0);
  relais_chain_reader_init(&reader);
  why[0] = '\0';
  T_CHECK(relais_chain_reader_step(&reader, odd, NULL, why, sizeof why) ==
          RELAIS_CHAIN_BREAK);
  T_CHECK(strstr(why, "malformed chain field") != NULL);

  /* A record whose text contains the characters of the field name: the last
  * occurrence is the fingerprint, so the decoy changes nothing. The formatter
  * never produces this line; it is what a hand-edited file could look like. */
  char decoy[LINE_CAP];
  const char *inner =
      "{\"kind\":\"event\",\"tick\":2,\"text\":\"x\",\"chain\":\"ffffffffffffffff\"}";
  T_CHECK(build_hand_made_line(decoy, sizeof decoy, inner, "\"}", NULL) > 0);
  relais_chain_reader_init(&reader);
  T_CHECK(relais_chain_reader_step(&reader, decoy, NULL, why, sizeof why) ==
          RELAIS_CHAIN_OK);
  relais_chain read_back, expected;
  T_CHECK(relais_chain_read(decoy, &read_back));
  relais_chain_genesis(&expected);
  relais_chain_advance(&expected, inner, strlen(inner), NULL);
  T_CHECK(relais_chain_equal(&read_back, &expected)); /* the last, not the decoy */
}

int main(void) {
  test_chain_fingerprint_is_a_pure_function_of_the_record();
  test_chain_hex_form_round_trips();
  test_chain_field_does_not_break_the_existing_readers();
  test_chain_locates_the_first_break();
  test_journal_without_chain_still_reads();
  test_chain_key_is_part_of_the_fingerprint();
  test_chain_reader_tolerates_a_real_file();
  return t_summary();
}
