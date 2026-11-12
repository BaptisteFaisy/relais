/* Relais — fine hardware fault unit tests (M18).
 *
 * Four claims, each turned into a machine-checkable property:
 *   SEC-04  a bit flipped in the raw mandate never applies an order;
 *   DET-09  a bit flipped in a computed value is unmasked by the order/shadow
 *           comparison at level C;
 *   SEC-02  a corrupted frame with a recomputed CRC is accepted by the CRC at
 *           level S and rejected by the keyed MAC at level C (measured over
 *           10 000 corruptions);
 *   ROB-02/OBS-02/MAN-04  resource exhaustion leads to a loud, safe state, never to a
 *           double command.
 */
#include "test_util.h"

#include "bitflip.h"
#include "corrupt.h"
#include "resource.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* 16-byte link key for the level C tests (SEC-02). */
static const char *MAC_HEX = "0123456789abcdef0123456789abcdef";

static void key_mac(relais_key *k) { relais_key_from_hex(k, MAC_HEX); }

static bool event_seen(const relais_arb_out *o, relais_event_kind kind) {
  for (uint8_t i = 0; i < o->event_count; i++)
    if (o->events[i].kind == kind) return true;
  return false;
}

static uint32_t applied_total(const relais_arb *a) {
  return a->applied_by_source[RELAIS_SRC_HOLDER] +
         a->applied_by_source[RELAIS_SRC_SHADOW] +
         a->applied_by_source[RELAIS_SRC_TESTAMENT] +
         a->applied_by_source[RELAIS_SRC_FALLBACK];
}

/* The three grains and the four zones behave, deterministically. */
static void test_flip_primitives(void) {
  t_section("bitflip primitives");
  relais_params p;
  relais_params_default(&p);
  fi_rng rng;
  fi_rng_seed(&rng, 7);
  fi_report rep;

  relais_ctl_state c;
  relais_ctl_init(&c, &p);
  c.last_value = 0.0f;
  T_EQ_U(fi_flip_ctl(&c, FI_ZONE_VALUE, FI_MODE_BIT, 0, &rng, &rep), 1u);
  T_CHECK(rep.ok && rep.zone == FI_ZONE_VALUE);
  T_CHECK(rep.mask == 0x01u && rep.before == 0u && rep.after == 0x01u);

  relais_node_sm s;
  memset(&s, 0, sizeof s);
  T_EQ_U(fi_flip_node(&s, FI_ZONE_MANDATE, FI_MODE_WORD, FI_RANDOM, &rng, &rep), 4u);
  T_CHECK(rep.ok && rep.zone == FI_ZONE_MANDATE);
  T_CHECK(rep.before == 0u && rep.after == 0xFFu);

  /* A zone with no region in the target refuses the flip. */
  T_EQ_U(fi_flip_ctl(&c, FI_ZONE_MANDATE, FI_MODE_BIT, 0, &rng, &rep), 0u);
  T_CHECK(!rep.ok);
}

/* SEC-04: a raw mandate bit flip is dropped by both the CRC and the MAC, and a
 * frame that does carry a wrong mandate is never applied as an order. */
static void test_sec04_mandate_flip(void) {
  t_section("SEC-04 mandate bit flip");
  relais_params p;
  relais_params_default(&p);
  relais_key kcrc, kmac;
  relais_key_clear(&kcrc);
  key_mac(&kmac);

  relais_msg m;
  t_order(&m, RELAIS_NODE_A, 1u, 10u, 0.5f, &p);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(&m, &kmac, buf, sizeof buf);
  T_CHECK(len > 0);

  fi_rng rng;
  fi_rng_seed(&rng, 1);
  fi_report rep;
  T_EQ_U(fi_flip_frame(buf, len, FI_ZONE_MANDATE, FI_MODE_BIT, 0, &rng, &rep), 1u);
  T_CHECK(rep.ok && rep.zone == FI_ZONE_MANDATE);

  relais_msg out;
  relais_reject why;
  T_CHECK(!relais_frame_decode(buf, len, &kmac, &out, &why));
  T_CHECK(why == RELAIS_REJ_CHECK);
  T_CHECK(!relais_frame_decode(buf, len, &kcrc, &out, &why));
  T_CHECK(why == RELAIS_REJ_CHECK);

  /* Even fed to the actuator after a memory row fault (unchecked decode), the
   * wrong mandate is refused: no order is applied. */
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_arb_out o;
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  relais_msg d;
  t_demande(&d, RELAIS_NODE_A, 0, 1, true, 0.5f, &p);
  relais_arb_step(&a, 1, &d, 1, &meas, &o);
  T_CHECK(o.source == RELAIS_SRC_HOLDER);

  relais_msg bad;
  t_order(&bad, RELAIS_NODE_A, a.mandate + 1u, 2u, 0.5f, &p);
  relais_arb_step(&a, 2, &bad, 1, &meas, &o);
  T_CHECK(o.source != RELAIS_SRC_HOLDER);
  T_EQ_U(a.rejected[RELAIS_REJ_MANDATE_FUTURE], 1u);
}

/* DET-09: once the second is certified, a value that diverges from the order
 * is reported as a disagreement after three ticks. */
