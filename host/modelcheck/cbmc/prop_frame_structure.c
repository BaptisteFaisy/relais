/* Relais — CBMC property P-FRAME-1: the parser accepts a frame only when its
 * header is legal and its declared length matches the bytes present.
 *
 * Proved over an ARBITRARY buffer (every byte nondeterministic, every length
 * up to RELAIS_FRAME_MAX): the parser never trusts its input (ROB-09) and
 * never accepts a frame whose declared payload length does not match the
 * bytes received (SEC-01).
 *
 * Command: see host/modelcheck/cbmc/run.sh.
 */
#include <stddef.h>

#include "mc_cbmc.h"
#include "relais/frame.h"

/* The payload size of every type, docs/PROTOCOL.md section 2. */
static size_t payload_of(uint8_t type) {
  switch (type) {
    case RELAIS_MSG_STATUT: return 24u;
    case RELAIS_MSG_ORDRE: return 37u;
    case RELAIS_MSG_OMBRE: return 5u;
    case RELAIS_MSG_DEMANDE: return 43u;
    case RELAIS_MSG_CESSION: return 1u;
    case RELAIS_MSG_SUIVI: return 2u;
    default: return 0u; /* ETAT: variable, 5 + ctl_state_len */
  }
}

int main(void) {
  uint8_t buf[RELAIS_FRAME_MAX];
  for (size_t i = 0; i < sizeof buf; i++) buf[i] = MC_NONDET_U8();
  uint32_t len = MC_NONDET_U32();
  MC_ASSUME(len <= RELAIS_FRAME_MAX);
  relais_msg out;
  bool ok = relais_frame_decode_unchecked(buf, (size_t)len, &out);
  if (!ok) return 0;
  MC_ASSERT(relais_frame_read_u32(buf) == RELAIS_MAGIC, "P-FRAME-1: magic not checked");
  MC_ASSERT(buf[4] == RELAIS_VERSION, "P-FRAME-1: version not checked");
  MC_ASSERT(buf[5] >= RELAIS_MSG_STATUT && buf[5] <= RELAIS_MSG_SUIVI,
            "P-FRAME-1: type out of the published range");
  MC_ASSERT(buf[6] == RELAIS_NODE_A || buf[6] == RELAIS_NODE_B || buf[6] == RELAIS_NODE_C ||
                buf[6] == RELAIS_NODE_ACTUATOR,
            "P-FRAME-1: sender outside the published set");
  uint16_t paylen = (uint16_t)(((uint16_t)buf[20] << 8) | (uint16_t)buf[21]);
  MC_ASSERT((size_t)paylen + RELAIS_HEADER_LEN + RELAIS_CHECK_LEN == (size_t)len,
            "P-FRAME-1: declared length does not match the bytes received");
  if (buf[5] == RELAIS_MSG_ETAT) {
    MC_ASSERT(paylen >= 5u && paylen <= 5u + RELAIS_CTL_STATE_MAX,
              "P-FRAME-1: ETAT state length out of range");
    MC_ASSERT(buf[28] == (uint8_t)(paylen - 5u), "P-FRAME-1: ETAT state length inconsistent");
  } else {
    MC_ASSERT((size_t)paylen == payload_of(buf[5]),
              "P-FRAME-1: payload size does not match the type");
  }
  MC_ASSERT(out.tick == relais_frame_read_u64(buf + 8), "P-FRAME-1: tick label mangled");
  MC_ASSERT(out.mandate == relais_frame_read_u32(buf + 16), "P-FRAME-1: mandate mangled");
  return 0;
}
