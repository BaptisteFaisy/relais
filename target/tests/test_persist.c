/* Relais — PC test of the persistence layer (target/persist.c).
 *
 * Runs in a temporary directory and exercises the whole MAN-11 story:
 * atomic write (no `.tmp` left), wear limiting (a no-op when the snapshot is
 * unchanged), round-trip restore, recovery from a corrupt primary copy, and
 * the "power cut during write" cases (truncated primary, missing primary).
 *
 * Self-contained harness (no external test framework): builds with
 *
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror -Wshadow -Wconversion \
 *       -Wno-sign-conversion -D_POSIX_C_SOURCE=200809L \
 *       -Icore/include -Itarget target/persist.c target/tests/test_persist.c \
 *       build/librelais.a -lm -o build/test_persist
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "persist.h"
#include "relais/arbiter.h"

static long g_checks;
static long g_failures;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

/* Fill an arbiter with a specific mandate/holder/certification. */
static void arb_state(relais_arb *a, relais_params *p, relais_mandate mandate,
                      relais_node holder, bool certified, relais_node certified_node) {
  relais_params_default(p);
  relais_arb_init(a, p);
  a->mandate = mandate;
  a->holder = holder;
  a->certified = certified;
  a->certified_node = certified ? certified_node : RELAIS_NODE_NONE;
}

static bool file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Overwrite a file with arbitrary bytes (used to simulate corruption). */
static int dump(const char *path, const uint8_t *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) return -1;
  int n = (int)fwrite(data, 1, len, f);
  fclose(f);
  return n;
}

static void tmp_path(char *out, size_t cap, const char *dir, const char *name) {
  snprintf(out, cap, "%s/%s", dir, name);
}

/* Fresh start + first save + wear limit + a mandate change. */
static void test_save_and_wear(const char *state) {
  relais_persist p;
  relais_params params;
  relais_arb arb, arb2;
  check(relais_persist_init(&p, state) == 0, "init");
  uint8_t buf[RELAIS_PERSIST_MAX];
  size_t len = 0;
  check(relais_persist_load(&p, buf, sizeof buf, &len) == 1, "fresh load == 1");
  check(!file_exists(state), "no primary yet");

  arb_state(&arb, &params, 3, RELAIS_NODE_A, true, RELAIS_NODE_B);
  check(relais_persist_save_arb(&p, &arb) == 0, "save #3");
  check(file_exists(state) && file_exists(p.backup), "both copies written");
  char tmp[640];
  snprintf(tmp, sizeof tmp, "%s.tmp", state);
  check(!file_exists(tmp), "no .tmp left behind");
  check(relais_persist_save_arb(&p, &arb) == 1, "wear-limited skip");

  arb_state(&arb2, &params, 4, RELAIS_NODE_B, false, RELAIS_NODE_NONE);
  check(relais_persist_save_arb(&p, &arb2) == 0, "save #4");
}

/* Restore the latest state from a fresh handle. */
static void test_restore(const char *state) {
  relais_arb arb3;
  relais_params params;
  arb_state(&arb3, &params, 0, RELAIS_NODE_NONE, false, RELAIS_NODE_NONE);
  relais_persist p2;
  check(relais_persist_init(&p2, state) == 0, "init p2");
  check(relais_persist_load_arb(&p2, &arb3) == 0, "load arb");
  check(arb3.mandate == 4 && arb3.holder == RELAIS_NODE_B, "mandate 4 / holder B");
  check(!p2.recovered, "primary used, not backup");
}