static void test_det09_value_disagreement(void) {
  t_section("DET-09 value flip detected");
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_arb_out o;
  relais_meas meas;
  memset(&meas, 0, sizeof meas);

  relais_msg d;
  t_demande(&d, RELAIS_NODE_A, 0, 1, true, 0.5f, &p);
  relais_arb_step(&a, 1, &d, 1, &meas, &o);

  relais_msg msgs[2];
  for (relais_tick t = 2; t <= 4; t++) {
    t_order(&msgs[0], RELAIS_NODE_A, a.mandate, t, 0.5f, &p);
    t_shadow(&msgs[1], RELAIS_NODE_B, t, 0.5f, 0);
    relais_arb_step(&a, t, msgs, 2, &meas, &o);
  }
  T_CHECK(a.certified && a.certified_node == RELAIS_NODE_B);

  relais_tick disagreement_at = 0;
  for (relais_tick t = 5; t <= 7; t++) {
    t_order(&msgs[0], RELAIS_NODE_A, a.mandate, t, 0.5f, &p);
    t_shadow(&msgs[1], RELAIS_NODE_B, t, 0.9f, 0);
    relais_arb_step(&a, t, msgs, 2, &meas, &o);
    if (event_seen(&o, RELAIS_EV_DISAGREEMENT)) disagreement_at = t;
  }
  T_CHECK(a.disagreement);
  T_EQ_U(disagreement_at, 7u);
}

/* SEC-02: 10 000 corruptions, each re-signed with a CRC. The CRC accepts them
 * all at level S; the keyed MAC accepts none at level C. */
static void test_crc_vs_mac(void) {
  t_section("CRC vs MAC on 10000 corruptions");
  relais_params p;
  relais_params_default(&p);
  relais_key kcrc, kmac;
  relais_key_clear(&kcrc);
  key_mac(&kmac);
  relais_msg m;
  t_order(&m, RELAIS_NODE_A, 1u, 10u, -0.25f, &p);

  fi_rng rng;
  fi_rng_seed(&rng, 0x1234u);
  uint32_t crc_ok = 0, mac_ok = 0;
  for (uint32_t i = 0; i < 10000; i++) {
    uint8_t buf[RELAIS_FRAME_MAX];
    size_t len = relais_frame_encode(&m, &kmac, buf, sizeof buf);
    T_CHECK(len > 0);
    fi_report rep;
    T_CHECK(fi_corrupt_frame(buf, len, &kcrc, FI_ZONE_VALUE, FI_MODE_BIT, &rng, &rep) > 0);
    relais_msg out;
    relais_reject why;
    if (relais_frame_decode(buf, len, &kcrc, &out, &why)) crc_ok++;
    if (relais_frame_decode(buf, len, &kmac, &out, &why)) mac_ok++;
  }
  printf("CRC accepted %u/10000, MAC accepted %u/10000\n", crc_ok, mac_ok);
  T_EQ_U(crc_ok, 10000u);
  T_EQ_U(mac_ok, 0u);
}

/* ROB-02: a full queue refuses and counts; it never grows past its capacity. */
static void test_queue_full(void) {
  relais_msg buf[4];
  fi_queue q;
  fi_queue_init(&q, buf, 4);
  relais_msg m;
  relais_msg_clear(&m);
  m.type = RELAIS_MSG_STATUT;
  m.sender = RELAIS_NODE_ACTUATOR;
  m.tick = 1;
  for (uint32_t i = 0; i < 4; i++) T_CHECK(fi_queue_push(&q, &m));
  T_EQ_U(fi_queue_count(&q), 4u);
  T_CHECK(!fi_queue_push(&q, &m));
  T_EQ_U(fi_queue_dropped(&q), 1u);
  T_EQ_U(fi_queue_count(&q), 4u);
  T_CHECK(fi_queue_pop(&q, NULL));
  T_EQ_U(fi_queue_count(&q), 3u);
}

/* OBS-02: a saturated journal buffer refuses the line and counts the overflow,
 * so there is never an unreported silence. */
static void test_journal_saturated(void) {
  relais_jr_tick r;
  memset(&r, 0, sizeof r);
  r.tick = 1;
  r.value = 0.5f;
  r.source = RELAIS_SRC_HOLDER;

  char small[32];
  fi_journal j;
  fi_journal_init(&j, small, sizeof small);
  T_CHECK(!fi_journal_write_tick(&j, &r));
  T_EQ_U(j.overflows, 1u);

  char big[RELAIS_JOURNAL_LINE_MAX];
  fi_journal k;
  fi_journal_init(&k, big, sizeof big);
  T_CHECK(fi_journal_write_tick(&k, &r));
  T_EQ_U(k.overflows, 0u);
}

/* MAN-04: the core allocates nothing, so a failing allocator cannot reach it;
 * and with no order delivered any more the actuator ends in the safe state,
 * applying exactly one bounded value per tick -- never a double command. */
static void test_lowmem_safe_state(void) {
  relais_params p;
  relais_params_default(&p);
  fi_mem mem;
  fi_mem_init(&mem, 1); /* fail the first allocation: the core must not care */
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_arb_out o;
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  relais_msg d;
  t_demande(&d, RELAIS_NODE_A, 0, 1, true, 0.5f, &p);
  relais_arb_step(&a, 1, &d, 1, &meas, &o);
  /* The queue is full and no order is delivered any more: the fallback loop
   * takes over at the end of the will, one bounded value per tick. */
  t_arb_silent_ticks(&a, 2, 9, &o);
  T_EQ_U(mem.alloc_calls, 0u);
  T_CHECK(o.source == RELAIS_SRC_FALLBACK);
  T_CHECK(isfinite((double)o.value));
  T_CHECK(fabsf(o.value) <= p.max_amplitude);
  T_EQ_U(applied_total(&a), 10u);
}

static void test_resource_exhaustion(void) {
  t_section("resource exhaustion");
  test_queue_full();
  test_journal_saturated();
  test_lowmem_safe_state();
}

int main(void) {
  test_flip_primitives();
  test_sec04_mandate_flip();
  test_det09_value_disagreement();
  test_crc_vs_mac();
  test_resource_exhaustion();
  return t_summary();
}