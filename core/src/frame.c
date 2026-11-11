/* Relais — the wire format (spec 6.1, 6.2) and its guards.
 *
 * Header, 24 bytes, big-endian, written byte by byte so the layout does not
 * depend on the host:
 *   magic 4 | version 1 | type 1 | sender 1 | flags 1 | tick 8 | mandate 4
 *   payload_len 2 | reserved 2
 * then the payload of the type, then the 8-byte control (CRC-32C at level S,
 * keyed MAC at level C, SEC-02).
 *
 * Payload sizes are fixed per type, so a frame of an unexpected length is
 * dropped as malformed (SEC-01, DET-04). Requirements covered: MAN-02 (the
 * tick label and the mandate travel with every order), SEC-03 (the tick label
 * makes a replayed frame stale), ROB-09 (the parser never trusts its input).
 */
#include "relais/frame.h"

#include <string.h>

#define PAYLOAD_STATUT 24u
#define PAYLOAD_ORDRE 37u
#define PAYLOAD_OMBRE 5u
#define PAYLOAD_DEMANDE 43u
#define PAYLOAD_CESSION 1u
#define PAYLOAD_SUIVI 2u

/* A cursor over a fixed buffer: every access is bounds checked, so a short or
 * forged frame can never read or write outside of it (ROB-09). */
typedef struct {
  uint8_t *buf;
  size_t at;
  size_t cap;
  bool ok;
} cur_t;

static void put_u8(cur_t *c, uint8_t v) {
  if (c->at + 1 > c->cap) { c->ok = false; return; }
  c->buf[c->at++] = v;
}

static void put_u16(cur_t *c, uint16_t v) {
  if (c->at + 2 > c->cap) { c->ok = false; return; }
  c->buf[c->at++] = (uint8_t)(v >> 8);
  c->buf[c->at++] = (uint8_t)(v & 0xffu);
}

static void put_u32(cur_t *c, uint32_t v) {
  if (c->at + 4 > c->cap) { c->ok = false; return; }
  for (int i = 3; i >= 0; i--) c->buf[c->at++] = (uint8_t)((v >> (i * 8)) & 0xffu);
}

static void put_u64(cur_t *c, uint64_t v) {
  if (c->at + 8 > c->cap) { c->ok = false; return; }
  for (int i = 7; i >= 0; i--) c->buf[c->at++] = (uint8_t)((v >> (i * 8)) & 0xffu);
}

static void put_f32(cur_t *c, float v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof bits);
  put_u32(c, bits);
}

static void put_bytes(cur_t *c, const uint8_t *src, size_t len) {
  if (c->at + len > c->cap) { c->ok = false; return; }
  memcpy(c->buf + c->at, src, len);
  c->at += len;
}

static uint8_t get_u8(cur_t *c) {
  if (c->at + 1 > c->cap) { c->ok = false; return 0; }
  return c->buf[c->at++];
}

static uint16_t get_u16(cur_t *c) {
  if (c->at + 2 > c->cap) { c->ok = false; return 0; }
  uint16_t v = (uint16_t)((uint16_t)c->buf[c->at] << 8 | c->buf[c->at + 1]);
  c->at += 2;
  return v;
}

static uint32_t get_u32(cur_t *c) {
  if (c->at + 4 > c->cap) { c->ok = false; return 0; }
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v = (v << 8) | c->buf[c->at++];
  return v;
}

static uint64_t get_u64(cur_t *c) {
  if (c->at + 8 > c->cap) { c->ok = false; return 0; }
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | c->buf[c->at++];
  return v;
}

static float get_f32(cur_t *c) {
  uint32_t bits = get_u32(c);
  float v;
  memcpy(&v, &bits, sizeof v);
  return v;
}

/* Public helpers, also used by the state export of the control law. */
size_t relais_frame_write_u64(uint8_t *p, uint64_t v) { cur_t c = {p, 0, 8, true}; put_u64(&c, v); return 8; }
size_t relais_frame_write_u32(uint8_t *p, uint32_t v) { cur_t c = {p, 0, 4, true}; put_u32(&c, v); return 4; }
size_t relais_frame_write_f32(uint8_t *p, float v) { cur_t c = {p, 0, 4, true}; put_f32(&c, v); return 4; }
uint64_t relais_frame_read_u64(const uint8_t *p) { cur_t c = {(uint8_t *)p, 0, 8, true}; return get_u64(&c); }
uint32_t relais_frame_read_u32(const uint8_t *p) { cur_t c = {(uint8_t *)p, 0, 4, true}; return get_u32(&c); }
float relais_frame_read_f32(const uint8_t *p) { cur_t c = {(uint8_t *)p, 0, 4, true}; return get_f32(&c); }

void relais_msg_clear(relais_msg *m) {
  if (m != NULL) memset(m, 0, sizeof *m);
}

