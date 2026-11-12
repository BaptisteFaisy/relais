/* Relais — the CAN transport, tested on a memory bus (mission M32, idea 75).
 *
 * A real controller is not available on this machine: creating a vcan interface
 * needs CAP_NET_ADMIN (`ip link add dev vcan0 type vcan` answers "Operation not
 * permitted" here), so the SocketCAN backend of can_socketcan.c is compiled but
 * not run. What CAN segmentation, reassembly, filtering and loss do to the
 * protocol, on the other hand, is fully testable: the medium below is a shared
 * bus with per-node inboxes, exactly like a wire everybody listens to.
 *
 * The last test is the one that matters: the 200-tick scenario with a death and
 * a handover, run three times — no transport, memory transport, fragmented CAN
 * — and the three decision streams must be identical, with zero violation of
 * I1..I11 judged by the core's own checker.
 *
 * Requirements: SEC-05, DET-04, ROB-01, ROB-02, ROB-03, MAN-04, I1..I11.
 */
#include <stdio.h>
#include <string.h>

#include "can.h"
#include "transport_mem.h"
#include "transport_world.h"

/* --------------------------------------------------- a shared memory bus --- */

#define BUS_NODES 3u   /* 0 actuator, 1 computer A, 2 computer B */
#define BUS_QUEUE 512u /* one tick of level C is ~200 CAN frames */

typedef struct {
  relais_can_frame q[BUS_QUEUE];
  size_t head;
  size_t count;
  uint32_t overflow;
} can_inbox;

typedef struct {
  can_inbox in[BUS_NODES];
  uint32_t bus_frames; /* every frame put on the wire by anybody */
  uint32_t lost;       /* frames the medium swallowed on purpose */
  uint32_t lose_at;    /* fault hook: drop the Nth frame from now (0 = none) */
} can_medium;

typedef struct {
  can_medium *m;
  unsigned self;
} can_port;

static can_medium g_medium;
static can_port g_ports[BUS_NODES];
static relais_can g_can[BUS_NODES];

/* Fresh medium, ports bound to it. Every test that talks on the bus starts
 * here, so no endpoint is ever left pointing at nothing. */
static void bus_reset(void) {
  memset(&g_medium, 0, sizeof g_medium);
  for (unsigned i = 0; i < BUS_NODES; i++) {
    g_ports[i].m = &g_medium;
    g_ports[i].self = i;
  }
}

static int port_xmit(void *ctx, const relais_can_frame *f) {
  can_port *p = (can_port *)ctx;
  p->m->bus_frames++;
  if (p->m->lose_at != 0u && p->m->bus_frames >= p->m->lose_at) {
    /* A CAN frame lost on the bus: the sender never knows, the receiver sees a
     * hole in the segmentation and nothing else. */
    p->m->lose_at = 0u;
    p->m->lost++;
    return RELAIS_TR_OK;
  }
  for (unsigned i = 0; i < BUS_NODES; i++) {
    if (i == p->self) continue; /* a node does not talk to itself here */
    can_inbox *in = &p->m->in[i];
    if (in->count >= BUS_QUEUE) {
      in->overflow++;
      continue;
    }
    size_t at = (in->head + in->count) % BUS_QUEUE;
    in->q[at] = *f;
    in->count++;
  }
  return RELAIS_TR_OK;
}

static int port_recv(void *ctx, relais_can_frame *f) {
  can_port *p = (can_port *)ctx;
  can_inbox *in = &p->m->in[p->self];
  if (in->count == 0) return RELAIS_TR_TIMEOUT;
  *f = in->q[in->head];
  in->head = (in->head + 1) % BUS_QUEUE;
  in->count--;
  return RELAIS_TR_OK;
}

/* CAN identifiers of the demonstration: the actuator has the lowest ones, and
 * on every node the actuator link is lower than the peer link, so orders and
 * status win the arbitration over state replication (spec 4.9). */
static const uint32_t TX_ID[BUS_NODES][RELAIS_LINK_COUNT] = {
    {0x100u, 0x101u}, /* actuator -> A, actuator -> B */
    {0x200u, 0x201u}, /* A -> actuator, A -> B */
    {0x300u, 0x301u}  /* B -> actuator, B -> A */
};

