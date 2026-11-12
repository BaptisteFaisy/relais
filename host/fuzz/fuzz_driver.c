/* Relais — home-grown fuzzing driver, used when clang/libFuzzer is absent.
 *
 * ROB-09 wants the parser to survive hostile bytes. With clang the target is
 * driven by libFuzzer (coverage guided); without it, this driver plays the same
 * role with a seeded generator: random buffers, plus mutations of the real
 * frames of the corpus (bit flip, byte set, insert, delete, splice, interesting
 * values). The generator is deterministic: the same seed and the same corpus
 * give the same sequence of inputs, so a run is reproducible.
 *
 * The sanitizers are the point: the target is built with
 * -fsanitize=address,undefined and -fno-sanitize-recover=undefined, so a memory
 * error or an undefined behaviour stops the run at once.
 *
 * A failing input is kept: the driver holds the last input in memory and writes
 * it to the crash file from the sanitizer death callback (ASAN errors) or from a
 * chained signal handler (SIGILL of a broken contract, SIGABRT of UBSAN,
 * SIGSEGV), so the case can be replayed with --input.
 *
 * Usage: fuzz_frame [--budget SEC] [--seed N] [--corpus DIR] [--max-len N]
 *                   [--runs N] [--input FILE] [--crash-file PATH]
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#if defined(__SANITIZE_ADDRESS__)
extern void __sanitizer_set_death_callback(void (*callback)(void));
#define FUZZ_HAVE_DEATH_CALLBACK 1
#endif

#define MAX_INPUT 4096u
#define MAX_SEEDS 64u
#define HANDLED_SIGNALS 5u

static uint8_t g_seeds[MAX_SEEDS][MAX_INPUT];
static size_t g_seed_len[MAX_SEEDS];
static size_t g_seed_count;

static uint8_t g_last[MAX_INPUT];
static size_t g_last_len;
static int g_crash_fd = -1;
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static const int g_signals[HANDLED_SIGNALS] = {SIGSEGV, SIGILL, SIGABRT, SIGFPE, SIGBUS};
static struct sigaction g_old[HANDLED_SIGNALS];

/* --- keeping the failing input ----------------------------------------- */

static void save_input(void) {
  if (g_crash_fd < 0 || g_last_len == 0u) return;
  (void)!pwrite(g_crash_fd, g_last, g_last_len, 0);
  (void)!ftruncate(g_crash_fd, (off_t)g_last_len);
}

static void chain_old(int sig, siginfo_t *si, void *uc) {
  for (size_t i = 0; i < HANDLED_SIGNALS; i++) {
    if (g_signals[i] != sig) continue;
    const struct sigaction *old = &g_old[i];
    if ((old->sa_flags & SA_SIGINFO) != 0 && old->sa_sigaction != NULL) {
      old->sa_sigaction(sig, si, uc);
      return;
    }
    if (old->sa_handler != NULL && old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
      old->sa_handler(sig);
      return;
    }
  }
  struct sigaction dfl;
  memset(&dfl, 0, sizeof dfl);
  dfl.sa_handler = SIG_DFL;
  sigaction(sig, &dfl, NULL);
  raise(sig);
}

static void on_signal(int sig, siginfo_t *si, void *uc) {
  save_input();
  chain_old(sig, si, uc);
}

static void install_handlers(void) {
  for (size_t i = 0; i < HANDLED_SIGNALS; i++) {
    struct sigaction act;
    memset(&act, 0, sizeof act);
    act.sa_sigaction = on_signal;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    sigaction(g_signals[i], &act, &g_old[i]);
  }
#ifdef FUZZ_HAVE_DEATH_CALLBACK
  __sanitizer_set_death_callback(save_input);
#endif
}

/* --- the generator ----------------------------------------------------- */

static uint64_t rng_next(void) {
  uint64_t x = g_rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  g_rng = x;
  return x * 0x2545F4914F6CDD1DULL;
}

static size_t rng_below(size_t n) {
  if (n == 0u) return 0u;
  return (size_t)(rng_next() % (uint64_t)n);
}

static void load_corpus(const char *dir) {
  DIR *d = opendir(dir);
  if (d == NULL) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL && g_seed_count < MAX_SEEDS) {
    if (e->d_name[0] == '.') continue;
    char path[512];
    int n = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (n <= 0 || (size_t)n >= sizeof path) continue;
    FILE *f = fopen(path, "rb");
    if (f == NULL) continue;
    size_t got = fread(g_seeds[g_seed_count], 1u, MAX_INPUT, f);
    fclose(f);
    if (got == 0u) continue;
    g_seed_len[g_seed_count] = got;
    g_seed_count++;
  }
  closedir(d);
}

