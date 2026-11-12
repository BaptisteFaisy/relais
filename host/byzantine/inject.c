/* Relais — byzantine frame injector (mission M20). See inject.h. */
#include "inject.h"

#include <string.h>

void relais_byz_key(relais_key *k) {
  if (k == NULL) return;
  relais_key_clear(k);
  /* The bench link key, the same secret the actuator and both computers share
   * at level C. An attacker holding it is an insider (SEC-02). */
  relais_key_from_hex(k, "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
}

bool relais_byz_roundtrip(const relais_msg *m, const relais_key *k, relais_msg *out, uint8_t *buf,
                          size_t cap) {
  if (m == NULL || out == NULL || buf == NULL) return false;
  size_t len = relais_frame_encode(m, k, buf, cap);
  if (len == 0) return false; /* does not fit: not a wire-valid frame */
  relais_reject why = RELAIS_REJ_NONE;
  return relais_frame_decode(buf, len, k, out, &why);
}

static void flat_will(relais_msg *m, float value, const relais_params *p) {
  uint8_t len = (p != NULL) ? p->testament_len : RELAIS_TESTAMENT_MAX;
  if (len > RELAIS_TESTAMENT_MAX) len = RELAIS_TESTAMENT_MAX;
  m->testament_len = len;
  for (uint8_t i = 0; i < len; i++) m->testament[i] = value;
}

void relais_byz_order(relais_msg *m, relais_node from, relais_mandate mandate, relais_tick tick,
                      float value, const relais_params *p) {
  if (m == NULL) return;
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ORDRE;
  m->sender = from;
  m->mandate = mandate;
  m->tick = tick;
  m->value = value;
  m->has_order = true;
  flat_will(m, value, p);
}

void relais_byz_fake_statut(relais_msg *m, relais_node from, relais_tick tick,
                            relais_mandate mandate, relais_node holder) {
  if (m == NULL) return;
  relais_msg_clear(m);
  m->type = RELAIS_MSG_STATUT;
  m->sender = from; /* a computer usurping the actuator, or a forged actuator */
  m->tick = tick;
  m->mandate = mandate;
  m->holder = holder;
  m->source = RELAIS_SRC_HOLDER;
  m->lamp = RELAIS_LAMP_OFF;
  m->cause = RELAIS_CAUSE_NONE;
}

void relais_byz_cession(relais_msg *m, relais_node claimed, relais_mandate mandate,
                        uint8_t reason) {
  if (m == NULL) return;
  relais_msg_clear(m);
  m->type = RELAIS_MSG_CESSION;
  m->sender = claimed; /* the holder being usurped; the emitter is another one */
  m->mandate = mandate;
  m->flags = RELAIS_FLAG_CESSION;
  m->reason = reason;
}

void relais_byz_etat(relais_msg *m, relais_node from, relais_tick tick, float last_value) {
  if (m == NULL) return;
  relais_msg_clear(m);
  m->type = RELAIS_MSG_ETAT;
  m->sender = from;
  m->tick = tick;
  m->last_value = last_value;
  /* A short but corrupt control state, authenticated with the stolen key. */
  m->ctl_state_len = 4;
  m->ctl_state[0] = 0xde;
  m->ctl_state[1] = 0xad;
  m->ctl_state[2] = 0xbe;
  m->ctl_state[3] = 0xef;
}

void relais_byz_demande(relais_msg *m, relais_node from, relais_mandate seen, relais_tick tick,
                        bool with_order, float value, const relais_params *p) {
  if (m == NULL) return;
  relais_msg_clear(m);
  m->type = RELAIS_MSG_DEMANDE;
  m->sender = from;
  m->tick = tick;
  m->seen_mandate = seen;
  m->reason = 1;
  m->has_order = with_order;
  m->flags = (uint8_t)(with_order ? RELAIS_FLAG_HAS_ORDER : 0u);
  if (with_order) {
    m->value = value;
    flat_will(m, value, p);
  }
}
