/* Relais — HIL verifier (mission M41): the same invariants, with the
 * hypothesis of the channel stated explicitly.
 *
 * The offline verifier (host/verify/relais-verify.c) judges a journal in the
 * hypothesis of a quiet link: every tick served by a fresh order (level C,
 * zero gap). On a shared PC that hypothesis does not hold — the OS scheduler
 * delays frames, the second loses its certification, and the will covers the
 * gap. The core library already names that case: relais_inv_lossy_channel()
 * (ROB-02, ROB-03): "the channel loses or delays frames: the zero-gap bound
 * of level C no longer applies, the will covers the gap instead".
 *
 * So the HIL bench runs the invariants twice on the same journal:
 *   --lossy   the claim that holds on a disturbed channel: the safety
 *             invariants are enforced (I1, I2, I3, I6, I8, I10, I11) and the
 *             gap is measured and published, not enforced;
 *   --strict  the level-C completeness claim (zero gap): reported so the
 *             reader sees exactly what the environment costs.
 *
 * The journal is parsed by the core (relais/journal.h) and judged by the core
 * (relais/invariants.h): this file only wires the two together.
 *
 * Usage: verify_hil <journal.jsonl> [--lossy|--strict] [--socle]
 *                    [--double-fault]
 * Exit code: 0 no violation, 1 violated, 2 bad usage or unreadable file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relais/invariants.h"
#include "relais/journal.h"

#define LINE_CAP RELAIS_JOURNAL_LINE_MAX

static void usage(const char *prog) {
  fprintf(stderr,
          "usage: %s <journal.jsonl> [--lossy|--strict] [--socle] [--double-fault]\n",
          prog);
}

/* Feed the journal to the invariants; the records are read in file order. */
static void feed(relais_inv *inv, const char *path) {
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    fprintf(stderr, "verify_hil: %s illisible\n", path);
    exit(2);
  }
  char line[LINE_CAP];
  while (fgets(line, sizeof line, f) != NULL) {
    if (relais_journal_is_header(line)) {
      relais_jr_header h;
      if (relais_journal_parse_header(line, &h)) {
        inv->params = h.params;
        inv->gap_bound = inv->expect_certified_second
                             ? 0u
                             : (uint32_t)inv->params.patience + 1u;
      }
      continue;
    }
    if (relais_journal_is_tick(line)) {
      relais_jr_tick r;
      if (relais_journal_parse_tick(line, &r)) {
        relais_inv_on_tick(inv, &r, NULL);
        relais_inv_end_tick(inv, r.tick);
      }
      continue;
    }
    if (relais_journal_is_event(line)) {
      relais_jr_event e;
      if (relais_journal_parse_event(line, &e)) relais_inv_on_event(inv, &e);
    }
  }
  fclose(f);
}

int main(int argc, char **argv) {
  const char *journal = NULL;
  bool lossy = false, socle = false, double_fault = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--lossy") == 0) lossy = true;
    else if (strcmp(argv[i], "--strict") == 0) lossy = false;
    else if (strcmp(argv[i], "--socle") == 0) socle = true;
    else if (strcmp(argv[i], "--double-fault") == 0) double_fault = true;
    else if (journal == NULL) journal = argv[i];
    else { usage(argv[0]); return 2; }
  }
  if (journal == NULL) { usage(argv[0]); return 2; }

  relais_inv inv;
  relais_params params;
  relais_params_default(&params);
  relais_inv_init(&inv, &params);
  relais_inv_expect_certified(&inv, !socle);
  relais_inv_lossy_channel(&inv, lossy);
  if (double_fault) relais_inv_double_fault(&inv, 1);

  feed(&inv, journal);

  char report[RELAIS_INV_MSG_MAX * 2];
  relais_inv_report(&inv, report, sizeof report);
  printf("%s\n", report);
  printf("journal: %s (%u ticks, mode=%s, channel=%s)\n", journal,
         (unsigned)inv.ticks, socle ? "socle" : "complet",
         lossy ? "disturbed" : "quiet");
  return relais_inv_ok(&inv) ? 0 : 1;
}
