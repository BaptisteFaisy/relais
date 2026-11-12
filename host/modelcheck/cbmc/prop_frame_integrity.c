/* Relais — CBMC property P-FRAME-2: a corrupted frame never reaches the
 * arbiter (docs/PROTOCOL.md 3; SEC-01, SEC-02, ROB-09).
 *
 * The frame is built with the REAL encoder, one byte is corrupted, and the
 * decoder must refuse it: the integrity check covers the whole frame, so a
 * corrupted frame is treated as lost and never arbitrated.
 *
 * The level C case (keyed MAC) is the expensive one: the solver has to
 * reason about SipHash over the whole frame. The command in run.sh bounds the
 * time; the level S case (CRC-32C) is the cheap one.
 *
 * Command: see host/modelcheck/cbmc/run.sh.
 */
#include <stddef.h>
#include <string.h>

#include "mc_cbmc.h"
#include "relais/check.h"
#include "relais/frame.h"

int main(void) {
  relais_key k;
  relais_key_clear(&k);
  k.enabled = false; /* level S: CRC-32C. Level C is the same property with a
                       * keyed SipHash: the command is in run.sh. */
  for (unsigned i = 0; i < sizeof k.bytes; i++) k.bytes[i] = (uint8_t)i;
  relais_msg m;
  memset(&m, 0, sizeof m);
  m.type = RELAIS_MSG_ORDRE;
  m.sender = RELAIS_NODE_A;
  m.tick = 7;
  m.mandate = 3;
  m.value = 0.25f;
  m.has_order = true;
  m.testament_len = 2;
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(&m, &k, buf, sizeof buf);
  MC_ASSUME(len > 0);
  unsigned at = MC_NONDET_U32() % 69u; /* the ORDRE frame is 69 bytes */
  MC_ASSUME(at < (unsigned)len);
  uint8_t value = MC_NONDET_U8();
  MC_ASSUME(value != buf[at]);
  buf[at] = value;
  relais_msg out;
  relais_reject why = RELAIS_REJ_NONE;
  bool ok = relais_frame_decode(buf, len, &k, &out, &why);
  MC_ASSERT(!ok, "P-FRAME-2: a corrupted frame was accepted");
  MC_ASSERT(why == RELAIS_REJ_CHECK, "P-FRAME-2: the corruption was not seen as a check failure");
  return 0;
}