/* One havoc pass over a copy of a seed. */
static size_t mutate(uint8_t *buf, size_t len, size_t max_len) {
  unsigned ops = 1u + (unsigned)rng_below(4u);
  for (unsigned op = 0; op < ops; op++) {
    size_t choice = rng_below(7u);
    if (len == 0u) {
      buf[0] = (uint8_t)rng_next();
      len = 1u;
    } else if (choice == 0u) {
      buf[rng_below(len)] ^= (uint8_t)(1u << (rng_next() & 7u));
    } else if (choice == 1u) {
      buf[rng_below(len)] = (uint8_t)rng_next();
    } else if (choice == 2u) {
      static const uint8_t vals[] = {0x00u, 0x01u, 0x7fu, 0x80u, 0xffu};
      buf[rng_below(len)] = vals[rng_below(sizeof vals)];
    } else if (choice == 3u && len + 1u <= max_len) {
      size_t at = rng_below(len);
      memmove(buf + at + 1u, buf + at, len - at);
      buf[at] = (uint8_t)rng_next();
      len++;
    } else if (choice == 4u && len > 1u) {
      size_t at = rng_below(len);
      memmove(buf + at, buf + at + 1u, len - at - 1u);
      len--;
    } else if (choice == 5u && g_seed_count > 0u) {
      size_t si = rng_below(g_seed_count);
      size_t at = rng_below(len);
      size_t take = rng_below(g_seed_len[si]);
      if (take > max_len - at) take = max_len - at;
      memcpy(buf + at, g_seeds[si], take);
      len = at + take;
    } else {
      size_t at = rng_below(len);
      size_t take = 1u + rng_below(len - at);
      for (size_t i = 0; i < take; i++) buf[at + i] = (uint8_t)rng_next();
    }
  }
  return len;
}

static size_t next_input(uint8_t *buf, size_t max_len) {
  if (g_seed_count > 0u && rng_below(4u) != 0u) {
    size_t si = rng_below(g_seed_count);
    size_t len = g_seed_len[si];
    if (len > max_len) len = max_len; /* a seed longer than the budget is cut */
    memcpy(buf, g_seeds[si], len);
    return mutate(buf, len, max_len);
  }
  size_t len = rng_below(max_len + 1u);
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rng_next();
  return len;
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* --- the run ----------------------------------------------------------- */

static int run_once(const char *file, size_t max_len) {
  static uint8_t buf[MAX_INPUT];
  FILE *f = fopen(file, "rb");
  if (f == NULL) {
    fprintf(stderr, "fuzz: cannot open %s\n", file);
    return 2;
  }
  size_t len = fread(buf, 1u, max_len, f);
  fclose(f);
  g_last_len = len;
  memcpy(g_last, buf, len);
  return LLVMFuzzerTestOneInput(buf, len);
}

static void usage(void) {
  fprintf(stderr, "usage: %s [--budget SEC] [--seed N] [--corpus DIR] [--max-len N]\n"
                  "       [--runs N] [--input FILE] [--crash-file PATH]\n",
          "fuzz_target");
}

typedef struct {
  double budget;
  unsigned long long seed;
  unsigned long long runs;
  size_t max_len;
  const char *corpus;
  const char *input;
  const char *crash_file;
} opts_t;

static bool parse_opts(int argc, char **argv, opts_t *o) {
  o->budget = 60.0;
  o->seed = 0xC0FFEEULL;
  o->runs = 0;
  o->max_len = 512u;
  o->corpus = NULL;
  o->input = NULL;
  o->crash_file = NULL;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--budget") == 0 && i + 1 < argc) o->budget = atof(argv[++i]);
    else if (strcmp(a, "--seed") == 0 && i + 1 < argc) o->seed = strtoull(argv[++i], NULL, 0);
    else if (strcmp(a, "--corpus") == 0 && i + 1 < argc) o->corpus = argv[++i];
    else if (strcmp(a, "--max-len") == 0 && i + 1 < argc)
      o->max_len = (size_t)strtoul(argv[++i], NULL, 0);
    else if (strcmp(a, "--runs") == 0 && i + 1 < argc) o->runs = strtoull(argv[++i], NULL, 0);
    else if (strcmp(a, "--input") == 0 && i + 1 < argc) o->input = argv[++i];
    else if (strcmp(a, "--crash-file") == 0 && i + 1 < argc) o->crash_file = argv[++i];
    else {
      usage();
      return false;
    }
  }
  return true;
}

/* The bounded run itself: generate, call the target, count, stop on the budget. */
static int fuzz_loop(const opts_t *o, const char *argv0) {
  static uint8_t buf[MAX_INPUT];
  if (o->crash_file != NULL)
    g_crash_fd = open(o->crash_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (o->corpus != NULL) load_corpus(o->corpus);
  fprintf(stderr,
          "FUZZ-BACKEND homegrown (no clang/libFuzzer here): gcc "
          "-fsanitize=address,undefined + seeded generator, seed=%llu, corpus seeds=%zu\n",
          o->seed, g_seed_count);

  double start = now_seconds();
  unsigned long long execs = 0;
  while (o->runs == 0u || execs < o->runs) {
    if (now_seconds() - start >= o->budget) break;
    size_t len = next_input(buf, o->max_len);
    g_last_len = len;
    memcpy(g_last, buf, len);
    (void)LLVMFuzzerTestOneInput(buf, len);
    execs++;
  }
  double elapsed = now_seconds() - start;
  const char *slash = strrchr(argv0, '/');
  const char *name = slash != NULL ? slash + 1 : argv0;
  printf("FUZZ-SUMMARY target=%s backend=homegrown execs=%llu time=%.1f exec_per_s=%.0f "
         "seed=%llu max_len=%zu corpus=%s crashes=0\n",
         name, execs, elapsed, elapsed > 0.0 ? (double)execs / elapsed : 0.0, o->seed,
         o->max_len, o->corpus != NULL ? o->corpus : "none");
  return 0;
}

int main(int argc, char **argv) {
  opts_t o;
  if (!parse_opts(argc, argv, &o)) return 2;
  if (o.max_len == 0u || o.max_len > MAX_INPUT) o.max_len = MAX_INPUT;
  g_rng = o.seed != 0u ? o.seed : 0x9E3779B97F4A7C15ULL;
  install_handlers();
  if (o.input != NULL) return run_once(o.input, o.max_len);
  return fuzz_loop(&o, argv[0]);
}
