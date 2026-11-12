/* Relais — the transport interface, proven transport-independent (M32, idea 75).
 *
 * The interface of core/include/relais/transport.h is exercised here against a
 * memory transport (static queues, no socket, no clock, no allocation). The two
 * real implementations cannot be tested by a unit test: UDP needs a live
 * loopback socket and CAN needs a bus, so they are checked by their own
 * harnesses (host/transport/udp_check.c, host/transport/test_can.c).
 *
 * The decisive test is the last one: the SAME scenario — two computers, one
 * actuator, a death and a handover — is run twice, once with the frames passed
 * by direct call and once with every frame encoded, queued, decoded through the
 * memory transport. Both runs must produce the same decisions tick by tick and
 * zero invariant violation. That is what "the protocol does not depend on the
 * transport" means, measured rather than asserted.
 *
 * Requirements covered: SEC-05, DET-04, SEC-01, SEC-02, ROB-01, ROB-02, I1..I11.
 */
#include <stdio.h>
#include <string.h>

#include "relais/invariants.h"
#include "relais/transport.h"
#include "test_util.h"

#include "transport_mem.h"
#include "transport_world.h"

/* ------------------------------------------------------- interface tests -- */

static void test_vtable_validation(void) {
  t_section("transport: vtable validation and NULL safety");
  relais_transport t;
  memset(&t, 0, sizeof t);
  T_CHECK(!relais_tr_valid(NULL));
  T_CHECK(!relais_tr_valid(&t)); /* no verb at all */
  t.name = "half";
  t.init = mem_init;
  t.send = mem_send;
  T_CHECK(!relais_tr_valid(&t)); /* recv and close missing */
  t.recv = mem_recv;
  t.close = mem_close;
  T_CHECK(relais_tr_valid(&t));
  /* An unusable transport answers with a status; it never crashes. */
  t.ctx = NULL;
  T_EQ_U(relais_tr_init(NULL, NULL), RELAIS_TR_ERR_CONFIG);
  T_EQ_U(relais_tr_send_raw(NULL, RELAIS_LINK_PEER, NULL, 0), RELAIS_TR_ERR_CONFIG);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = 12345;
  T_EQ_U(relais_tr_recv_raw(NULL, NULL, buf, sizeof buf, &len), RELAIS_TR_ERR_CONFIG);
  T_EQ_U(len, 0u);
  relais_tr_close(NULL); /* must not crash */
  T_CHECK(true);
}

static void test_link_rule(void) {
  t_section("transport: the link rule of spec 4.9 is the one of the core");
  for (int ty = RELAIS_MSG_STATUT; ty <= RELAIS_MSG_SUIVI; ty++) {
    relais_msg_type type = (relais_msg_type)ty;
    relais_link want = relais_msg_to_peer(type) ? RELAIS_LINK_PEER : RELAIS_LINK_ACTUATOR;
    T_EQ_U(relais_link_of(type), (uint64_t)want);
  }
  /* ETAT and SUIVI are the only frames between the two computers. */
  T_EQ_U(relais_link_of(RELAIS_MSG_ETAT), RELAIS_LINK_PEER);
  T_EQ_U(relais_link_of(RELAIS_MSG_SUIVI), RELAIS_LINK_PEER);
  T_EQ_U(relais_link_of(RELAIS_MSG_ORDRE), RELAIS_LINK_ACTUATOR);
  T_EQ_U(relais_link_of(RELAIS_MSG_STATUT), RELAIS_LINK_ACTUATOR);
}

