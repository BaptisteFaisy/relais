/* Relais — offline verifier (OBS-07).
 *
 * Reads an actuator journal (JSON lines) and checks every safety invariant of
 * the specification, section 7.2. The invariants live in the core library
 * (core/src/invariants.c), so the explorer, the simulator and this tool judge
 * the same way: one implementation, three uses.
 *
 * Usage: relais-verify <journal.jsonl> [--level socle|complet]
 *                      [--fault-free] [--twin <journal.jsonl>]
 *                      [--check-chain] [--chain-key <32 hex>]
 *                      [--expect-chain <16 hex>]
 * Exit code: 0 no violation, 1 violated, 2 bad usage or unreadable file.
 *
 * --check-chain verifies the chained fingerprint every record carries (idea 34)
 * before the invariants are run, and names the first break: a record that was
 * edited, deleted or inserted afterwards cannot be hidden. A journal written
 * before the chain existed has no fingerprint at all; it is still read, and the
 * check reports that there was nothing to verify.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relais/invariants.h"
#include "relais/journal.h"

#define LINE_CAP RELAIS_JOURNAL_CHAIN_LINE_MAX

/* One journal record, in file order: a tick line or an event line. */
typedef struct {
  bool is_tick;
  relais_jr_tick tick;
  relais_jr_event event;
} rec_t;

typedef struct {
  rec_t *items;
  size_t count;
  relais_params params;
  bool have_params;
} journal_t;

static void usage(const char *prog) {
  fprintf(stderr,
          "usage: %s <journal.jsonl> [--level socle|complet] [--fault-free]\n"
          "          [--double-fault] [--twin <journal.jsonl>]\n"
          "          [--check-chain] [--chain-key <32 hex>]\n"
          "          [--expect-chain <16 hex>]\n", prog);
}

/* Walk a journal and verify its chain (idea 34, SEC-07). Returns 0 when every
 * record continues the chain, 1 at the first break, 2 when the file cannot be
 * read. *chained counts the records that carried a fingerprint, so a journal
 * written before the chain existed is told apart from a journal that lost it. */
static int chain_check(const char *path, const relais_key *key, char *why, size_t why_cap,
                       size_t *records, size_t *chained, relais_chain *last) {
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    fprintf(stderr, "relais-verify: cannot read %s\n", path);
    return 2;
  }
  relais_chain_reader reader;
  relais_chain_reader_init(&reader);
  char line[LINE_CAP];
  int verdict = 0;
  while (fgets(line, sizeof line, f) != NULL) {
    relais_chain_status st = relais_chain_reader_step(&reader, line, key, why, why_cap);
    if (st == RELAIS_CHAIN_BREAK) {
      verdict = 1;
      break; /* the first break is the one that matters: the rest follows from it */
    }
  }
  fclose(f);
  *records = reader.records;
  *chained = reader.chained;
  *last = reader.expected;
  return verdict;
}

/* The anchor: the only thing that can reveal a tail cut off the file. */
static int check_anchor(const char *journal, const relais_chain *last, const char *expect) {
  relais_chain want;
  char found[RELAIS_CHAIN_HEX + 1];
  relais_chain_to_hex(last, found, sizeof found);
  if (!relais_chain_from_hex(&want, expect)) {
    printf("relais-verify: --expect-chain takes 16 hexadecimal digits\n");
    return 2;
  }
  if (!relais_chain_equal(&want, last)) {
    printf("CHAIN-SHORT %s ends at %s, the sealed anchor was %s\n", journal, found, expect);
    return 1;
  }
  printf("chain: anchor matches %s\n", expect);
  return 0;
}

static bool journal_load(const char *path, journal_t *j) {
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    fprintf(stderr, "relais-verify: %s illisible\n", path);
    return false;
  }
  char line[LINE_CAP];
  size_t cap = 1024;
  j->items = malloc(cap * sizeof *j->items);
  j->count = 0;
  j->have_params = false;
  relais_params_default(&j->params);
  while (j->items != NULL && fgets(line, sizeof line, f) != NULL) {
    if (j->count == cap) {
      cap *= 2;
      rec_t *grown = realloc(j->items, cap * sizeof *j->items);
      if (grown == NULL) break;
      j->items = grown;
    }
    rec_t *r = &j->items[j->count];
    r->is_tick = false;
    memset(&r->tick, 0, sizeof r->tick);
    memset(&r->event, 0, sizeof r->event);
    if (relais_journal_is_header(line)) {
      relais_jr_header h;
      if (relais_journal_parse_header(line, &h)) {
        j->params = h.params;
        j->have_params = true;
      }
      continue;
    }
    if (relais_journal_is_tick(line)) {
      if (!relais_journal_parse_tick(line, &r->tick)) continue;
      r->is_tick = true;
      j->count++;
      continue;
    }
    if (relais_journal_is_event(line)) {
      if (!relais_journal_parse_event(line, &r->event)) continue;
      j->count++;
    }
  }
  fclose(f);
  return j->items != NULL;
}

static void journal_free(journal_t *j) {
  free(j->items);
  j->items = NULL;
  j->count = 0;
}

