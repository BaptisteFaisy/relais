/* Relais — wire format tests (spec 6.1, 6.2, 6.3): round trips, field by
 * field, encoded sizes, and every rejection the decoder must make.
 *
 * Requirements covered here:
 *   SEC-01  malformed / unknown version / unknown type / unknown sender /
 *           truncated / inconsistent length / bad control are rejected, each
 *           with the code the caller counts (DET-04);
 *   SEC-03  the tick label survives the round trip: that is what makes a
 *           captured frame stale when it is replayed;
 *   ROB-02  a duplicated frame decodes to the same message.
 * ROB-09 (the parser never trusts its input) lives in test_frame_fuzz.c.
 *
 * The fixtures below zero the message before filling every field its type
 * carries, so the round trip can be compared with a single memcmp: a field the
 * codec forgets shows up as a difference, not as a silent zero.
 */
#include <string.h>

#include "test_util.h"

/* A tick wider than 32 bits and a mandate with the high bit set: the tests
 * prove both survive the round trip (SEC-03, MAN-02). */
#define FX_TICK 0x0123456789abcdefull
#define FX_MANDATE 0xdeadbeefu

/* Deterministic payload byte for the replicated control state. */
static uint8_t fx_byte(unsigned i) { return (uint8_t)(i * 7u + 3u); }

static void fx_fill_meas(relais_meas *m) {
  m->pos = 0.125f;
  m->vel = -0.0625f;
  m->applied = 0.375f;
  m->setpoint = -0.875f;
}

static void fx_fill_testament(relais_msg *m) {
  m->testament_len = RELAIS_TESTAMENT_MAX; /* N = 8, the full horizon */
  for (uint8_t i = 0; i < RELAIS_TESTAMENT_MAX; i++)
    m->testament[i] = (float)i * 0.125f - 0.5f;
}

/* Every field of the message, for the type given. */
static void fx_fill(relais_msg *m, relais_msg_type type) {
  relais_msg_clear(m);
  m->type = type;
  m->sender = RELAIS_NODE_A;
  m->flags = RELAIS_FLAG_READY | RELAIS_FLAG_TEST;
  m->tick = FX_TICK;
  m->mandate = FX_MANDATE;
  switch (type) {
    case RELAIS_MSG_STATUT:
      m->sender = RELAIS_NODE_ACTUATOR;
      m->holder = RELAIS_NODE_B;
      m->source = RELAIS_SRC_TESTAMENT;
      m->misses = 2;
      m->certified = true;
      m->certified_node = RELAIS_NODE_A;
      m->lamp = RELAIS_LAMP_YELLOW;
      m->cause = RELAIS_CAUSE_LINK_LOST;
      m->stop_requested = true;
      fx_fill_meas(&m->meas);
      break;
    case RELAIS_MSG_ORDRE:
      m->value = -0.4375f;
      m->has_order = true; /* set by the decoder: an order carries a value */
      fx_fill_testament(m);
      break;
    case RELAIS_MSG_OMBRE:
      m->value = 0.6875f;
      m->replica_age = 17;
      break;
    case RELAIS_MSG_DEMANDE:
      m->seen_mandate = 0x0badf00du;
      m->reason = 3;
      m->has_order = true;
      m->flags |= RELAIS_FLAG_HAS_ORDER;
      m->value = 0.25f;
      fx_fill_testament(m);
      break;
    case RELAIS_MSG_CESSION:
      m->reason = 9;
      m->flags |= RELAIS_FLAG_CESSION;
      break;
    case RELAIS_MSG_ETAT:
      m->last_value = -0.1875f;
      m->ctl_state_len = RELAIS_CTL_STATE_MAX - 1u; /* 255: uint8_t ceiling */
      for (uint8_t i = 0; i < m->ctl_state_len; i++) m->ctl_state[i] = fx_byte(i);
      break;
    case RELAIS_MSG_SUIVI:
      m->replica_age = 42;
      m->ready = true;
      break;
  }
}

