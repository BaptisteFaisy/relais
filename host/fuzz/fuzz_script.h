/* Relais — the script format of the arbiter fuzz target.
 *
 * The arbiter target does not parse frames: it reads a SCRIPT, a sequence of
 * steps, each step being "how many messages, then those messages, then the
 * measurement". The layout is fixed here so the corpus generator and the target
 * cannot drift apart.
 *
 *   step  := n:u8  record[n]  meas
 *   record:= type:u8 sender:u8 flags:u8 mode:u8 value:f32 testament_len:u8
 *             replica_age:u8 reason:u8 has_order:u8 seen_mandate:u32
 *             testament0:f32 testament1:f32                    (24 bytes)
 *   meas  := pos:f32 vel:f32 applied:f32 setpoint:f32          (16 bytes)
 *
 * mode bit 0 means "aligned with the arbiter": the sender, the mandate and the
 * tick are taken from its current state, so the message is really accepted.
 */
#ifndef RELAIS_FUZZ_SCRIPT_H
#define RELAIS_FUZZ_SCRIPT_H

#include <stdint.h>
#include <string.h>

#define FUZZ_SCRIPT_REC 24u
#define FUZZ_SCRIPT_MEAS 16u
#define FUZZ_SCRIPT_STEP_HEAD 1u
#define FUZZ_SCRIPT_STEP_MAX (FUZZ_SCRIPT_STEP_HEAD + 8u * FUZZ_SCRIPT_REC + FUZZ_SCRIPT_MEAS)

static inline float fuzz_script_f32(const uint8_t *p) {
  float v;
  memcpy(&v, p, sizeof v);
  return v;
}

static inline void fuzz_script_put_f32(uint8_t *p, float v) { memcpy(p, &v, sizeof v); }

static inline uint32_t fuzz_script_u32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, sizeof v);
  return v;
}

static inline void fuzz_script_put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }

#endif /* RELAIS_FUZZ_SCRIPT_H */