bool relais_msg_to_peer(relais_msg_type type) {
  return type == RELAIS_MSG_ETAT || type == RELAIS_MSG_SUIVI;
}

static size_t etat_payload(const relais_msg *m) {
  return 5u + (size_t)m->ctl_state_len;
}

size_t relais_frame_encoded_size(const relais_msg *m) {
  if (m == NULL) return 0;
  size_t payload = 0;
  switch (m->type) {
    case RELAIS_MSG_STATUT: payload = PAYLOAD_STATUT; break;
    case RELAIS_MSG_ORDRE: payload = PAYLOAD_ORDRE; break;
    case RELAIS_MSG_OMBRE: payload = PAYLOAD_OMBRE; break;
    case RELAIS_MSG_DEMANDE: payload = PAYLOAD_DEMANDE; break;
    case RELAIS_MSG_CESSION: payload = PAYLOAD_CESSION; break;
    case RELAIS_MSG_ETAT: payload = etat_payload(m); break;
    case RELAIS_MSG_SUIVI: payload = PAYLOAD_SUIVI; break;
    default: return 0;
  }
  if (payload > RELAIS_PAYLOAD_MAX) return 0;
  return RELAIS_HEADER_LEN + payload + RELAIS_CHECK_LEN;
}

static void put_testament(cur_t *c, const relais_msg *m) {
  put_u8(c, m->testament_len);
  for (uint8_t i = 0; i < RELAIS_TESTAMENT_MAX; i++)
    put_f32(c, i < m->testament_len ? m->testament[i] : 0.0f);
}

static void get_testament(cur_t *c, relais_msg *m) {
  uint8_t len = get_u8(c);
  m->testament_len = len > RELAIS_TESTAMENT_MAX ? RELAIS_TESTAMENT_MAX : len;
  for (uint8_t i = 0; i < RELAIS_TESTAMENT_MAX; i++) {
    float v = get_f32(c);
    if (i < m->testament_len) m->testament[i] = v;
  }
}

static void put_payload(cur_t *c, const relais_msg *m) {
  switch (m->type) {
    case RELAIS_MSG_STATUT:
      put_u8(c, (uint8_t)m->holder);
      put_u8(c, (uint8_t)m->source);
      put_u8(c, m->misses);
      put_u8(c, (uint8_t)(m->certified ? 1 : 0));
      put_u8(c, (uint8_t)m->certified_node);
      put_u8(c, (uint8_t)m->lamp);
      put_u8(c, (uint8_t)m->cause);
      put_u8(c, (uint8_t)(m->stop_requested ? 1 : 0));
      put_f32(c, m->meas.pos);
      put_f32(c, m->meas.vel);
      put_f32(c, m->meas.applied);
      put_f32(c, m->meas.setpoint);
      break;
    case RELAIS_MSG_ORDRE:
      put_f32(c, m->value);
      put_testament(c, m);
      break;
    case RELAIS_MSG_OMBRE:
      put_f32(c, m->value);
      put_u8(c, m->replica_age);
      break;
    case RELAIS_MSG_DEMANDE:
      put_u32(c, m->seen_mandate);
      put_u8(c, m->reason);
      put_u8(c, (uint8_t)(m->has_order ? 1 : 0));
      put_f32(c, m->value);
      put_testament(c, m);
      break;
    case RELAIS_MSG_CESSION:
      put_u8(c, m->reason);
      break;
    case RELAIS_MSG_ETAT:
      put_f32(c, m->last_value);
      put_u8(c, m->ctl_state_len);
      put_bytes(c, m->ctl_state, m->ctl_state_len);
      break;
    case RELAIS_MSG_SUIVI:
      put_u8(c, m->replica_age);
      put_u8(c, (uint8_t)(m->ready ? 1 : 0));
      break;
    default:
      c->ok = false;
      break;
  }
}

size_t relais_frame_encode(const relais_msg *m, const relais_key *k, uint8_t *buf, size_t cap) {
  size_t total = relais_frame_encoded_size(m);
  if (total == 0 || buf == NULL || cap < total) return 0;
  memset(buf, 0, total);
  cur_t c = {buf, 0, total, true};
  put_u32(&c, RELAIS_MAGIC);
  put_u8(&c, RELAIS_VERSION);
  put_u8(&c, (uint8_t)m->type);
  put_u8(&c, (uint8_t)m->sender);
  put_u8(&c, m->flags);
  put_u64(&c, m->tick);
  put_u32(&c, m->mandate);
  put_u16(&c, (uint16_t)(total - RELAIS_HEADER_LEN - RELAIS_CHECK_LEN));
  put_u16(&c, 0);
  put_payload(&c, m);
  if (!c.ok) return 0;
  uint64_t check = relais_check_compute(k, buf, total - RELAIS_CHECK_LEN);
  c.at = total - RELAIS_CHECK_LEN;
  put_u64(&c, check);
  return c.ok ? total : 0;
}