/* Twin lookup table indexed by tick; NaN when the twin has no such tick. */
static float *twin_table(const journal_t *twin, size_t *out_size) {
  relais_tick max_tick = 0;
  for (size_t i = 0; i < twin->count; i++) {
    if (twin->items[i].is_tick && twin->items[i].tick.tick > max_tick)
      max_tick = twin->items[i].tick.tick;
  }
  size_t size = (size_t)max_tick + 1;
  float *table = malloc(size * sizeof *table);
  if (table == NULL) return NULL;
  for (size_t i = 0; i < size; i++) table[i] = NAN;
  for (size_t i = 0; i < twin->count; i++) {
    if (!twin->items[i].is_tick) continue;
    table[(size_t)twin->items[i].tick.tick] = twin->items[i].tick.value;
  }
  *out_size = size;
  return table;
}

static void run_check(relais_inv *inv, const journal_t *j, const float *twin,
                      size_t twin_size) {
  for (size_t i = 0; i < j->count; i++) {
    const rec_t *r = &j->items[i];
    if (!r->is_tick) {
      relais_inv_on_event(inv, &r->event);
      continue;
    }
    float value = 0.0f;
    bool has_twin = twin != NULL && (size_t)r->tick.tick < twin_size &&
                    !isnan(twin[(size_t)r->tick.tick]);
    if (has_twin) value = twin[(size_t)r->tick.tick];
    relais_inv_on_tick(inv, &r->tick, has_twin ? &value : NULL);
    relais_inv_end_tick(inv, r->tick.tick);
  }
}

/* Print the verdict and release what the run owned. */
static int finish(relais_inv *inv, journal_t *j, const char *journal,
                  const char *twin_path, float *twin, size_t twin_size,
                  journal_t *twin_journal) {
  char report[RELAIS_INV_MSG_MAX * 2];
  relais_inv_report(inv, report, sizeof report);
  printf("%s\n", report);
  printf("journal: %s (%zu records, %u ticks)\n", journal, j->count, (unsigned)inv->ticks);
  if (twin_path != NULL) printf("twin   : %s (%zu ticks)\n", twin_path, twin_size);
  free(twin);
  if (twin_path != NULL) journal_free(twin_journal);
  journal_free(j);
  return relais_inv_ok(inv) ? 0 : 1;
}

/* Verify and report the chain of one journal (idea 34). Returns 0 when the
 * chain is intact or absent, 1 at the first break, 2 when nothing can be read. */
static int report_chain(const char *journal, const relais_key *key, const char *expect) {
  char why[192] = "";
  size_t records = 0, chained = 0;
  relais_chain last;
  relais_chain_genesis(&last);
  const int verdict = chain_check(journal, key, why, sizeof why, &records, &chained, &last);
  if (verdict == 2) return 2;
  if (verdict == 1) {
    printf("CHAIN-BREAK %s\n", why);
    printf("chain: first break in %s\n", journal);
    return 1;
  }
  if (chained == 0)
    printf("chain: absent (%zu lines, journal written without a chain)\n", records);
  else
    printf("chain: OK (%zu of %zu lines chained, no break)\n", chained, records);
  return expect == NULL ? 0 : check_anchor(journal, &last, expect);
}

/* Everything the command line decides, in one place, so main stays readable. */
typedef struct {
  const char *journal;
  const char *twin_path;
  const char *expect_chain; /* the sealed anchor, when the run recorded one */
  relais_key chain_key;
  bool complete;
  bool fault_free;
  bool double_fault;
  bool check_chain;
} options_t;

static bool parse_options(int argc, char **argv, options_t *o) {
  memset(o, 0, sizeof *o);
  relais_key_clear(&o->chain_key);
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--level") == 0 && i + 1 < argc)
      o->complete = strcmp(argv[++i], "complet") == 0;
    else if (strcmp(a, "--fault-free") == 0) o->fault_free = true;
    else if (strcmp(a, "--double-fault") == 0) o->double_fault = true;
    else if (strcmp(a, "--check-chain") == 0) o->check_chain = true;
    else if (strcmp(a, "--chain-key") == 0 && i + 1 < argc) {
      if (!relais_key_from_hex(&o->chain_key, argv[++i])) return false;
    } else if (strcmp(a, "--expect-chain") == 0 && i + 1 < argc) o->expect_chain = argv[++i];
    else if (strcmp(a, "--twin") == 0 && i + 1 < argc) o->twin_path = argv[++i];
    else if (o->journal == NULL) o->journal = a;
    else return false;
  }
  return o->journal != NULL;
}

int main(int argc, char **argv) {
  options_t opt;
  if (!parse_options(argc, argv, &opt)) {
    usage(argv[0]);
    return 2;
  }
  /* The chain is judged first: the invariants of an edited journal mean nothing. */
  if (opt.check_chain) {
    const int verdict = report_chain(opt.journal, &opt.chain_key, opt.expect_chain);
    if (verdict != 0) return verdict;
  }

  journal_t j;
  if (!journal_load(opt.journal, &j)) return 2;

  relais_inv inv;
  relais_inv_init(&inv, &j.params);
  relais_inv_expect_certified(&inv, opt.complete);
  relais_inv_fault_free(&inv, opt.fault_free);
  if (opt.double_fault) {
    /* A second fault landed before redundancy was back (spec 3.3, F9/F10):
     * the claim is the safe state, not the continuity of the command. */
    relais_inv_double_fault(&inv, 1);
  }

  float *twin = NULL;
  size_t twin_size = 0;
  journal_t twin_journal;
  if (opt.twin_path != NULL) {
    if (!journal_load(opt.twin_path, &twin_journal)) {
      journal_free(&j);
      return 2;
    }
    twin = twin_table(&twin_journal, &twin_size);
  }

  run_check(&inv, &j, twin, twin_size);
  return finish(&inv, &j, opt.journal, opt.twin_path, twin, twin_size, &twin_journal);
}