/* Encode a fixture of the given type; returns the encoded size (0 on error). */
static size_t fx_encode(relais_msg_type type, const relais_key *k, uint8_t *buf, size_t cap) {
  relais_msg m;
  fx_fill(&m, type);
  return relais_frame_encode(&m, k, buf, cap);
}
static const relais_msg_type ALL_TYPES[] = {
    RELAIS_MSG_STATUT,  RELAIS_MSG_ORDRE, RELAIS_MSG_OMBRE, RELAIS_MSG_DEMANDE,
    RELAIS_MSG_CESSION, RELAIS_MSG_ETAT,  RELAIS_MSG_SUIVI};

#define TYPE_COUNT (sizeof ALL_TYPES / sizeof ALL_TYPES[0])

static void fill_key(relais_key *k) {
  relais_key_clear(k);
  T_CHECK(relais_key_from_hex(k, "0f1e2d3c4b5a69788796a5b4c3d2e1f0"));
  T_CHECK(k->enabled);
}

/* Encode a fixture and decode it back. Returns the encoded size, 0 on error. */
static size_t rt(relais_msg_type type, const relais_key *k, relais_msg *back) {
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t n = fx_encode(type, k, buf, sizeof buf);
  if (n == 0) return 0;
  relais_reject why = RELAIS_REJ_NONE;
  if (!relais_frame_decode(buf, n, k, back, &why)) return 0;
  return n;
}

/* 6.1/6.2: one round trip per type, compared byte for byte on the structure. */
void test_roundtrip_every_type_all_fields(void) {
  t_section("round trip, every type, every field");
  relais_key k;
  fill_key(&k);
  for (size_t i = 0; i < TYPE_COUNT; i++) {
    relais_msg sent, back;
    const relais_key *keys[2] = {NULL, &k};
    for (size_t j = 0; j < 2; j++) {
      fx_fill(&sent, ALL_TYPES[i]);
      size_t n = rt(ALL_TYPES[i], keys[j], &back);
      T_CHECK(n > 0);
      /* Byte-for-byte equality of the whole message: decoding zeroes it first,
       * so the comparison also catches a field the codec forgot. */
      T_CHECK(memcmp(&sent, &back, sizeof sent) == 0);
      T_EQ_U(back.type, sent.type);
      T_EQ_U(back.sender, sent.sender);
      T_EQ_U(back.flags, sent.flags);
      T_EQ_U(back.tick, sent.tick);
      T_EQ_U(back.mandate, sent.mandate);
    }
  }
}

/* The payload of each type must be exactly reproduced, field by field. */
void test_roundtrip_payload_fields(void) {
  t_section("round trip, payload fields of each type");
  relais_msg m, b;

  fx_fill(&m, RELAIS_MSG_STATUT);
  T_CHECK(rt(RELAIS_MSG_STATUT, NULL, &b) > 0);
  T_EQ_U(b.holder, RELAIS_NODE_B);
  T_EQ_U(b.source, RELAIS_SRC_TESTAMENT);
  T_EQ_U(b.misses, 2u);
  T_CHECK(b.certified);
  T_EQ_U(b.certified_node, RELAIS_NODE_A);
  T_EQ_U(b.lamp, RELAIS_LAMP_YELLOW);
  T_EQ_U(b.cause, RELAIS_CAUSE_LINK_LOST);
  T_CHECK(b.stop_requested);
  /* The four measurements, bit for bit. */
  T_CHECK(memcmp(&b.meas, &m.meas, sizeof m.meas) == 0);

  fx_fill(&m, RELAIS_MSG_ORDRE);
  T_CHECK(rt(RELAIS_MSG_ORDRE, NULL, &b) > 0);
  T_EQ_U(b.value, m.value);
  T_EQ_U(b.testament_len, RELAIS_TESTAMENT_MAX);
  T_CHECK(memcmp(b.testament, m.testament, sizeof m.testament) == 0);
  T_CHECK(b.has_order); /* an order always carries a value */

  fx_fill(&m, RELAIS_MSG_DEMANDE);
  T_CHECK(rt(RELAIS_MSG_DEMANDE, NULL, &b) > 0);
  T_EQ_U(b.seen_mandate, m.seen_mandate);
  T_EQ_U(b.reason, m.reason);
  T_CHECK(b.has_order);
  T_EQ_U(b.value, m.value);
  T_CHECK(memcmp(b.testament, m.testament, sizeof m.testament) == 0);

  fx_fill(&m, RELAIS_MSG_OMBRE);
  T_CHECK(rt(RELAIS_MSG_OMBRE, NULL, &b) > 0);
  T_EQ_U(b.value, m.value);

  fx_fill(&m, RELAIS_MSG_CESSION);
  T_CHECK(rt(RELAIS_MSG_CESSION, NULL, &b) > 0);
  T_EQ_U(b.reason, m.reason);

  fx_fill(&m, RELAIS_MSG_SUIVI);
  T_CHECK(rt(RELAIS_MSG_SUIVI, NULL, &b) > 0);
  T_CHECK(b.ready);
}

