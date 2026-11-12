/* Relais M46 — minimal proof program, compiled with qcc for QNX 8.0.
 *
 * Runs inside the real guest (QEMU/KVM) and prints machine-readable timing
 * evidence on the serial console: uname, clock_gettime(CLOCK_MONOTONIC)
 * overhead, and 1 ms periodic-sleep overshoot. These are the numbers the
 * host harness (host/bench/qnx_vm.py) parses from the transcript.
 *
 * Requirement IDs: PERF-01 (measured timings, never invented), QNX-08
 * (boot-to-measure path on real QNX), OBS-04 (evidence on the console).
 *
 * Build (from the repository root, SDP sourced):
 *   qcc -V gcc_ntox86_64 -std=c11 -O2 -Wall -Wextra -Werror -Wshadow \
 *       -Wconversion -Wno-sign-conversion -D_POSIX_C_SOURCE=200809L \
 *       host/bench/guest/qnx_proof.c -o build/qnx-vm-bin/rl-proof
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/utsname.h>

#define GETTIME_CALLS 10000U
#define SLEEP_CALLS   200U
#define SLEEP_NS      1000000 /* 1 ms */

static int64_t ns_between(const struct timespec *from,
                          const struct timespec *to)
{
  return (int64_t)(to->tv_sec - from->tv_sec) * INT64_C(1000000000) +
         (int64_t)to->tv_nsec - (int64_t)from->tv_nsec;
}

/* Min/mean/max accumulator over integer nanosecond samples. */
struct stats {
  int64_t min;
  int64_t max;
  int64_t sum;
  uint64_t count;
};

static void stats_add(struct stats *s, int64_t sample)
{
  if (s->count == 0U || sample < s->min) {
    s->min = sample;
  }
  if (s->count == 0U || sample > s->max) {
    s->max = sample;
  }
  s->sum += sample;
  s->count += 1U;
}

static void stats_print(const char *tag, const struct stats *s)
{
  double mean_us = ((double)s->sum / (double)s->count) / 1000.0;
  printf("rl-proof: %s calls=%" PRIu64 " min_us=%.3f mean_us=%.3f "
         "max_us=%.3f\n", tag, s->count, (double)s->min / 1000.0, mean_us,
         (double)s->max / 1000.0);
  fflush(stdout);
}

static void print_uname(void)
{
  struct utsname u;
  if (uname(&u) != 0) {
    printf("rl-proof: uname sys=unknown rel=unknown ver=unknown\n");
    return;
  }
  printf("rl-proof: uname sys=%s rel=%s ver=%s machine=%s node=%s\n",
         u.sysname, u.release, u.version, u.machine, u.nodename);
  fflush(stdout);
}

static int measure_gettime(void)
{
  struct stats s = {0, 0, 0, 0U};
  struct timespec a;
  struct timespec b;
  if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) {
    printf("rl-proof: error clock_gettime failed\n");
    return 1;
  }
  for (uint64_t i = 0U; i < GETTIME_CALLS; i++) {
    if (clock_gettime(CLOCK_MONOTONIC, &b) != 0) {
      printf("rl-proof: error clock_gettime failed at %" PRIu64 "\n", i);
      return 1;
    }
    stats_add(&s, ns_between(&a, &b));
    a = b;
  }
  stats_print("gettime_us", &s);
  return 0;
}

static int measure_sleep(void)
{
  struct stats s = {0, 0, 0, 0U};
  struct timespec a;
  struct timespec b;
  const struct timespec nap = {0, (long)SLEEP_NS};
  for (uint64_t i = 0U; i < SLEEP_CALLS; i++) {
    if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) {
      return 1;
    }
    if (nanosleep(&nap, NULL) != 0) {
      printf("rl-proof: error nanosleep failed at %" PRIu64 "\n", i);
      return 1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &b) != 0) {
      return 1;
    }
    /* Overshoot above the requested 1 ms: what the scheduler adds. */
    stats_add(&s, ns_between(&a, &b) - (int64_t)SLEEP_NS);
  }
  stats_print("sleep_overshoot_us", &s);
  return 0;
}

int main(void)
{
  print_uname();
  if (measure_gettime() != 0) {
    printf("rl-proof: done rc=1\n");
    return 1;
  }
  if (measure_sleep() != 0) {
    printf("rl-proof: done rc=1\n");
    return 1;
  }
  printf("rl-proof: done rc=0\n");
  fflush(stdout);
  return 0;
}
