/* Relais M48 — the in-guest failover probe.
 *
 * It runs INSIDE the QNX 8.0 guest, next to the real processes, and it does
 * what the host cannot do: measure the takeover on the GUEST clock and read
 * the actuator's journal in the guest.
 *
 * The sequence (INJ-01, PERF-02, OBS-04):
 *   1. wait until the actuator's journal shows the expected holder and the
 *      expected certified second (the relay is normally established),
 *   2. record t0 = clock_gettime(CLOCK_MONOTONIC), then SIGKILL the holder
 *      from inside the guest (the fault is injected in the guest, not by the
 *      bench),
 *   3. watch the second's own status file (flushed line by line) until it
 *      reports role=holder: t1 = clock_gettime(CLOCK_MONOTONIC),
 *   4. wait until the actuator has written its last tick, then read its
 *      journal and report what the protocol itself recorded: the tick of the
 *      handover, the number of ticks without a fresh order (the evidence the
 *      event carries, DET-03), the mandate before and after (MAN-03).
 *
 * Every measured fact is printed as one "rl-relais: key=value ..." line: that
 * is what the host collects (the same grammar as the M46 harnesses).
 *
 * Nothing here decides anything: the verdict comes from the journal written
 * by the actuator, parsed by the very same code the verifier uses
 * (core/src/journal.c).
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "relais/journal.h"

#define LINE_CAP RELAIS_JOURNAL_LINE_MAX
#define NODE_NAME_MAX 16

/* The journal, read as a growing file: only the new lines are parsed. */
typedef struct {
  FILE *f;
  long pos;
} jfollow_t;

/* What the protocol recorded, straight from the journal. */
typedef struct {
  uint64_t ticks;
  uint64_t records;
  uint64_t last_tick;
  char holder[NODE_NAME_MAX];
  uint64_t mandate;
  uint8_t misses;
  bool certified;
  char certified_node[NODE_NAME_MAX];
  /* the events that matter for the handover */
  int revoked;
  int granted;
  int auto_switch;
  int certified_events;
  uint64_t revoke_tick;
  uint64_t revoke_evidence;
  char revoked_holder[NODE_NAME_MAX];
  uint64_t switch_tick;
  uint64_t switch_mandate;
  char switch_holder[NODE_NAME_MAX];
  uint64_t switch_evidence;
  uint64_t switch_misses;
  uint64_t switch_prev_tick;
  uint64_t switch_prev_evidence;
  char switch_prev_holder[NODE_NAME_MAX];
  bool have_switch;
} verdict_t;

static uint64_t now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* A short wait between two polls (POSIX nanosleep: usleep is gone from
 * POSIX.1-2008, and the guest is built with -std=c11). */
static void sleep_us(long us) {
  struct timespec ts;
  ts.tv_sec = us / 1000000L;
  ts.tv_nsec = (us % 1000000L) * 1000L;
  nanosleep(&ts, NULL);
}

static void copy_name(char *dst, size_t cap, const char *src) {
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  snprintf(dst, cap, "%s", src);
}

static bool jfollow_open(jfollow_t *j, const char *path) {
  j->f = fopen(path, "r");
  j->pos = 0;
  return j->f != NULL;
}

/* One new line of the journal, or 0 when there is nothing new yet. */
static int jfollow_next(jfollow_t *j, char *line, size_t cap) {
  if (j->f == NULL) return 0;
  clearerr(j->f);
  if (fseek(j->f, j->pos, SEEK_SET) != 0) return 0;
  if (fgets(line, cap, j->f) == NULL) return 0;
  j->pos = ftell(j->f);
  return 1;
}

static void jfollow_close(jfollow_t *j) {
  if (j->f != NULL) fclose(j->f);
  j->f = NULL;
}

/* Feed one journal line into the verdict. Ticks and events are parsed by the
 * core, so the guest reads the journal exactly as host/verify does. */
