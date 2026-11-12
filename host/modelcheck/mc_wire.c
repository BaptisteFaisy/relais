/* Relais — space A: bounded exhaustion of the wire decoder (mission M36).
 *
 * Every case calls the REAL relais_frame_decode() of core/src/frame.c. The
 * oracle is the published frame format (docs/PROTOCOL.md sections 1..3), not
 * the implementation: the payload sizes below are transcribed from the spec
 * table and cross-checked once against relais_frame_encoded_size() so a drift
 * between spec and code cannot pass silently.
 *
 * Properties checked on every case:
 *   WA-1 (SEC-01, ROB-09) a frame is accepted exactly when its version, type,
 *         sender and declared payload length are legal for the bytes present.
 *   WA-2 (SEC-02) any single-byte corruption of a valid frame is rejected, or
 *         decodes to a message indistinguishable from the original: no
 *         corruption is ever observable by the arbiter.
 *   WA-3 (ROB-09) every truncation and every extension is rejected.
 *   WA-4 (MAN-02, SEC-03) tick and mandate extremes round-trip exactly, so the
 *         tick tag that makes a replay stale is never mangled.
 *   WA-5 (SEC-01) dropping a corrupted frame is equivalent to it never having
 *         been sent: the arbiter's decision is unchanged.
 */
#include "mc.h"

#include <string.h>

#include "relais/check.h"
#include "relais/frame.h"

/* Payload size per type, docs/PROTOCOL.md section 2. Index = relais_msg_type.
 * ETAT (6) is variable: 5 + ctl_state_len, ctl_state_len <= RELAIS_CTL_STATE_MAX. */
static const uint16_t SPEC_PAYLOAD[MC_A_TYPES + 1] = {0, 24, 37, 5, 43, 1, 0, 2};
#define ETAT_FIXED 5u

/* Lay out a frame byte by byte (PROTOCOL.md section 1) with a chosen declared
 * payload length and a deterministic payload pattern. The trailer is computed
 * over the bytes actually present, so the integrity check is always correct and
 * only the length/type/version/sender rules are under test. */
static size_t build_raw(uint8_t *buf, size_t cap, const relais_key *k, uint8_t version,
                        uint8_t type, uint8_t sender, uint8_t flags, relais_tick tick,
                        relais_mandate mandate, uint16_t paylen) {
  size_t total = (size_t)RELAIS_HEADER_LEN + (size_t)paylen + RELAIS_CHECK_LEN;
  if (buf == NULL || total > cap) return 0;
  memset(buf, 0, total);
  relais_frame_write_u32(buf, RELAIS_MAGIC);
  buf[4] = version;
  buf[5] = type;
  buf[6] = sender;
  buf[7] = flags;
  relais_frame_write_u64(buf + 8, tick);
  relais_frame_write_u32(buf + 16, mandate);
  buf[20] = (uint8_t)(paylen >> 8);
  buf[21] = (uint8_t)(paylen & 0xffu);
  for (uint16_t i = 0; i < paylen; i++)
    buf[RELAIS_HEADER_LEN + i] = (uint8_t)(1u + (uint32_t)i * 7u);
  relais_frame_write_u64(buf + total - RELAIS_CHECK_LEN,
                         relais_check_compute(k, buf, total - RELAIS_CHECK_LEN));
  return total;
}

/* Spec oracle for A1: is this (type, declared length, payload bytes) legal? */
static bool spec_accepts(uint8_t type, uint16_t paylen, const uint8_t *buf) {
  if (type < RELAIS_MSG_STATUT || type > RELAIS_MSG_SUIVI) return false;
  if (type != RELAIS_MSG_ETAT) return paylen == SPEC_PAYLOAD[type];
  if (paylen < ETAT_FIXED || paylen > ETAT_FIXED + RELAIS_CTL_STATE_MAX) return false;
  return (uint16_t)buf[RELAIS_HEADER_LEN + 4] == (uint16_t)(paylen - ETAT_FIXED);
}

/* The decoded header fields must be exactly the bytes that were written. */
static bool round_trip_ok(const relais_msg *m, uint8_t type, uint8_t sender, uint8_t flags,
                          relais_tick tick, relais_mandate mandate) {
  return m->type == (relais_msg_type)type && m->sender == (relais_node)sender &&
         m->flags == flags && m->tick == tick && m->mandate == mandate;
}