/* REP-01/6.2: the replicated state travels whole, up to the 255-byte ceiling
 * that the one-byte length field allows. */
void test_roundtrip_ctl_state(void) {
  t_section("round trip, 255-byte control state (ETAT)");
  relais_msg m, b;
  uint8_t buf[RELAIS_FRAME_MAX];
  fx_fill(&m, RELAIS_MSG_ETAT);
  T_EQ_U(m.ctl_state_len, 255u);
  size_t n = relais_frame_encode(&m, NULL, buf, sizeof buf);
  T_CHECK(n > 0);
  T_CHECK(n <= RELAIS_FRAME_MAX);
  /* payload = 4 (last_value) + 1 (len) + 255 = 260; total 292. */
  T_EQ_U(n, 292u);
  T_EQ_U(relais_frame_encoded_size(&m), n);
  T_CHECK(relais_frame_decode(buf, n, NULL, &b, NULL));
  T_EQ_U(b.ctl_state_len, m.ctl_state_len);
  T_CHECK(memcmp(b.ctl_state, m.ctl_state, m.ctl_state_len) == 0);

  /* The state must be whole: one byte short of the length is malformed. */
  relais_reject why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n - 1, NULL, &b, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
}

/* 6.1: fixed size per type, never above 512 bytes, and the size the encoder
 * announces is the size it writes. */
void test_encoded_sizes(void) {
  t_section("encoded size per type, at most 512 bytes");
  uint8_t buf[RELAIS_FRAME_MAX];
  const size_t want[] = {56, 69, 37, 75, 33, 292, 34};
  for (size_t i = 0; i < TYPE_COUNT; i++) {
    relais_msg m;
    fx_fill(&m, ALL_TYPES[i]);
    size_t size = relais_frame_encoded_size(&m);
    size_t n = relais_frame_encode(&m, NULL, buf, sizeof buf);
    T_EQ_U(size, want[i]);
    T_EQ_U(n, size);
    T_CHECK(size <= RELAIS_FRAME_MAX);
    /* One byte less than announced: the encoder refuses rather than truncate. */
    T_EQ_U(relais_frame_encode(&m, NULL, buf, size - 1), 0u);
  }
  /* A message of a type that has no wire form has no size and no encoding. */
  relais_msg m;
  fx_fill(&m, (relais_msg_type)0);
  T_EQ_U(relais_frame_encoded_size(&m), 0u);
  T_EQ_U(relais_frame_encode(&m, NULL, buf, sizeof buf), 0u);
  fx_fill(&m, (relais_msg_type)(RELAIS_MSG_SUIVI + 1));
  T_EQ_U(relais_frame_encoded_size(&m), 0u);
  /* A NULL message has no size either. */
  T_EQ_U(relais_frame_encoded_size(NULL), 0u);
}

/* SEC-03: the tick label travels with the frame; it is what makes a replay of
 * a captured frame stale. */
