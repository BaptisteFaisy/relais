/* Relais — offline verifier (OBS-07).
 *
 * Reads an actuator journal (JSON lines) and checks every safety invariant of
 * the specification, section 7.2. The invariants live in the core library
 * (core/src/invariants.c), so the explorer, the simulator and this tool judge
 * the same way: one implementation, three uses.
 *
 * Usage: relais-verify <journal.jsonl> [--level socle|complet]
 *                      [--fault-free] [--twin <journal.jsonl>]
 * Exit code: 0 no violation, 1 violated, 2 bad usage or unreadable file.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relais/invariants.h"
#include "relais/journal.h"

#define LINE_CAP RELAIS_JOURNAL_LINE_MAX

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
          "          [--double-fault] [--twin <journal.jsonl>]\n", prog);
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

int main(int argc, char **argv) {
  const char *journal = NULL;
  const char *twin_path = NULL;
  bool complete = false;
  bool fault_free = false;
  bool double_fault = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
      complete = strcmp(argv[++i], "complet") == 0;
    } else if (strcmp(argv[i], "--fault-free") == 0) {
      fault_free = true;
    } else if (strcmp(argv[i], "--double-fault") == 0) {
      double_fault = true;
    } else if (strcmp(argv[i], "--twin") == 0 && i + 1 < argc) {
      twin_path = argv[++i];
    } else if (journal == NULL) {
      journal = argv[i];
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  if (journal == NULL) {
    usage(argv[0]);
    return 2;
  }

  journal_t j;
  if (!journal_load(journal, &j)) return 2;

  relais_inv inv;
  relais_inv_init(&inv, &j.params);
  relais_inv_expect_certified(&inv, complete);
  relais_inv_fault_free(&inv, fault_free);
  if (double_fault) {
    /* A second fault landed before redundancy was back (spec 3.3, F9/F10):
     * the claim is the safe state, not the continuity of the command. */
    relais_inv_double_fault(&inv, 1);
  }

  float *twin = NULL;
  size_t twin_size = 0;
  journal_t twin_journal;
  if (twin_path != NULL) {
    if (!journal_load(twin_path, &twin_journal)) {
      journal_free(&j);
      return 2;
    }
    twin = twin_table(&twin_journal, &twin_size);
  }

  run_check(&inv, &j, twin, twin_size);

  char report[RELAIS_INV_MSG_MAX * 2];
  relais_inv_report(&inv, report, sizeof report);
  printf("%s\n", report);
  printf("journal: %s (%zu records, %u ticks)\n", journal, j.count,
         (unsigned)inv.ticks);
  if (twin_path != NULL) printf("twin   : %s (%zu ticks)\n", twin_path, twin_size);

  free(twin);
  if (twin_path != NULL) journal_free(&twin_journal);
  journal_free(&j);
  return relais_inv_ok(&inv) ? 0 : 1;
}