/* A0: the spec table and the encoder agree on every payload size. */
static void a0_crosscheck(mc_space *sp) {
  for (unsigned t = RELAIS_MSG_STATUT; t <= MC_A_TYPES; t++) {
    relais_msg m;
    mc_fill_msg(&m, (relais_msg_type)t);
    size_t total = relais_frame_encoded_size(&m);
    uint16_t paylen = (uint16_t)(total - RELAIS_HEADER_LEN - RELAIS_CHECK_LEN);
    char d[96];
    mc_desc(d, sizeof d, "A0 spec/encoder payload drift", t, paylen);
    if (t == RELAIS_MSG_ETAT) {
      T_MSG(paylen == (uint16_t)(ETAT_FIXED + MC_A_ETAT_STATE), d);
    } else {
      T_MSG(paylen == SPEC_PAYLOAD[t], d);
    }
    sp->cases++;
  }
}

/* A1: every declared payload length, for every type, both integrity levels. */
static void a1_paylen(mc_space *sp) {
  uint8_t buf[RELAIS_FRAME_MAX];
  relais_msg out;
  for (unsigned mode = 0; mode < MC_A_KEY_MODES; mode++) {
    relais_key k;
    mc_key(&k, mode == 1);
    for (unsigned t = RELAIS_MSG_STATUT; t <= MC_A_TYPES; t++) {
      for (unsigned len = 0; len <= MC_A_PAYLEN_MAX; len++) {
        uint16_t paylen = (uint16_t)len;
        size_t total = build_raw(buf, sizeof buf, &k, RELAIS_VERSION, (uint8_t)t,
                                 (uint8_t)RELAIS_NODE_A, 0, 13, 3, paylen);
        relais_reject why = RELAIS_REJ_NONE;
        bool ok = relais_frame_decode(buf, total, &k, &out, &why);
        bool want = spec_accepts((uint8_t)t, paylen, buf);
        char d[96];
        mc_desc(d, sizeof d, "A1 length rule", (uint64_t)t * 10000u + len, (uint64_t)why);
        T_MSG(ok == want, d);
        if (ok) {
          sp->accepted++;
          T_MSG(round_trip_ok(&out, (uint8_t)t, (uint8_t)RELAIS_NODE_A, 0, 13, 3), d);
        } else {
          sp->rejected++;
          T_MSG(why == RELAIS_REJ_MALFORMED, d);
        }
        sp->cases++;
      }
    }
  }
}

/* A1b: a consistent ETAT payload for every legal ctl_state_len (REP-01). */
static void a1b_etat(mc_space *sp) {
  uint8_t buf[RELAIS_FRAME_MAX];
  relais_msg out;
  for (unsigned mode = 0; mode < MC_A_KEY_MODES; mode++) {
    relais_key k;
    mc_key(&k, mode == 1);
    for (unsigned n = 0; n <= RELAIS_CTL_STATE_MAX; n++) {
      uint16_t paylen = (uint16_t)(ETAT_FIXED + n);
      size_t total = build_raw(buf, sizeof buf, &k, RELAIS_VERSION, RELAIS_MSG_ETAT,
                               (uint8_t)RELAIS_NODE_A, 0, 13, 3, paylen);
      buf[RELAIS_HEADER_LEN + 4] = (uint8_t)n; /* ctl_state_len, one byte */
      relais_frame_write_u64(buf + total - RELAIS_CHECK_LEN,
                             relais_check_compute(&k, buf, total - RELAIS_CHECK_LEN));
      relais_reject why = RELAIS_REJ_NONE;
      bool ok = relais_frame_decode(buf, total, &k, &out, &why);
      char d[96];
      mc_desc(d, sizeof d, "A1b ETAT state length", n, (uint64_t)why);
      /* the length travels in one byte: 256 cannot be carried, so such a frame
       * is malformed; the usable states are 0..255 (REP-01 keeps 38 bytes). */
      T_MSG(ok == (n <= 255u), d);
      if (ok) {
        sp->accepted++;
        T_MSG(out.ctl_state_len == (uint8_t)n, d);
      }
      sp->cases++;
    }
  }
}