static void verdict_line(verdict_t *v, const char *line) {
  if (relais_journal_is_tick(line)) {
    relais_jr_tick t;
    if (!relais_journal_parse_tick(line, &t)) return;
    v->records++;
    v->ticks++;
    v->last_tick = (uint64_t)t.tick;
    copy_name(v->holder, sizeof v->holder, relais_node_name(t.holder));
    v->mandate = t.mandate;
    v->misses = t.misses;
    v->certified = t.certified;
    copy_name(v->certified_node, sizeof v->certified_node,
              relais_node_name(t.certified_node));
    return;
  }
  if (!relais_journal_is_event(line)) return;
  relais_jr_event e;
  if (!relais_journal_parse_event(line, &e)) return;
  v->records++;
  if (e.kind == RELAIS_EV_MANDATE_REVOKED) {
    v->revoked++;
    v->revoke_tick = (uint64_t)e.tick;
    v->revoke_evidence = e.detail;
    copy_name(v->revoked_holder, sizeof v->revoked_holder, relais_node_name(e.node));
  } else if (e.kind == RELAIS_EV_MANDATE_GRANTED) {
    v->granted++;
  } else if (e.kind == RELAIS_EV_MANDATE_AUTO_SWITCH) {
    v->auto_switch++;
    v->have_switch = true;
    v->switch_tick = (uint64_t)e.tick;
    v->switch_mandate = e.mandate;
    v->switch_evidence = e.detail;
    copy_name(v->switch_holder, sizeof v->switch_holder, relais_node_name(e.node));
    /* the revocation that led here, as the event itself recorded it (DET-03) */
    v->switch_prev_tick = v->revoke_tick;
    v->switch_prev_evidence = v->revoke_evidence;
    copy_name(v->switch_prev_holder, sizeof v->switch_prev_holder,
              v->revoked_holder);
  } else if (e.kind == RELAIS_EV_CERTIFIED) {
    v->certified_events++;
  }
}

/* Read the journal to the end and fold it into the verdict. */
static void verdict_scan(verdict_t *v, const char *path) {
  FILE *f = fopen(path, "r");
  if (f == NULL) return;
  char line[LINE_CAP];
  while (fgets(line, sizeof line, f) != NULL) verdict_line(v, line);
  fclose(f);
}

/* Wait until the journal shows the expected holder. Returns false on timeout. */
static bool wait_established(jfollow_t *j, verdict_t *v, const char *holder,
                              const char *certified, int timeout_ms) {
  char line[LINE_CAP];
  uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
  for (;;) {
    while (jfollow_next(j, line, sizeof line)) verdict_line(v, line);
    bool holder_ok = v->ticks > 0 && strcmp(v->holder, holder) == 0;
    bool certified_ok = certified == NULL || certified[0] == '\0' ||
                        strcmp(v->certified_node, certified) == 0;
    if (holder_ok && certified_ok) return true;
    if (now_ns() > deadline) return false;
    sleep_us(1000);
  }
}

/* Watch the second's status file for the moment it becomes the holder.
 * Returns the detection instant in nanoseconds, 0 on timeout. */
static uint64_t wait_takeover(const char *path, uint64_t from_ns, int timeout_ms) {
  FILE *f = fopen(path, "r");
  if (f == NULL) return 0;
  uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
  char line[512];
  for (;;) {
    while (fgets(line, sizeof line, f) != NULL) {
      if (!strstr(line, "role=holder")) continue;
      uint64_t at = now_ns();
      if (at >= from_ns) {
        fclose(f);
        return at;
      }
    }
    clearerr(f);
    if (now_ns() > deadline) break;
    sleep_us(200);
  }
  fclose(f);
  return 0;
}

static void usage(void) {
  fprintf(stderr,
          "usage: rl-relais-failover --journal FILE --second-log FILE --victim-pid N\n"
          "       [--ticks N] [--expect-holder A] [--expect-second B] [--timeout-ms N]\n");
}