/* Who listens to which identifier, and on which link it arrives. An explicit
 * table, because that is exactly what a CAN filter is. The actuator hears both
 * computers on its actuator link; a computer hears the actuator on one link and
 * its peer on the other. */
static const uint32_t RX_ID[BUS_NODES][2] = {
    {0x200u, 0x300u}, /* actuator <- A, <- B */
    {0x100u, 0x301u}, /* A <- actuator, <- B */
    {0x101u, 0x201u}  /* B <- actuator, <- A */
};
static const relais_link RX_LINK[BUS_NODES][2] = {
    {RELAIS_LINK_ACTUATOR, RELAIS_LINK_ACTUATOR},
    {RELAIS_LINK_ACTUATOR, RELAIS_LINK_PEER},
    {RELAIS_LINK_ACTUATOR, RELAIS_LINK_PEER}};

static void bus_attach(relais_transport *t, unsigned self) {
  relais_can_bus bus;
  memset(&bus, 0, sizeof bus);
  bus.name = "memory-can";
  bus.ctx = &g_ports[self];
  bus.xmit = port_xmit;
  bus.recv = port_recv;
  relais_can_attach(t, &g_can[self], &bus, TX_ID[self][RELAIS_LINK_ACTUATOR],
                    TX_ID[self][RELAIS_LINK_PEER]);
  for (unsigned k = 0; k < 2u; k++) relais_can_accept(&g_can[self], RX_ID[self][k], RX_LINK[self][k]);
  relais_tr_cfg cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.actuator = "can0:actuator";
  cfg.peer = "can0:peer";
  cfg.mtu = RELAIS_CAN_DLC;
  (void)relais_tr_init(t, &cfg);
}

static void wire_can(world_t *w) {
  bus_reset();
  for (unsigned i = 0; i < BUS_NODES; i++) bus_attach(&w->tr[i], i);
}

static relais_transport w_tr(unsigned self) {
  relais_transport t;
  bus_attach(&t, self);
  return t;
}

/* ------------------------------------------------------- CAN unit tests --- */

static void test_fragmentation_math(void) {
  t_section("can: the cost of segmentation, computed before plugging anything");
  T_EQ_U(relais_can_fragment_count(0), 0u);
  T_EQ_U(relais_can_fragment_count(1), 1u);
  T_EQ_U(relais_can_fragment_count(RELAIS_CAN_SF_MAX), 1u);      /* 7 -> 1 */
  T_EQ_U(relais_can_fragment_count(RELAIS_CAN_SF_MAX + 1u), 2u); /* 8 -> FF+CF */
  T_EQ_U(relais_can_fragment_count(13), 2u);                     /* 6 + 7 */
  T_EQ_U(relais_can_fragment_count(14), 3u);                     /* 6 + 7 + 1 */
  T_EQ_U(relais_can_fragment_count(68), 10u);                    /* 6 + 9*7 */
  T_EQ_U(relais_can_fragment_count(512), 74u);                   /* worst case */
}

/* Encode a message, push it through one CAN endpoint, pull it out of the other
 * and compare the bytes. Returns the number of CAN frames the bus carried. */