static void test_roundtrip(void) {
  t_section("transport: a frame survives the trip, byte for byte");
  memset(&g_bus, 0, sizeof g_bus);
  mem_ctx ctx_a, ctx_act;
  relais_transport a = make_endpoint(&ctx_a, 1, 0, 2);
  relais_transport act = make_endpoint(&ctx_act, 0, 1, 2);
  T_EQ_U(relais_tr_init(&a, NULL), RELAIS_TR_ERR_CONFIG);

  relais_params p;
  relais_params_default(&p);
  relais_key key;
  T_CHECK(relais_key_from_hex(&key, "00112233445566778899aabbccddeeff"));
  relais_msg ordre, got;
  t_order(&ordre, RELAIS_NODE_A, 1, 7, 0.35f, &p);
  uint8_t scratch[RELAIS_FRAME_MAX];
  T_EQ_U(relais_tr_send_msg(&a, &key, &ordre, scratch, sizeof scratch), RELAIS_TR_OK);
  T_CHECK(g_bus.bytes > 0u); /* the frame really went through the medium */

  uint8_t buf[RELAIS_FRAME_MAX];
  relais_link link = RELAIS_LINK_PEER;
  relais_reject why = RELAIS_REJ_NONE;
  T_EQ_U(relais_tr_recv_msg(&act, &key, &link, &got, &why, buf, sizeof buf), RELAIS_TR_OK);
  T_EQ_U(link, RELAIS_LINK_ACTUATOR);
  T_EQ_U(got.type, RELAIS_MSG_ORDRE);
  T_EQ_U(got.sender, RELAIS_NODE_A);
  T_EQ_U(got.tick, 7u);
  T_EQ_U(got.mandate, 1u);
  T_NEAR(got.value, 0.35, 1e-6);
  T_EQ_U(got.testament_len, ordre.testament_len);

  /* Level S is served by the same interface: no CRC-32C special case anywhere. */
  relais_key plain;
  relais_key_clear(&plain);
  T_EQ_U(relais_tr_send_msg(&a, &plain, &ordre, scratch, sizeof scratch), RELAIS_TR_OK);
  T_EQ_U(relais_tr_recv_msg(&act, &plain, &link, &got, &why, buf, sizeof buf), RELAIS_TR_OK);
  T_EQ_U(got.tick, 7u);
  relais_tr_close(&a);
  relais_tr_close(&act);
  T_EQ_U(relais_tr_send_raw(&a, RELAIS_LINK_ACTUATOR, buf, 8), RELAIS_TR_CLOSED);
}

/* Flip one bit in flight: the medium delivered it, the core must refuse it. */
static void corrupt_and_refuse(size_t at, relais_reject expected) {
  mem_ctx ctx_a, ctx_act;
  relais_transport a = make_endpoint(&ctx_a, 1, 0, 2);
  relais_transport act = make_endpoint(&ctx_act, 0, 1, 2);
  relais_params p;
  relais_params_default(&p);
  relais_key key;
  relais_key_from_hex(&key, "00112233445566778899aabbccddeeff");
  relais_msg ordre, got;
  uint8_t scratch[RELAIS_FRAME_MAX], buf[RELAIS_FRAME_MAX];
  relais_link link = RELAIS_LINK_PEER;
  relais_reject why = RELAIS_REJ_NONE;
  t_order(&ordre, RELAIS_NODE_A, 1, 7, 0.35f, &p);
  g_bus.corrupt_next = true;
  g_bus.corrupt_at = at;
  T_EQ_U(relais_tr_send_msg(&a, &key, &ordre, scratch, sizeof scratch), RELAIS_TR_OK);
  T_EQ_U(relais_tr_recv_msg(&act, &key, &link, &got, &why, buf, sizeof buf), RELAIS_TR_ERR_IO);
  T_CHECK(why != RELAIS_REJ_NONE); /* the core said why it refused */
  if (expected != RELAIS_REJ_NONE) T_EQ_U(why, expected);
}

static void test_tampered_frames(void) {
  t_section("transport: a tampered frame never reaches the protocol");
  memset(&g_bus, 0, sizeof g_bus);
  corrupt_and_refuse(30, RELAIS_REJ_CHECK);     /* SEC-02: the MAC sees it */
  memset(&g_bus, 0, sizeof g_bus);
  /* DET-04: a flipped bit in the header is caught by the shape check first. */
  corrupt_and_refuse(20, RELAIS_REJ_NONE);
}

