/* Relais — helpers shared by the three fuzz targets.
 *
 * One cursor, one contract checker, no duplication between the three targets.
 * A target must never fault on its own input handling: every read goes through
 * the cursor, which yields zero bytes once the input is exhausted, so whatever
 * a sanitizer reports comes from the core library, not from the harness.
 */
#ifndef RELAIS_FUZZ_UTIL_H
#define RELAIS_FUZZ_UTIL_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  const uint8_t *data;
  size_t size;
  size_t at;
} fuzz_cur;

static inline void fuzz_cur_init(fuzz_cur *c, const uint8_t *data, size_t size) {
  c->data = data;
  c->size = size;
  c->at = 0;
}

static inline uint8_t fuzz_u8(fuzz_cur *c) {
  return c->at < c->size ? c->data[c->at++] : 0u;
}

static inline uint32_t fuzz_u32(fuzz_cur *c) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v = (v << 8) | (uint32_t)fuzz_u8(c);
  return v;
}

static inline size_t fuzz_below(fuzz_cur *c, size_t n) {
  if (n == 0u) return 0u;
  return (size_t)(fuzz_u32(c) % (uint32_t)n);
}

/* A float the core can be fed: the formatters and the journal cannot carry
 * NaN or infinity, and neither can the control law be judged on them. */
static inline float fuzz_f32(fuzz_cur *c) {
  uint32_t bits = fuzz_u32(c);
  float v;
  memcpy(&v, &bits, sizeof v);
  return isfinite(v) ? v : 0.0f;
}

/* A violated contract is a real defect, not a fuzzing artefact: the location is
 * written out, then the trap makes the sanitizer stop the run and the driver
 * keep the input. write() is async-signal-safe, so this works from any state. */
static inline void fuzz_fail(const char *file, int line) {
  char buf[192];
  int n = snprintf(buf, sizeof buf, "FUZZ-CONTRACT-VIOLATION %s:%d\n", file, line);
  if (n > 0) {
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1u;
    (void)!write(2, buf, len);
  }
  __builtin_trap();
}

#define fuzz_require(cond) \
  do {                     \
    if (!(cond)) fuzz_fail(__FILE__, __LINE__); \
  } while (0)

/* Relative comparison, tolerant of the %.6g the journal writes. */
static inline bool fuzz_close(float got, float want) {
  float scale = fabsf(want) + 1e-30f;
  return fabsf(got - want) <= 1e-5f * scale;
}

#endif /* RELAIS_FUZZ_UTIL_H */