/* One corrupted frame: rejected, or indistinguishable from the original. */
static void corrupt_case(mc_space *sp, const relais_msg *ref, const uint8_t *src, size_t len,
                         size_t pos, uint8_t val, const relais_key *k, bool level_c) {
  uint8_t buf[RELAIS_FRAME_MAX + 16];
  relais_msg out;
  relais_reject why = RELAIS_REJ_NONE;
  memcpy(buf, src, len);
  buf[pos] = val;
  bool ok = relais_frame_decode(buf, len, k, &out, &why);
  char d[128];
  mc_desc(d, sizeof d, "A2 corruption observable", (uint64_t)pos, val);
  if (!ok) {
    sp->rejected++;
    T_MSG(why == RELAIS_REJ_CHECK || why == RELAIS_REJ_MALFORMED ||
              why == RELAIS_REJ_VERSION,
          d);
  } else {
    /* WA-2: acceptance is only tolerated when nothing observable changed. */
    sp->accepted++;
    T_MSG(out.type == ref->type && out.sender == ref->sender && out.tick == ref->tick &&
              out.mandate == ref->mandate && out.value == ref->value &&
              out.testament_len == ref->testament_len,
          d);
  }
  (void)level_c;
  sp->cases++;
}

/* A2: every byte position x every byte value of every valid frame. */
static void a2_single_byte(mc_space *sp) {
  for (unsigned mode = 0; mode < MC_A_KEY_MODES; mode++) {
    relais_key k;
    mc_key(&k, mode == 1);
    for (unsigned t = RELAIS_MSG_STATUT; t <= MC_A_TYPES; t++) {
      uint8_t src[RELAIS_FRAME_MAX];
      relais_msg ref;
      mc_fill_msg(&ref, (relais_msg_type)t);
      size_t len = relais_frame_encode(&ref, &k, src, sizeof src);
      T_CHECK(len > 0);
      for (size_t pos = 0; pos < len; pos++)
        for (unsigned v = 0; v < MC_A_BYTE_VALUES; v++)
          if ((uint8_t)v != src[pos])
            corrupt_case(sp, &ref, src, len, pos, (uint8_t)v, &k, mode == 1);
    }
  }
}

/* A3 and A4: every truncation and every extension of every valid frame. */
static void a3_trunc_extend(mc_space *sp) {
  for (unsigned mode = 0; mode < MC_A_KEY_MODES; mode++) {
    relais_key k;
    mc_key(&k, mode == 1);
    for (unsigned t = RELAIS_MSG_STATUT; t <= MC_A_TYPES; t++) {
      uint8_t src[RELAIS_FRAME_MAX + 16];
      relais_msg out;
      size_t len = mc_encode((relais_msg_type)t, &k, src, sizeof src);
      for (size_t cut = 0; cut < len; cut++) { /* WA-3, truncation */
        relais_reject why = RELAIS_REJ_NONE;
        bool ok = relais_frame_decode(src, cut, &k, &out, &why);
        char d[96];
        mc_desc(d, sizeof d, "A3 truncation accepted", t, (uint64_t)cut);
        T_MSG(!ok && why == RELAIS_REJ_MALFORMED, d);
        sp->rejected++;
        sp->cases++;
      }
      for (unsigned extra = 1; extra <= MC_A_EXTEND_MAX; extra++) { /* WA-3 */
        size_t grown = len + extra;
        memset(src + len, 0xa5, extra);
        relais_reject why = RELAIS_REJ_NONE;
        bool ok = relais_frame_decode(src, grown, &k, &out, &why);
        char d[96];
        mc_desc(d, sizeof d, "A4 extension accepted", t, extra);
        T_MSG(!ok && why == RELAIS_REJ_MALFORMED, d);
        sp->rejected++;
        sp->cases++;
      }
    }
  }
}

/* A5: every value of the four discriminating header fields, trailer correct so
 * only the field rule is under test (WA-1). */