static void test_error_paths(void) {
  t_section("transport: timeout, small buffer and full queue are reported");
  memset(&g_bus, 0, sizeof g_bus);
  mem_ctx ctx_a, ctx_act;
  relais_transport a = make_endpoint(&ctx_a, 1, 0, 2);
  relais_transport act = make_endpoint(&ctx_act, 0, 1, 2);
  uint8_t buf[RELAIS_FRAME_MAX];
  size_t len = 0;
  /* ROB-01: silence is a status, not a guess and not a stale frame. */
  T_EQ_U(relais_tr_recv_raw(&act, NULL, buf, sizeof buf, &len), RELAIS_TR_TIMEOUT);
  T_EQ_U(len, 0u);
  T_EQ_U(ctx_act.timeouts, 1u);
  T_EQ_U(ctx_a.timeouts, 0u);

  relais_params p;
  relais_params_default(&p);
  relais_key key;
  relais_key_clear(&key);
  relais_msg m;
  t_order(&m, RELAIS_NODE_A, 1, 3, 0.1f, &p);
  uint8_t scratch[RELAIS_FRAME_MAX];
  size_t need = relais_frame_encode(&m, &key, scratch, sizeof scratch);
  T_CHECK(need > 0);
  T_EQ_U(relais_tr_send_msg(&a, &key, &m, scratch, 8), RELAIS_TR_ERR_FULL);
  T_EQ_U(relais_tr_send_msg(&a, &key, &m, NULL, sizeof scratch), RELAIS_TR_ERR_FULL);
  T_EQ_U(relais_tr_recv_raw(&act, NULL, buf, need - 1, &len), RELAIS_TR_TIMEOUT);
  /* The frame is still there: a too-small buffer loses nothing. */
  T_EQ_U(relais_tr_send_raw(&a, RELAIS_LINK_ACTUATOR, scratch, need), RELAIS_TR_OK);
  T_EQ_U(relais_tr_recv_raw(&act, NULL, buf, need - 1, &len), RELAIS_TR_ERR_FULL);
  T_EQ_U(relais_tr_recv_raw(&act, NULL, buf, sizeof buf, &len), RELAIS_TR_OK);
  T_EQ_U(len, need);
  T_EQ_U(relais_tr_recv_raw(&act, NULL, buf, sizeof buf, &len), RELAIS_TR_TIMEOUT);
  /* A queue that overflows reports it and counts the loss (ROB-02). */
  uint32_t ok = 0;
  for (uint32_t i = 0; i < MEM_DEPTH + 4u; i++)
    if (relais_tr_send_raw(&a, RELAIS_LINK_ACTUATOR, scratch, need) == RELAIS_TR_OK) ok++;
  T_EQ_U(ok, MEM_DEPTH);
  T_CHECK(g_bus.q[0][RELAIS_LINK_ACTUATOR].full_drops >= 4u);
  relais_tr_close(&a);
  relais_tr_close(&act);
}

/* ------------------------------------------------ the system over a medium -- */

static world_t g_direct, g_wired;

static void wire_memory(world_t *w) { wire_memory_endpoints(w->tr); }

/* The decisive test: the same scenario, the same decisions, tick by tick, with
 * and without a medium in between. If the protocol depended on UDP in any way,
 * this is where it would show. */
static void test_scenario_equivalence(void) {
  t_section("transport: same decisions with and without a medium (200 ticks)");
  run_scenario(&g_direct, NULL);
  run_scenario(&g_wired, wire_memory);

  uint32_t same = 0;
  for (uint32_t i = 0; i < SCEN_TICKS; i++)
    if (tick_identical(&g_direct.trace[i], &g_wired.trace[i])) same++;
  T_EQ_U(same, SCEN_TICKS);
  T_EQ_U(g_direct.frames, 0u);          /* nothing crossed a medium */
  T_CHECK(g_wired.frames > SCEN_TICKS); /* the wired run really used it */
  T_EQ_U(g_wired.rejected, 0u);         /* MAC on: not one frame was refused */

  /* I1..I11 hold over the foreign transport, judged by the core's own checker. */
  T_CHECK(relais_inv_ok(&g_direct.inv));
  T_CHECK(relais_inv_ok(&g_wired.inv));
  T_EQ_U(g_wired.inv.violations, 0u);
  T_EQ_U(g_direct.inv.violations, 0u);

  /* The trace has teeth: a death, an auto-switch and a substitution happened. */
  T_EQ_U(g_wired.switches, g_direct.switches);
  T_CHECK(g_wired.switches >= 1u);
  const trace_tick *last = &g_wired.trace[SCEN_TICKS - 1];
  T_EQ_U(last->holder, RELAIS_NODE_B);
  T_EQ_U(last->mandate, 2u); /* MAN-03: one step per change of holder */
  T_CHECK(count_source(&g_wired, RELAIS_SRC_SHADOW) > 0u);
  T_CHECK(count_source(&g_wired, RELAIS_SRC_HOLDER) > SCEN_TICKS / 2u);
  printf("  wired: %u frames over the memory transport, %u rejected, "
         "sources holder=%u shadow=%u testament=%u fallback=%u, violations=%u\n",
         (unsigned)g_wired.frames, (unsigned)g_wired.rejected,
         (unsigned)count_source(&g_wired, RELAIS_SRC_HOLDER),
         (unsigned)count_source(&g_wired, RELAIS_SRC_SHADOW),
         (unsigned)count_source(&g_wired, RELAIS_SRC_TESTAMENT),
         (unsigned)count_source(&g_wired, RELAIS_SRC_FALLBACK),
         (unsigned)g_wired.inv.violations);
}

int main(void) {
  test_vtable_validation();
  test_link_rule();
  test_roundtrip();
  test_tampered_frames();
  test_error_paths();
  test_scenario_equivalence();
  return t_summary();
}