static bool get_payload(cur_t *c, relais_msg *m, size_t payload_len) {
  switch (m->type) {
    case RELAIS_MSG_STATUT:
      if (payload_len != PAYLOAD_STATUT) return false;
      m->holder = (relais_node)get_u8(c);
      m->source = (relais_source)get_u8(c);
      m->misses = get_u8(c);
      m->certified = get_u8(c) != 0;
      m->certified_node = (relais_node)get_u8(c);
      m->lamp = (relais_lamp)get_u8(c);
      m->cause = (relais_cause)get_u8(c);
      m->stop_requested = get_u8(c) != 0;
      m->meas.pos = get_f32(c);
      m->meas.vel = get_f32(c);
      m->meas.applied = get_f32(c);
      m->meas.setpoint = get_f32(c);
      break;
    case RELAIS_MSG_ORDRE:
      if (payload_len != PAYLOAD_ORDRE) return false;
      m->value = get_f32(c);
      m->has_order = true;
      get_testament(c, m);
      break;
    case RELAIS_MSG_OMBRE:
      if (payload_len != PAYLOAD_OMBRE) return false;
      m->value = get_f32(c);
      m->replica_age = get_u8(c);
      break;
    case RELAIS_MSG_DEMANDE:
      if (payload_len != PAYLOAD_DEMANDE) return false;
      m->seen_mandate = get_u32(c);
      m->reason = get_u8(c);
      m->has_order = get_u8(c) != 0;
      m->value = get_f32(c);
      get_testament(c, m);
      break;
    case RELAIS_MSG_CESSION:
      if (payload_len != PAYLOAD_CESSION) return false;
      m->reason = get_u8(c);
      break;
    case RELAIS_MSG_ETAT:
      if (payload_len < 5u || payload_len > 5u + RELAIS_CTL_STATE_MAX) return false;
      m->last_value = get_f32(c);
      m->ctl_state_len = get_u8(c);
      if ((size_t)m->ctl_state_len + 5u != payload_len) return false;
      for (uint8_t i = 0; i < m->ctl_state_len; i++) m->ctl_state[i] = get_u8(c);
      break;
    case RELAIS_MSG_SUIVI:
      if (payload_len != PAYLOAD_SUIVI) return false;
      m->replica_age = get_u8(c);
      m->ready = get_u8(c) != 0;
      break;
    default:
      return false;
  }
  return c->ok;
}

static bool decode_common(const uint8_t *buf, size_t len, relais_msg *out, bool with_check,
                          const relais_key *k, relais_reject *why) {
  cur_t c = {(uint8_t *)buf, 0, len, true};
  if (len < RELAIS_HEADER_LEN + RELAIS_CHECK_LEN) {
    if (why != NULL) *why = RELAIS_REJ_MALFORMED;
    return false;
  }
  if (get_u32(&c) != RELAIS_MAGIC) {
    if (why != NULL) *why = RELAIS_REJ_MALFORMED;
    return false;
  }
  if (get_u8(&c) != RELAIS_VERSION) {
    if (why != NULL) *why = RELAIS_REJ_VERSION;
    return false;
  }
  relais_msg_clear(out);
  out->type = (relais_msg_type)get_u8(&c);
  out->sender = (relais_node)get_u8(&c);
  out->flags = get_u8(&c);
  out->tick = get_u64(&c);
  out->mandate = get_u32(&c);
  uint16_t payload_len = get_u16(&c);
  get_u16(&c); /* reserved */
  bool sender_ok =
      relais_node_is_computer(out->sender) || out->sender == RELAIS_NODE_ACTUATOR;
  if (out->type < RELAIS_MSG_STATUT || out->type > RELAIS_MSG_SUIVI || !sender_ok) {
    if (why != NULL) *why = RELAIS_REJ_MALFORMED;
    return false;
  }
  if ((size_t)payload_len + RELAIS_HEADER_LEN + RELAIS_CHECK_LEN != len) {
    if (why != NULL) *why = RELAIS_REJ_MALFORMED;
    return false;
  }
  if (with_check && !relais_check_verify(k, buf, len - RELAIS_CHECK_LEN,
                                         relais_frame_read_u64(buf + len - RELAIS_CHECK_LEN))) {
    if (why != NULL) *why = RELAIS_REJ_CHECK;
    return false;
  }
  if (!get_payload(&c, out, payload_len)) {
    if (why != NULL) *why = RELAIS_REJ_MALFORMED;
    return false;
  }
  return true;
}

bool relais_frame_decode(const uint8_t *buf, size_t len, const relais_key *k, relais_msg *out,
                         relais_reject *why) {
  if (buf == NULL || out == NULL) return false;
  return decode_common(buf, len, out, true, k, why);
}

bool relais_frame_decode_unchecked(const uint8_t *buf, size_t len, relais_msg *out) {
  if (buf == NULL || out == NULL) return false;
  return decode_common(buf, len, out, false, NULL, NULL);
}