void test_sec03_tick_survives_roundtrip(void) {
  t_section("SEC-03 tick label survives the round trip");
  relais_msg sent, back;
  fx_fill(&sent, RELAIS_MSG_ORDRE);
  sent.tick = 0xFEDCBA9876543210ull;
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t n = relais_frame_encode(&sent, NULL, buf, sizeof buf);
  T_CHECK(n > 0);
  T_CHECK(relais_frame_decode(buf, n, NULL, &back, NULL));
  T_EQ_U(back.tick, 0xFEDCBA9876543210ull);
  /* Replayed later, the same frame no longer names the tick being applied. */
  T_CHECK(back.tick != back.tick + 1u);
  /* Extreme labels are not flattened by the codec. */
  sent.tick = RELAIS_TICK_NONE;
  n = relais_frame_encode(&sent, NULL, buf, sizeof buf);
  T_CHECK(relais_frame_decode(buf, n, NULL, &back, NULL));
  T_EQ_U(back.tick, RELAIS_TICK_NONE);
}

/* ROB-02: a duplicated frame has no effect on the command: it decodes to the
 * very same message, so the arbiter takes the same decision twice. */
void test_rob02_duplicate_decodes_identically(void) {
  t_section("ROB-02 duplicate frame decodes identically, deterministically");
  relais_key k;
  fill_key(&k);
  uint8_t buf[RELAIS_FRAME_MAX];
  relais_msg a, b, c;
  size_t n = fx_encode(RELAIS_MSG_DEMANDE, &k, buf, sizeof buf);
  T_CHECK(n > 0);
  T_CHECK(relais_frame_decode(buf, n, &k, &a, NULL));
  T_CHECK(relais_frame_decode(buf, n, &k, &b, NULL));
  T_CHECK(relais_frame_decode(buf, n, &k, &c, NULL));
  T_CHECK(memcmp(&a, &b, sizeof a) == 0);
  T_CHECK(memcmp(&a, &c, sizeof a) == 0);
  /* The decoder is deterministic on an unchecked pass too (ROB-09). */
  relais_msg d, e;
  T_CHECK(relais_frame_decode_unchecked(buf, n, &d));
  T_CHECK(relais_frame_decode_unchecked(buf, n, &e));
  T_CHECK(memcmp(&d, &e, sizeof d) == 0);
  T_CHECK(memcmp(&a, &d, sizeof a) == 0);
}

/* Build a valid STATUT frame for the rejection tests. */
static size_t base_frame(uint8_t *buf, size_t cap, const relais_key *k) {
  return fx_encode(RELAIS_MSG_STATUT, k, buf, cap);
}

/* Recompute the trailer after a deliberate mutation, so the test reaches the
 * parser defect it targets instead of stopping on the control (SEC-01). */
static void seal(uint8_t *buf, size_t n, const relais_key *k) {
  uint64_t check = relais_check_compute(k, buf, n - RELAIS_CHECK_LEN);
  for (size_t i = 0; i < RELAIS_CHECK_LEN; i++)
    buf[n - RELAIS_CHECK_LEN + i] = (uint8_t)(check >> ((RELAIS_CHECK_LEN - 1u - i) * 8u));
}

/* SEC-01: each defect is named with the code the caller counts (DET-04). */
void test_sec01_rejections_with_code(void) {
  t_section("SEC-01 malformed frames rejected with the expected code");
  uint8_t buf[RELAIS_FRAME_MAX];
  relais_msg out;
  relais_reject why = RELAIS_REJ_NONE;

  /* magic */
  size_t n = base_frame(buf, sizeof buf, NULL);
  uint8_t save = buf[0];
  buf[0] = (uint8_t)(save ^ 0xffu);
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
  buf[0] = save;

  /* version */
  buf[4] = (uint8_t)(RELAIS_VERSION + 1u);
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_VERSION);
  buf[4] = RELAIS_VERSION;

  /* type, both ends of the enumeration */
  buf[5] = 0;
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
  buf[5] = (uint8_t)(RELAIS_MSG_SUIVI + 1u);
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
  buf[5] = (uint8_t)RELAIS_MSG_STATUT;

  /* sender: none is not a sender, and neither is an unknown number */
  buf[6] = (uint8_t)RELAIS_NODE_NONE;
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
  buf[6] = 9;
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
  buf[6] = (uint8_t)RELAIS_NODE_ACTUATOR;

  /* payload_len inconsistent with the bytes actually received */
  buf[20] = (uint8_t)(buf[20] + 1u);
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
  buf[20] = (uint8_t)(buf[20] - 1u);
  T_CHECK(relais_frame_decode(buf, n, NULL, &out, NULL)); /* restored */

  /* truncated by one byte, and truncated below the fixed minimum */
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n - 1, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, RELAIS_HEADER_LEN + RELAIS_CHECK_LEN - 1u, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
}