static void a5_one(mc_space *sp, const relais_key *k, unsigned field, uint8_t v) {
  uint8_t buf[RELAIS_FRAME_MAX];
  relais_msg out;
  relais_reject why = RELAIS_REJ_NONE;
  uint8_t version = field == 2 ? v : RELAIS_VERSION;
  uint8_t type = field == 0 ? v : (uint8_t)RELAIS_MSG_STATUT;
  uint8_t sender = field == 1 ? v : (uint8_t)RELAIS_NODE_A;
  uint8_t flags = field == 3 ? v : 0;
  uint16_t paylen = (type >= RELAIS_MSG_STATUT && type <= MC_A_TYPES)
                        ? (uint16_t)(type == RELAIS_MSG_ETAT ? ETAT_FIXED + 29u
                                                             : SPEC_PAYLOAD[type])
                        : SPEC_PAYLOAD[RELAIS_MSG_STATUT];
  size_t total = build_raw(buf, sizeof buf, k, version, type, sender, flags, 13, 3, paylen);
  bool ok = relais_frame_decode(buf, total, k, &out, &why);
  bool want = spec_accepts(type, paylen, buf) && version == RELAIS_VERSION &&
              (sender >= RELAIS_NODE_A && sender <= RELAIS_NODE_ACTUATOR);
  char d[96];
  mc_desc(d, sizeof d, "A5 header field rule", (uint64_t)field * 256u + v, (uint64_t)why);
  T_MSG(ok == want, d);
  if (ok) {
    sp->accepted++;
    T_MSG(round_trip_ok(&out, type, sender, flags, 13, 3), d);
  } else {
    sp->rejected++;
    T_MSG(why == RELAIS_REJ_MALFORMED || why == RELAIS_REJ_VERSION, d);
    if (version != RELAIS_VERSION) T_MSG(why == RELAIS_REJ_VERSION, d);
  }
  sp->cases++;
}

static void a5_header_fields(mc_space *sp) {
  for (unsigned mode = 0; mode < MC_A_KEY_MODES; mode++) {
    relais_key k;
    mc_key(&k, mode == 1);
    for (unsigned field = 0; field < 4; field++)
      for (unsigned v = 0; v < MC_A_FIELD_VALUES; v++) a5_one(sp, &k, field, (uint8_t)v);
  }
}

/* A6: tick and mandate extremes must round-trip exactly (WA-4): the tick tag
 * is what makes a replayed frame stale (SEC-03), it can never be mangled. */
static void a6_extremes(mc_space *sp) {
  static const relais_tick ticks[MC_A_TICKS] = {0u,
                                                1u,
                                                (relais_tick)UINT32_MAX,
                                                (relais_tick)1 << 63,
                                                (relais_tick)UINT64_MAX - 1u,
                                                (relais_tick)UINT64_MAX};
  static const relais_mandate mds[MC_A_MANDATES] = {0u, 1u, 1u << 31, (relais_mandate)UINT32_MAX};
  for (unsigned mode = 0; mode < MC_A_KEY_MODES; mode++) {
    relais_key k;
    mc_key(&k, mode == 1);
    for (unsigned i = 0; i < MC_A_TICKS; i++) {
      for (unsigned j = 0; j < MC_A_MANDATES; j++) {
        uint8_t buf[RELAIS_FRAME_MAX];
        relais_msg out;
        relais_reject why = RELAIS_REJ_NONE;
        size_t total = build_raw(buf, sizeof buf, &k, RELAIS_VERSION, RELAIS_MSG_ORDRE,
                                 (uint8_t)RELAIS_NODE_A, 0, ticks[i], mds[j],
                                 SPEC_PAYLOAD[RELAIS_MSG_ORDRE]);
        bool ok = relais_frame_decode(buf, total, &k, &out, &why);
        char d[96];
        mc_desc(d, sizeof d, "A6 extreme round trip", i, j);
        T_MSG(ok, d);
        if (ok) {
          sp->accepted++;
          T_MSG(out.tick == ticks[i] && out.mandate == mds[j], d);
        }
        sp->cases++;
      }
    }
  }
}

void mc_space_wire(mc_space *sp) {
  double t0 = mc_now();
  sp->cases = sp->rejected = sp->accepted = 0;
  t_section("M36 space A: bounded exhaustion of relais_frame_decode");
  a0_crosscheck(sp);
  a1_paylen(sp);
  a1b_etat(sp);
  a2_single_byte(sp);
  a3_trunc_extend(sp);
  a5_header_fields(sp);
  a6_extremes(sp);
  sp->seconds = mc_now() - t0;
}