static uint32_t can_roundtrip(const relais_msg *m, size_t *encoded) {
  relais_key key;
  relais_key_from_hex(&key, "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
  uint8_t wire[RELAIS_FRAME_MAX];
  size_t len = relais_frame_encode(m, &key, wire, sizeof wire);
  if (encoded != NULL) *encoded = len;
  if (len == 0) return 0;
  uint32_t before = g_medium.bus_frames;
  relais_transport src = w_tr(1);
  relais_transport dst = w_tr(0);
  if (relais_tr_send_raw(&src, RELAIS_LINK_ACTUATOR, wire, len) != RELAIS_TR_OK) return 0;
  uint8_t back[RELAIS_FRAME_MAX];
  size_t got = 0;
  relais_link link = RELAIS_LINK_PEER;
  if (relais_tr_recv_raw(&dst, &link, back, sizeof back, &got) != RELAIS_TR_OK) return 0;
  T_EQ_U(got, len);
  T_CHECK(memcmp(wire, back, len) == 0);
  T_EQ_U(link, RELAIS_LINK_ACTUATOR);
  relais_msg decoded;
  relais_reject why = RELAIS_REJ_NONE;
  T_CHECK(relais_frame_decode(back, got, &key, &decoded, &why));
  T_EQ_U(decoded.type, m->type);
  return g_medium.bus_frames - before;
}

typedef struct {
  uint32_t frames;
  uint32_t bytes;
} can_cost;

/* One message across the bus: bytes in, CAN frames out, printed for the docs. */
static void roundtrip_report(const char *label, const relais_msg *m, can_cost *cost) {
  size_t encoded = 0;
  cost->frames += can_roundtrip(m, &encoded);
  cost->bytes += (uint32_t)encoded;
  printf("  %-8s %3u bytes -> %2u CAN frames\n", label, (unsigned)encoded,
         (unsigned)relais_can_fragment_count(encoded));
}

static void test_roundtrip_every_type(void) {
  t_section("can: every Relais message type survives segmentation");
  bus_reset();
  relais_params p;
  relais_params_default(&p);
  relais_ctl_state st;
  relais_meas meas;
  memset(&meas, 0, sizeof meas);
  relais_ctl_init(&st, &p);
  relais_msg m;
  can_cost cost = {0u, 0u};

  t_statut(&m, 12, 3, RELAIS_NODE_A, RELAIS_SRC_HOLDER, 0, &meas);
  roundtrip_report("STATUT", &m, &cost);
  t_order(&m, RELAIS_NODE_A, 3, 13, 0.42f, &p);
  roundtrip_report("ORDRE", &m, &cost);
  t_shadow(&m, RELAIS_NODE_B, 13, 0.42f, 0);
  roundtrip_report("OMBRE", &m, &cost);
  t_demande(&m, RELAIS_NODE_B, 3, 14, true, 0.4f, &p);
  roundtrip_report("DEMANDE", &m, &cost);
  t_cession(&m, RELAIS_NODE_A, 3, 1);
  roundtrip_report("CESSION", &m, &cost);
  t_etat(&m, RELAIS_NODE_A, 14, &st, 0.42f);
  roundtrip_report("ETAT", &m, &cost);

  T_CHECK(cost.frames > 0u);
  T_CHECK(cost.bytes > 0u);
  printf("  total: %u Relais bytes -> %u CAN frames on the bus\n", (unsigned)cost.bytes,
         (unsigned)cost.frames);
}

static void test_lost_fragment_is_a_silence(void) {
  t_section("can: a lost fragment is a silence, never an invented frame");
  bus_reset();
  relais_transport src = w_tr(1);
  relais_transport dst = w_tr(0);
  relais_key key;
  relais_key_from_hex(&key, "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
  relais_params p;
  relais_params_default(&p);
  relais_msg m, got;
  uint8_t wire[RELAIS_FRAME_MAX], back[RELAIS_FRAME_MAX];
  size_t n = 0;

  t_order(&m, RELAIS_NODE_A, 1, 5, 0.2f, &p);
  size_t len = relais_frame_encode(&m, &key, wire, sizeof wire);
  /* The second CAN frame of the flow disappears: the reassembly sees a hole in
   * the sequence, abandons the flow and counts it (ROB-01). */
  g_medium.lose_at = g_medium.bus_frames + 2u;
  T_EQ_U(relais_tr_send_raw(&src, RELAIS_LINK_ACTUATOR, wire, len), RELAIS_TR_OK);
  T_EQ_U(relais_tr_recv_raw(&dst, NULL, back, sizeof back, &n), RELAIS_TR_TIMEOUT);
  T_EQ_U(n, 0u);
  T_CHECK(g_can[0].lost_flows >= 1u);
  T_CHECK(g_can[0].dropped_pci >= 1u);
  T_EQ_U(g_medium.lost, 1u);

  /* The bus recovers, the next attempt goes through: nothing was corrupted. */
  relais_reject why = RELAIS_REJ_NONE;
  T_EQ_U(relais_tr_send_raw(&src, RELAIS_LINK_ACTUATOR, wire, len), RELAIS_TR_OK);
  T_EQ_U(relais_tr_recv_raw(&dst, NULL, back, sizeof back, &n), RELAIS_TR_OK);
  T_EQ_U(n, len);
  T_CHECK(relais_frame_decode(back, n, &key, &got, &why));
}

static void test_foreign_and_tampered(void) {
  t_section("can: foreign traffic ignored, a tampered frame refused");
  bus_reset();
  relais_transport src = w_tr(1);
  relais_transport dst = w_tr(0);
  relais_key key;
  relais_key_from_hex(&key, "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
  relais_params p;
  relais_params_default(&p);
  relais_msg m, out;
  uint8_t wire[RELAIS_FRAME_MAX], back[RELAIS_FRAME_MAX];
  size_t n = 0;

  /* A frame on an identifier nobody listens to is dropped, not decoded. */
  relais_can_frame foreign;
  memset(&foreign, 0, sizeof foreign);
  foreign.id = 0x7ffu;
  foreign.len = 8;
  for (int i = 0; i < 8; i++) foreign.data[i] = (uint8_t)i;
  uint32_t before = g_can[0].dropped_id;
  T_EQ_U(port_xmit(&g_ports[1], &foreign), RELAIS_TR_OK);
  T_EQ_U(relais_tr_recv_raw(&dst, NULL, back, sizeof back, &n), RELAIS_TR_TIMEOUT);
  T_EQ_U(g_can[0].dropped_id, before + 1u);

  /* One flipped bit inside a segment: the reassembled frame fails the MAC and is
   * refused by the core, exactly as over UDP (DET-04, SEC-02). */
  t_order(&m, RELAIS_NODE_A, 1, 5, 0.2f, &p);
  size_t len = relais_frame_encode(&m, &key, wire, sizeof wire);
  T_EQ_U(relais_tr_send_raw(&src, RELAIS_LINK_ACTUATOR, wire, len), RELAIS_TR_OK);
  can_inbox *inbox = &g_medium.in[0];
  inbox->q[(inbox->head + inbox->count - 1u) % BUS_QUEUE].data[1] ^= 0x80u;
  relais_reject why = RELAIS_REJ_NONE;
  relais_link link = RELAIS_LINK_PEER;
  T_EQ_U(relais_tr_recv_msg(&dst, &key, &link, &out, &why, back, sizeof back),
         RELAIS_TR_ERR_IO);
  T_EQ_U(why, RELAIS_REJ_CHECK);
}

static void test_identifier_priority(void) {
  t_section("can: identifiers give the actuator link the bus priority");
  for (unsigned i = 0; i < BUS_NODES; i++) {
    T_CHECK(TX_ID[i][RELAIS_LINK_ACTUATOR] < TX_ID[i][RELAIS_LINK_PEER]);
    T_CHECK(TX_ID[i][RELAIS_LINK_ACTUATOR] <= RELAIS_CAN_ID_MAX);
  }
  /* The actuator is the clock: its frames must win every arbitration. */
  T_CHECK(TX_ID[0][RELAIS_LINK_PEER] < TX_ID[1][RELAIS_LINK_ACTUATOR]);
  T_CHECK(TX_ID[1][RELAIS_LINK_PEER] < TX_ID[2][RELAIS_LINK_ACTUATOR]);
}

/* ------------------------------------- the whole system over fragmented CAN -- */

static world_t g_direct, g_memory, g_can_world;

/* A differential test that cannot show WHERE it diverges is not debuggable. */
static void report_divergence(const char *what, uint32_t i, const trace_tick *a,
                             const trace_tick *b) {
  printf("  [%s] first divergence at tick %u: value %.6f/%.6f source %u/%u holder %u/%u "
         "mandate %u/%u misses %u/%u lamp %u/%u cause %u/%u events %u/%u\n", what,
         (unsigned)(i + 1u), (double)a->value, (double)b->value, a->source, b->source,
         a->holder, b->holder, (unsigned)a->mandate, (unsigned)b->mandate, a->misses,
         b->misses, a->lamp, b->lamp, a->cause, b->cause, a->event_count, b->event_count);
}

static void wire_memory_world(world_t *w) { wire_memory_endpoints(w->tr); }

static void test_scenario_over_can(void) {
  t_section("can: 200 ticks, a death, a handover — same decisions as without a bus");
  run_scenario(&g_direct, NULL);
  run_scenario(&g_memory, wire_memory_world);
  run_scenario(&g_can_world, wire_can);

  uint32_t same_mem = 0, same_can = 0;
  for (uint32_t i = 0; i < SCEN_TICKS; i++) {
    if (tick_identical(&g_direct.trace[i], &g_memory.trace[i])) same_mem++;
    else if (same_mem == i) report_divergence("memory", i, &g_direct.trace[i], &g_memory.trace[i]);
    if (tick_identical(&g_direct.trace[i], &g_can_world.trace[i])) same_can++;
    else if (same_can == i) report_divergence("can", i, &g_direct.trace[i], &g_can_world.trace[i]);
  }
  T_EQ_U(same_mem, SCEN_TICKS);
  T_EQ_U(same_can, SCEN_TICKS);
  T_CHECK(relais_inv_ok(&g_direct.inv));
  T_CHECK(relais_inv_ok(&g_memory.inv));
  T_CHECK(relais_inv_ok(&g_can_world.inv));
  T_EQ_U(g_can_world.inv.violations, 0u);
  T_EQ_U(g_can_world.rejected, 0u);   /* MAC on: every frame authenticated */
  T_EQ_U(g_can_world.switches, g_direct.switches);
  T_CHECK(g_can_world.switches >= 1u);
  T_EQ_U(g_can_world.trace[SCEN_TICKS - 1].holder, RELAIS_NODE_B);
  /* Nothing was silently dropped for the two LIVE endpoints: the comparison is
   * honest. The dead machine (index 1) stops reading the bus at SCEN_DEATH, so
   * its inbox saturates — that is what a dead node looks like on a shared
   * medium, and it changes nothing for anybody else. */
  T_EQ_U(g_medium.in[0].overflow, 0u);
  T_EQ_U(g_medium.in[2].overflow, 0u);
  T_EQ_U(g_medium.lost, 0u);
  printf("  can: the dead node's inbox saturated after tick %u (%u frames unread)\n",
         (unsigned)SCEN_DEATH, (unsigned)g_medium.in[1].overflow);

  uint32_t can_frames = 0;
  for (unsigned i = 0; i < BUS_NODES; i++) can_frames += g_can[i].tx_frames;
  printf("  can: %u Relais frames -> %u CAN frames on the bus (%.2f CAN frames per"
         " Relais frame), %u lost flows, %u foreign, violations=%u\n",
         (unsigned)g_can_world.frames, (unsigned)can_frames,
         g_can_world.frames ? (double)can_frames / (double)g_can_world.frames : 0.0,
         (unsigned)(g_can[0].lost_flows + g_can[1].lost_flows + g_can[2].lost_flows),
         (unsigned)(g_can[0].dropped_id + g_can[1].dropped_id + g_can[2].dropped_id),
         (unsigned)g_can_world.inv.violations);
  printf("  can: sources holder=%u shadow=%u testament=%u fallback=%u\n",
         (unsigned)count_source(&g_can_world, RELAIS_SRC_HOLDER),
         (unsigned)count_source(&g_can_world, RELAIS_SRC_SHADOW),
         (unsigned)count_source(&g_can_world, RELAIS_SRC_TESTAMENT),
         (unsigned)count_source(&g_can_world, RELAIS_SRC_FALLBACK));
  printf("  memory transport for the same run: %u frames\n", (unsigned)g_memory.frames);
}

int main(void) {
  test_fragmentation_math();
  test_roundtrip_every_type();
  test_lost_fragment_is_a_silence();
  test_foreign_and_tampered();
  test_identifier_priority();
  test_scenario_over_can();
  return t_summary();
}
