/* Relais — relay adapter self-test driver (mission M41).
 *
 * build/relais-relay --backend loopback --state build/hil/relay.state --selftest
 *   -> exercises the switching logic here; exit 0 on PASS.
 * build/relais-relay --backend gpio --channel 17 --selftest
 * build/relais-relay --backend usb  --device /dev/ttyUSB0 --selftest
 *   -> exit 3 and print BLOCKED(no relay hardware) when the device is absent;
 *      exit 0 if real hardware is plugged and the sequence passes.
 *
 * The loopback self-test proves, on this machine: open leaves the contacts
 * OPEN (fail-safe), energize/readback/de-energize/readback round-trip through
 * a SECOND file handle, and switching timestamps are measured with
 * CLOCK_MONOTONIC. Requirements: INJ-04 (cut/restore of a link), ROB-05.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "relay_adapter.h"

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void print_usage(void) {
  fprintf(stderr,
          "usage: relais-relay --backend loopback|gpio|usb [--state <file>]\n"
          "                    [--gpio-root /sys/class/gpio] [--channel 17]\n"
          "                    [--device /dev/ttyUSB0] [--baud 9600] [--selftest]\n");
}

typedef struct {
  relay_config cfg;
  bool selftest;
} args_t;

static bool parse_args(int argc, char **argv, args_t *a, char *state, size_t cap) {
  relay_config_default(&a->cfg);
  a->selftest = false;
  for (int i = 1; i < argc; i++) {
    const char *k = argv[i];
    if (strcmp(k, "--backend") == 0 && i + 1 < argc) {
      if (!relay_backend_parse(argv[++i], &a->cfg.backend)) return false;
    } else if (strcmp(k, "--state") == 0 && i + 1 < argc) {
      snprintf(state, cap, "%s", argv[++i]);
      a->cfg.device = state;
    } else if (strcmp(k, "--device") == 0 && i + 1 < argc) {
      snprintf(state, cap, "%s", argv[++i]);
      a->cfg.device = state;
    } else if (strcmp(k, "--gpio-root") == 0 && i + 1 < argc) {
      a->cfg.gpio_root = argv[++i];
    } else if (strcmp(k, "--channel") == 0 && i + 1 < argc) {
      a->cfg.channel = atoi(argv[++i]);
    } else if (strcmp(k, "--baud") == 0 && i + 1 < argc) {
      a->cfg.baud = (unsigned)atoi(argv[++i]);
    } else if (strcmp(k, "--selftest") == 0) {
      a->selftest = true;
    } else {
      return false;
    }
  }
  return true;
}

/* One commanded transition + readback; returns false on mismatch. */
static bool expect_state(relay_t *r, bool want, int64_t *ns_out, int *fails) {
  int64_t t0 = now_ns();
  if (!relay_set(r, want)) {
    printf("FAIL relay_set(%s) on backend %s\n", want ? "true" : "false",
           relay_backend_name(r->backend));
    (*fails)++;
    return false;
  }
  if (ns_out != NULL) *ns_out = now_ns() - t0;
  bool got = false;
  if (!relay_get(r, &got) || got != want) {
    printf("FAIL readback: want %s got %s\n", want ? "energized" : "open",
           got ? "energized" : "open");
    (*fails)++;
    return false;
  }
  return true;
}

static int run_selftest(relay_t *r) {
  int fails = 0;
  bool initial = true;
  if (!relay_get(r, &initial) || initial) {
    printf("FAIL fail-safe: contacts must be OPEN right after relay_open\n");
    fails++;
  }
  int64_t close_ns = 0, open_ns = 0;
  if (!expect_state(r, true, &close_ns, &fails)) return fails;   /* link up */
  if (!expect_state(r, false, &open_ns, &fails)) return fails;   /* link cut */
  if (!expect_state(r, true, NULL, &fails)) return fails;        /* restore */
  if (!expect_state(r, false, NULL, &fails)) return fails;       /* cut again */
  printf("PASS relay selftest backend=%s device=%s close=%lld ns open=%lld ns\n",
         relay_backend_name(r->backend), r->device[0] ? r->device : "-",
         (long long)close_ns, (long long)open_ns);
  return fails;
}

int main(int argc, char **argv) {
  args_t a;
  char state[256] = "";
  if (!parse_args(argc, argv, &a, state, sizeof state)) {
    print_usage();
    return RELAY_EXIT_FAIL;
  }
  relay_t r;
  if (!relay_open(&r, &a.cfg)) {
    if (strstr(r.why, "BLOCKED") != NULL) {
      printf("%s (backend %s)\n", r.why, relay_backend_name(a.cfg.backend));
      printf("hardware mode command, when a relay is plugged:\n");
      if (a.cfg.backend == RELAY_BACKEND_GPIO)
        printf("  relais-relay --backend gpio --gpio-root %s --channel %d --selftest\n",
               a.cfg.gpio_root, a.cfg.channel);
      else
        printf("  relais-relay --backend usb --device %s --baud %u --selftest\n",
               state[0] ? state : "/dev/ttyUSB0", a.cfg.baud);
      return RELAY_EXIT_BLOCKED;
    }
    printf("FAIL relay_open: %s\n", r.why);
    return RELAY_EXIT_FAIL;
  }
  int rc = RELAY_EXIT_PASS;
  if (a.selftest) {
    rc = run_selftest(&r) == 0 ? RELAY_EXIT_PASS : RELAY_EXIT_FAIL;
  } else {
    printf("relay open: backend=%s device=%s (contacts OPEN = link cut)\n",
           relay_backend_name(r.backend), r.device);
  }
  relay_close(&r);
  return rc;
}
