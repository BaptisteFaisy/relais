/* Relais — warm restart of the actuator without losing the tempo (idea 29).
 *
 * The durable resume (mandate number and holder) is target/persist.c, which
 * stores the snapshot through the pure core API and survives a crash. This
 * file is the *warm* half of the life cycle: after the process (or HAM) has
 * restarted it, the actuator must catch up to the tick the tempo should be
 * serving now, and it must MEASURE the hole it left — the ticks during which
 * no STATUT went out, covered by the testament and then by the fallback law.
 *
 * Everything here is portable C11 and runs on the PC too, so the gap arithmetic
 * and the resume path are tested off-target. The only QNX-specific part — the
 * kernel pulse that gives the tempo — is described below and lives in
 * actuator_qnx.c, unchanged.
 *
 * Theoretical bound (documented, not proven here): N ticks are served by the
 * testament of the last order received; beyond N the local fallback law holds
 * the position for up to `degraded_ticks` before the stop request (DEG-05).
 * A warm restart is safe as long as gap <= N + degraded_ticks; the measurement
 * below is what proves the hole was within that bound on the real target.
 *
 * Requirements: MAN-11 (mandate survives), QNX-08 (kernel start to a certified
 * second), ACT-05 (one value per tick, never two mandates for the same tick).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "persist.h"
#include "relais/arbiter.h"
#include "relais/params.h"

/* Whole ticks that elapsed while the actuator was down. Floor of the downtime
 * over the period: a partial period does not count as a missed tick, the
 * actuator resumes at the first boundary it can still serve (ACT-05). */
uint64_t relais_warm_gap_ticks(uint32_t period_ms, uint64_t downtime_ms) {
  if (period_ms == 0) return 0;
  return downtime_ms / (uint64_t)period_ms;
}

/* The tick the actuator serves now, after catching up from `last_tick`.
 * `last_tick == RELAIS_TICK_NONE` means "no prior tempo" (cold start). */
relais_tick relais_warm_current_tick(relais_tick last_tick, uint64_t gap_ticks) {
  if (last_tick == RELAIS_TICK_NONE) return gap_ticks + 1;
  return last_tick + gap_ticks + 1;
}

/* What covered the hole, as one human line. testament_ticks = min(gap, N),
 * fallback_ticks = gap - testament_ticks. Returns the number of bytes that
 * would have been written (snprintf semantics), or 0 on a bad buffer. */
int relais_warm_gap_report(uint64_t downtime_ms, uint64_t gap_ticks,
                           uint8_t testament_len, char *buf, size_t cap) {
  if (buf == NULL || cap == 0) return 0;
  uint64_t testament = gap_ticks < (uint64_t)testament_len ? gap_ticks : (uint64_t)testament_len;
  uint64_t fallback = gap_ticks - testament;
  char covered[96];
  if (fallback == 0)
    snprintf(covered, sizeof covered, "fully by the testament (N=%u)",
             (unsigned)testament_len);
  else
    snprintf(covered, sizeof covered, "%" PRIu64 " tick(s) by testament, %" PRIu64
             " by the fallback law (N=%u)",
             testament, fallback, (unsigned)testament_len);
  return snprintf(buf, cap, "warm restart: hole %" PRIu64 " ticks (%" PRIu64
                  " ms down), covered %s",
                  gap_ticks, downtime_ms, covered);
}

/* Resume from persistence, then report the hole. `downtime_ms` and `last_tick`
 * are measured by the caller across the restart (monotonic clock survives a
 * process restart; wall clock is the fallback across a power cycle). */
int relais_warm_resume(relais_persist *p, relais_arb *arb, relais_tick last_tick,
                       uint64_t downtime_ms, uint32_t period_ms, uint8_t testament_len,
                       char *note, size_t note_cap) {
  int r = relais_persist_load_arb(p, arb);
  if (r < 0) return r;
  uint64_t gap = relais_warm_gap_ticks(period_ms, downtime_ms);
  relais_tick now = relais_warm_current_tick(last_tick, gap);
  if (note != NULL && note_cap > 0)
    relais_warm_gap_report(downtime_ms, gap, testament_len, note, note_cap);
  (void)now; /* the caller decides what to do with the resumed tempo */
  if (r == 0 && !p->recovered)
    return 0; /* mandate restored from the primary copy */
  return r == 0 ? 2 : 1; /* 2 = restored from backup, 1 = fresh start */
}

static void usage(void) {
  fprintf(stderr,
          "usage: relais-warm --state FILE [--downtime-ms N] [--last-tick T]\n"
          "       [--params T=5,p=1,N=8]\n"
          "proves the warm-restart resume: reads the persisted mandate, then\n"
          "reports the hole the tempo left (gap, coverage by testament/fallback).\n");
}

int main(int argc, char **argv) {
  const char *state = NULL;
  uint64_t downtime_ms = 0;
  relais_tick last_tick = RELAIS_TICK_NONE;
  relais_params params;
  relais_params_default(&params);
  char why[128] = "";

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--state") == 0 && i + 1 < argc) state = argv[++i];
    else if (strcmp(a, "--downtime-ms") == 0 && i + 1 < argc)
      downtime_ms = (uint64_t)strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--last-tick") == 0 && i + 1 < argc)
      last_tick = (relais_tick)strtoull(argv[++i], NULL, 10);
    else if (strcmp(a, "--params") == 0 && i + 1 < argc) {
      if (!relais_params_parse(&params, argv[++i], why, sizeof why)) {
        fprintf(stderr, "bad params: %s\n", why);
        return 2;
      }
    } else {
      usage();
      return 2;
    }
  }
  if (state == NULL) {
    usage();
    return 2;
  }

  relais_persist p;
  if (relais_persist_init(&p, state) != 0) {
    fprintf(stderr, "bad state path\n");
    return 2;
  }
  relais_arb arb;
  relais_arb_init(&arb, &params);
  char note[160];
  int r = relais_warm_resume(&p, &arb, last_tick, downtime_ms,
                             relais_params_period_ms(&params), params.testament_len,
                             note, sizeof note);
  printf("resume: %s\n",
         r == 0 ? "mandate restored (primary copy)"
                : r == 2 ? "mandate restored (backup copy)"
                         : r == 1 ? "fresh start, no persisted mandate"
                                  : "persistence unreadable");
  if (r >= 0) printf("mandate %u, holder %s\n", (unsigned)arb.mandate,
                     relais_node_name(arb.holder));
  if (note[0] != '\0') printf("%s\n", note);
  return (r < 0) ? 1 : 0;
}