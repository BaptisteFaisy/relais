/* Relais — Frama-C/WP harness for the frame decoder (mission M36).
 *
 * The harness builds a frame with the REAL encoder, decodes it with the REAL
 * decoder and states what must hold. WP proves the assertions from the
 * contracts of host/modelcheck/wp/relais_core_spec.h, and proves the
 * contracts themselves from the code of core/src/frame.c.
 *
 * Command: see host/modelcheck/wp/run.sh.
 */
#include "relais/frame.h"

int main(void) {
  relais_key k;
  relais_key_clear(&k);
  k.enabled = false; /* level S: CRC-32C */
  relais_msg m;
  relais_msg_clear(&m);
  m.type = RELAIS_MSG_ORDRE;
  m.sender = RELAIS_NODE_A;
  m.tick = 7;
  m.mandate = 3;
  m.value = 0.25f;
  m.has_order = true;
  m.testament_len = 2;
  m.testament[0] = 0.1f;
  m.testament[1] = 0.2f;
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(&m, &k, buf, sizeof buf);
  /*@ assert len > 0; */
  relais_msg out;
  relais_reject why = RELAIS_REJ_NONE;
  bool ok = relais_frame_decode(buf, len, &k, &out, &why);
  (void)ok; /* the assertions below are the statement; the cast keeps gcc quiet */
  /*@ assert ok; */
  /*@ assert out.type == RELAIS_MSG_ORDRE; */
  /*@ assert out.sender == RELAIS_NODE_A; */
  /*@ assert out.tick == 7; */
  /*@ assert out.mandate == 3; */
  return 0;
}