int main(int argc, char **argv) {
  const char *journal = NULL;
  const char *second_log = NULL;
  const char *expect_holder = "A";
  const char *expect_second = "B";
  int victim = -1;
  long ticks = 0;
  int timeout_ms = 20000;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--journal") && i + 1 < argc) journal = argv[++i];
    else if (!strcmp(argv[i], "--second-log") && i + 1 < argc) second_log = argv[++i];
    else if (!strcmp(argv[i], "--victim-pid") && i + 1 < argc) victim = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atol(argv[++i]);
    else if (!strcmp(argv[i], "--expect-holder") && i + 1 < argc) expect_holder = argv[++i];
    else if (!strcmp(argv[i], "--expect-second") && i + 1 < argc) expect_second = argv[++i];
    else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) timeout_ms = atoi(argv[++i]);
    else { usage(); return 2; }
  }
  if (journal == NULL || second_log == NULL || victim <= 0) {
    usage();
    return 2;
  }

  /* 1. the relay is normally established: the holder holds the mandate and
   *    the second is certified (REP-05). */
  jfollow_t jf;
  if (!jfollow_open(&jf, journal)) {
    fprintf(stderr, "rl-relais-failover: cannot read %s\n", journal);
    return 2;
  }
  verdict_t v;
  memset(&v, 0, sizeof v);
  if (!wait_established(&jf, &v, expect_holder, expect_second, timeout_ms)) {
    printf("rl-relais: phase=established_failed holder=%s certified=%s ticks=%llu\n",
           v.holder[0] ? v.holder : "none",
           v.certified_node[0] ? v.certified_node : "none",
           (unsigned long long)v.ticks);
    printf("rl-relais: done rc=1\n");
    jfollow_close(&jf);
    return 1;
  }
  printf("rl-relais: phase=established holder=%s mandate=%llu certified=%s tick=%llu\n",
         v.holder, (unsigned long long)v.mandate,
         v.certified_node[0] ? v.certified_node : "none",
         (unsigned long long)v.last_tick);
  fflush(stdout);

  /* 2. the fault, injected from inside the guest: SIGKILL on the holder. */
  uint64_t t0 = now_ns();
  if (kill((pid_t)victim, SIGKILL) != 0) {
    printf("rl-relais: kill_failed pid=%d errno=%d\n", victim, errno);
    printf("rl-relais: done rc=1\n");
    jfollow_close(&jf);
    return 1;
  }
  printf("rl-relais: kill pid=%d signal=9\n", victim);
  fflush(stdout);

  /* 3. the takeover, seen on the guest clock. */
  uint64_t t1 = wait_takeover(second_log, t0, timeout_ms);
  if (t1 == 0) {
    printf("rl-relais: takeover timeout_ms=%d\n", timeout_ms);
  } else {
    printf("rl-relais: takeover detected_us=%llu\n",
           (unsigned long long)((t1 - t0) / 1000ull));
  }
  fflush(stdout);

  /* 4. the journal, complete: the actuator's own account of the handover. */
  uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
  char line[LINE_CAP];
  while (ticks > 0 && (uint64_t)v.last_tick < (uint64_t)ticks && now_ns() < deadline) {
    while (jfollow_next(&jf, line, sizeof line)) verdict_line(&v, line);
    sleep_us(2000);
  }
  jfollow_close(&jf);
  verdict_scan(&v, journal);

  printf("rl-relais: verdict revoked=%d granted=%d auto_switch=%d certified_events=%d\n",
         v.revoked, v.granted, v.auto_switch, v.certified_events);
  if (v.have_switch) {
    printf("rl-relais: handover tick=%llu holder=%s mandate=%llu misses=%llu "
           "revoked_from=%s revoked_at_tick=%llu\n",
           (unsigned long long)v.switch_tick, v.switch_holder,
           (unsigned long long)v.switch_mandate,
           (unsigned long long)v.switch_evidence,
           v.switch_prev_holder[0] ? v.switch_prev_holder : "none",
           (unsigned long long)v.switch_prev_tick);
  } else {
    printf("rl-relais: handover none (no mandate_auto_switch event)\n");
  }
  printf("rl-relais: journal ticks=%llu records=%llu last_tick=%llu holder=%s\n",
         (unsigned long long)v.ticks, (unsigned long long)v.records,
         (unsigned long long)v.last_tick, v.holder[0] ? v.holder : "none");
  bool ok = v.have_switch && t1 != 0;
  printf("rl-relais: done rc=%d\n", ok ? 0 : 1);
  fflush(stdout);
  return ok ? 0 : 1;
}