/* Corrupt / truncate / delete the primary; the healthy backup must recover. */
static void test_primary_damage(const char *state) {
  relais_params params;
  uint8_t bad[64];
  memset(bad, 0xAA, sizeof bad);
  dump(state, bad, 20); /* bit-flip corruption */
  relais_arb arb4;
  arb_state(&arb4, &params, 0, RELAIS_NODE_NONE, false, RELAIS_NODE_NONE);
  relais_persist p3;
  check(relais_persist_init(&p3, state) == 0, "init p3");
  check(relais_persist_load_arb(&p3, &arb4) == 0, "recover from corrupt primary");
  check(p3.recovered && arb4.mandate == 4, "corrupt -> backup #4");

  dump(state, (const uint8_t *)"RPS1", 4); /* truncated (crash mid-write) */
  relais_arb arb5;
  arb_state(&arb5, &params, 0, RELAIS_NODE_NONE, false, RELAIS_NODE_NONE);
  relais_persist p4;
  check(relais_persist_init(&p4, state) == 0, "init p4");
  check(relais_persist_load_arb(&p4, &arb5) == 0, "recover from truncated primary");
  check(p4.recovered && arb5.mandate == 4, "truncated -> backup #4");

  unlink(state); /* missing primary (rename lost it) */
  relais_arb arb6;
  arb_state(&arb6, &params, 0, RELAIS_NODE_NONE, false, RELAIS_NODE_NONE);
  relais_persist p5;
  check(relais_persist_init(&p5, state) == 0, "init p5");
  check(relais_persist_load_arb(&p5, &arb6) == 0, "recover from missing primary");
  check(p5.recovered && arb6.mandate == 4, "missing primary -> backup #4");
}

/* Both copies corrupt -> load refuses, no silent trust (SEC-07). */
static void test_both_corrupt(const char *state) {
  relais_persist p5;
  relais_persist_init(&p5, state);
  dump(state, (const uint8_t *)"junk", 4);
  dump(p5.backup, (const uint8_t *)"junk", 4);
  relais_params params;
  relais_arb arb7;
  arb_state(&arb7, &params, 0, RELAIS_NODE_NONE, false, RELAIS_NODE_NONE);
  relais_persist p6;
  check(relais_persist_init(&p6, state) == 0, "init p6");
  check(relais_persist_load_arb(&p6, &arb7) == -1, "both corrupt -> -1");
}

/* MAN-11: the mandate survives a restart of the actuator. */
static void test_man11(const char *state) {
  relais_params params;
  relais_arb arb8, arb9;
  arb_state(&arb8, &params, 42, RELAIS_NODE_A, true, RELAIS_NODE_B);
  unlink(state);
  relais_persist p7;
  check(relais_persist_init(&p7, state) == 0, "init p7");
  check(relais_persist_save_arb(&p7, &arb8) == 0, "save #42");
  arb_state(&arb9, &params, 0, RELAIS_NODE_NONE, false, RELAIS_NODE_NONE);
  check(relais_persist_load_arb(&p7, &arb9) == 0, "load #42");
  check(arb9.mandate == 42, "mandate survives restart (MAN-11)");
  unlink(state);
  unlink(p7.backup);
}

/* The layer is not arbiter-only: a generic byte round-trip. */
static void test_generic_bytes(const char *dir) {
  char raw[600];
  tmp_path(raw, sizeof raw, dir, "raw.bin");
  uint8_t blob[8] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4};
  relais_persist pr;
  check(relais_persist_init(&pr, raw) == 0, "init raw");
  check(relais_persist_save(&pr, blob, sizeof blob) == 0, "save raw");
  uint8_t got[16];
  size_t got_len = 0;
  check(relais_persist_load(&pr, got, sizeof got, &got_len) == 0, "load raw");
  check(got_len == sizeof blob && memcmp(got, blob, sizeof blob) == 0, "raw equal");
  unlink(raw);
  unlink(pr.backup);
}

int main(void) {
  char dir[] = "/tmp/relais-persist-XXXXXX";
  check(mkdtemp(dir) != NULL, "mkdtemp");
  char state[600];
  tmp_path(state, sizeof state, dir, "arb.state");

  test_save_and_wear(state);
  test_restore(state);
  test_primary_damage(state);
  test_both_corrupt(state);
  test_man11(state);
  test_generic_bytes(dir);

  rmdir(dir);
  if (g_failures == 0) {
    printf("OK %ld checks\n", g_checks);
    return 0;
  }
  fprintf(stderr, "%ld/%ld checks failed\n", g_failures, g_checks);
  return 1;
}