/* DET-04: a corrupted control is reported as such, so the frame is counted as
 * lost rather than malformed. */
void test_sec01_bad_control_rejected(void) {
  t_section("DET-04 bad control rejected with RELAIS_REJ_CHECK");
  uint8_t buf[RELAIS_FRAME_MAX];
  relais_key k;
  fill_key(&k);
  relais_msg out;
  const relais_key *keys[2] = {NULL, &k};
  for (size_t j = 0; j < 2; j++) {
    size_t n = base_frame(buf, sizeof buf, keys[j]);
    T_CHECK(n > 0);
    buf[n - 1] = (uint8_t)(buf[n - 1] ^ 0x01u); /* last byte of the trailer */
    relais_reject why = RELAIS_REJ_NONE;
    T_CHECK(!relais_frame_decode(buf, n, keys[j], &out, &why));
    T_EQ_U(why, RELAIS_REJ_CHECK);
    /* The same bytes pass without the check: the parser is not the guard. */
    T_CHECK(relais_frame_decode_unchecked(buf, n, &out));
  }
}

/* A frame of a given type whose payload does not fit that type is malformed,
 * even when its control is perfectly valid: the size of the payload is part of
 * the type, so a relabelled frame is refused before it is interpreted. */
void test_sec01_payload_len_mismatch_per_type(void) {
  t_section("SEC-01 payload length must match the type");
  uint8_t buf[RELAIS_FRAME_MAX];
  relais_msg out;
  relais_reject why = RELAIS_REJ_NONE;

  /* A CESSION (1-byte payload) relabelled as a STATUT (24 bytes). */
  size_t n = fx_encode(RELAIS_MSG_CESSION, NULL, buf, sizeof buf);
  T_CHECK(n > 0);
  buf[5] = (uint8_t)RELAIS_MSG_STATUT;
  seal(buf, n, NULL);
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);
  /* With a valid control, the size rule is the only guard left: the unchecked
   * parser does read this frame, which is why the checked one must refuse it
   * on the type/size rule before anything is interpreted (ROB-09). */
  T_CHECK(!relais_frame_decode_unchecked(buf, n, &out));

  /* The other way round: a STATUT relabelled as an OMBRE. */
  n = base_frame(buf, sizeof buf, NULL);
  T_CHECK(n > 0);
  buf[5] = (uint8_t)RELAIS_MSG_OMBRE;
  seal(buf, n, NULL);
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);

  /* An ETAT whose inner length disagrees with the payload is malformed too. */
  n = fx_encode(RELAIS_MSG_ETAT, NULL, buf, sizeof buf);
  T_CHECK(n > 0);
  buf[RELAIS_HEADER_LEN + 4u] = 3; /* ctl_state_len announced 255, now 3 */
  seal(buf, n, NULL);
  why = RELAIS_REJ_NONE;
  T_CHECK(!relais_frame_decode(buf, n, NULL, &out, &why));
  T_EQ_U(why, RELAIS_REJ_MALFORMED);

}

int main(void) {
  test_roundtrip_every_type_all_fields();
  test_roundtrip_payload_fields();
  test_roundtrip_ctl_state();
  test_encoded_sizes();
  test_sec03_tick_survives_roundtrip();
  test_rob02_duplicate_decodes_identically();
  test_sec01_rejections_with_code();
  test_sec01_bad_control_rejected();
  test_sec01_payload_len_mismatch_per_type();
  return t_summary();
}
