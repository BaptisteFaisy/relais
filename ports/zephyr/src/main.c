/* Relais on Zephyr — one computer, three cooperative threads (mission M32).
 *
 * The QNX image splits a computer in three processes (target/): relais-net owns
 * the socket, relais-core owns the protocol state machine, relais-ctl owns the
 * control law. This port keeps that split, with threads instead of processes,
 * because the point of the exercise is that the CORE IS THE SAME BINARY:
 * core/src/*.c is compiled here exactly as it is on the PC and on QNX.
 *
 * Priority mapping. QNX uses SCHED_FIFO where a LOWER number is stronger
 * (net 62, core 61, ctl 60). Zephyr cooperative priorities are negative and a
 * MORE NEGATIVE number is stronger, so the port applies one rule:
 *
 *     zephyr_prio(qnx) = -(63 - qnx)      62 -> -1, 61 -> -2, 60 -> -3
 *
 * The ordering is preserved: ctl preempts core, core preempts net, and none of
 * the three can be preempted by a preemptible thread. CONFIG_NUM_COOP_PRIORITIES
 * must be >= 3 (prj.conf asks for 8).
 *
 * NOT COMPILED HERE: this machine has no Zephyr SDK, so this file is delivered
 * with its build files and the exact commands (docs/PORT-ZEPHYR.md). What is
 * verified here is the conditional check `make -C ports/zephyr check`, which
 * reports the missing toolchain and exits 0.
 *
 * Requirement IDs: QNX-01 (equivalent fixed priorities), SEC-05 (one owner of
 * the socket), CMD-02, CMD-09 (the law as a service), MAN-09, REP-01, REP-02.
 */
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "relais/node.h"
#include "relais/params.h"
#include "relais_port.h"
#include "udp.h"

LOG_MODULE_REGISTER(relais, CONFIG_RELAIS_LOG_LEVEL);

/* --- the priority rule above, written once --- */
#define RELAIS_PRIO_NET  (-1) /* QNX 62 */
#define RELAIS_PRIO_CORE (-2) /* QNX 61 */
#define RELAIS_PRIO_CTL  (-3) /* QNX 60 */

#define STACK_NET  2048
#define STACK_CORE 4096
#define STACK_CTL  2048
#define QUEUE_DEPTH 8

/* The RPC between the core and the law, the equivalent of a QNX MsgSend with
 * one client: one request in flight, so a plain pair of message queues is a
 * lock-step rendez-vous and needs no mutex. */
struct ctl_req {
  relais_meas meas;
  bool want_state; /* the core asks for the state to replicate (REP-01) */
  bool import;     /* the core pushes a state back (REP-02) */
  uint8_t state[RELAIS_CTL_STATE_MAX];
  uint8_t state_len;
};

struct ctl_rep {
  float value;
  uint8_t state[RELAIS_CTL_STATE_MAX];
  uint8_t state_len;
};

static K_THREAD_STACK_DEFINE(net_stack, STACK_NET);
static K_THREAD_STACK_DEFINE(core_stack, STACK_CORE);
static K_THREAD_STACK_DEFINE(ctl_stack, STACK_CTL);
static struct k_thread net_tid, core_tid, ctl_tid;

static K_MSGQ_DEFINE(net_to_core, sizeof(relais_msg), QUEUE_DEPTH, 4);
static K_MSGQ_DEFINE(core_to_net, sizeof(relais_msg), QUEUE_DEPTH, 4);
static K_MSGQ_DEFINE(ctl_requests, sizeof(struct ctl_req), 1, 4);
static K_MSGQ_DEFINE(ctl_replies, sizeof(struct ctl_rep), 1, 4);

static relais_params g_params;
static relais_key g_key;
static relais_udp g_udp;
static relais_transport g_transport;
static relais_node_sm g_node;
/* Which computer this board is: one Kconfig switch, no code difference. */
static relais_node g_self = IS_ENABLED(CONFIG_RELAIS_NODE_A) ? RELAIS_NODE_A : RELAIS_NODE_B;

/* ------------------------------------------------------- relais-ctl thread -- */

/* The control law as a service (CMD-09): three functions, no socket, no clock,
 * no notion of mandate. Same body as target/ctl.c, different kernel. */
static void ctl_thread(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  relais_ctl_state state;
  relais_ctl_init(&state, &g_params);
  LOG_INF("ctl: up, Zephyr priority %d (QNX %d)", RELAIS_PRIO_CTL, RELAIS_QNX_PRIO_CTL);
  for (;;) {
    struct ctl_req req;
    if (k_msgq_get(&ctl_requests, &req, K_FOREVER) != 0) continue;
    struct ctl_rep rep = {0};
    if (req.import) {
      /* REP-02: an unknown version or length is refused, the replica keeps
       * what it had rather than jumping to something it cannot trust. */
      (void)relais_ctl_import(&state, req.state, req.state_len);
    } else if (req.want_state) {
      rep.state_len = relais_ctl_export(&state, rep.state, sizeof rep.state);
      rep.value = state.last_value;
    } else {
      /* CMD-06: bounded here, before the value ever reaches the wire. */
      rep.value = relais_clamp_value(relais_ctl_step(&state, &req.meas, &g_params), &g_params);
      rep.state_len = relais_ctl_export(&state, rep.state, sizeof rep.state);
    }
    (void)k_msgq_put(&ctl_replies, &rep, K_FOREVER);
  }
}

/* The relais_law hooks of the core, answered by the ctl thread. This is what
 * makes the law a separate service without touching one line of the protocol. */
static void law_init(void *ctx, const relais_params *p) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(p); /* the ctl thread owns the only state */
}

static float law_step(void *ctx, const relais_meas *m, const relais_params *p) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(p);
  struct ctl_req req = {0};
  req.meas = *m;
  struct ctl_rep rep = {0};
  if (k_msgq_put(&ctl_requests, &req, K_FOREVER) != 0) return 0.0f;
  if (k_msgq_get(&ctl_replies, &rep, K_FOREVER) != 0) return 0.0f;
  return rep.value;
}

static uint8_t law_export(void *ctx, uint8_t *buf, size_t cap) {
  ARG_UNUSED(ctx);
  struct ctl_req req = {0};
  req.want_state = true;
  struct ctl_rep rep = {0};
  if (k_msgq_put(&ctl_requests, &req, K_FOREVER) != 0) return 0;
  if (k_msgq_get(&ctl_replies, &rep, K_FOREVER) != 0) return 0;
  size_t n = rep.state_len < cap ? rep.state_len : cap;
  for (size_t i = 0; i < n; i++) buf[i] = rep.state[i];
  return (uint8_t)n;
}

static bool law_import(void *ctx, const uint8_t *buf, size_t len) {
  ARG_UNUSED(ctx);
  struct ctl_req req = {0};
  req.import = true;
  req.state_len = len < sizeof req.state ? (uint8_t)len : (uint8_t)sizeof req.state;
  for (size_t i = 0; i < req.state_len; i++) req.state[i] = buf[i];
  struct ctl_rep rep = {0};
  if (k_msgq_put(&ctl_requests, &req, K_FOREVER) != 0) return false;
  return k_msgq_get(&ctl_replies, &rep, K_FOREVER) == 0;
}

static const relais_law g_law = {
    .ctx = NULL, .init = law_init, .step = law_step,
    .export_state = law_export, .import_state = law_import};

/* ------------------------------------------------------- relais-net thread -- */

/* The only thread that touches the socket (SEC-05). A frame that fails the
 * shape or the authenticator check dies here and is counted, so the core never
 * sees a forged frame (DET-04, SEC-01). */
static void net_thread(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  uint8_t buf[RELAIS_FRAME_MAX];
  LOG_INF("net: up, Zephyr priority %d (QNX %d)", RELAIS_PRIO_NET, RELAIS_QNX_PRIO_NET);
  for (;;) {
    relais_link link = RELAIS_LINK_ACTUATOR;
    size_t len = 0;
    if (relais_tr_recv_raw(&g_transport, &link, buf, sizeof buf, &len) == RELAIS_TR_OK) {
      relais_msg msg;
      relais_reject why = RELAIS_REJ_NONE;
      if (relais_frame_decode(buf, len, &g_key, &msg, &why)) {
        if (k_msgq_put(&net_to_core, &msg, K_NO_WAIT) != 0)
          LOG_WRN("net: core queue full, frame dropped");
      } else {
        LOG_WRN("net: frame dropped (%s)", relais_reject_name(why));
      }
    }
    /* One frame out, whatever the core asked for. The queue is short by design:
     * a tick produces at most RELAIS_OUT_MSG_MAX frames. */
    relais_msg out;
    while (k_msgq_get(&core_to_net, &out, K_NO_WAIT) == 0) {
      uint8_t scratch[RELAIS_FRAME_MAX];
      (void)relais_tr_send_msg(&g_transport, &g_key, &out, scratch, sizeof scratch);
    }
  }
}

/* ------------------------------------------------------ relais-core thread -- */

static void core_thread(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  relais_node_init(&g_node, &g_params, g_self);
  relais_node_set_law(&g_node, &g_law); /* CMD-09: the law lives in its thread */
  LOG_INF("core: up as %s, Zephyr priority %d (QNX %d)", relais_node_name(g_self),
          RELAIS_PRIO_CORE, RELAIS_QNX_PRIO_CORE);
  for (;;) {
    relais_msg msg;
    relais_node_out out;
    relais_node_clear_out(&out);
    /* One tick period without a frame is the ordinary case at the beginning of
     * a run; three of them is silence, and MAN-09 answers it. */
    if (k_msgq_get(&net_to_core, &msg, K_MSEC(g_params.period_ms * 3u)) != 0) {
      relais_node_on_silence(&g_node, g_node.now + 1, &out);
    } else {
      (void)relais_port_dispatch(&g_node, &msg, &out);
    }
    for (uint8_t i = 0; i < out.count; i++) (void)k_msgq_put(&core_to_net, &out.out[i], K_FOREVER);
  }
}

/* ------------------------------------------------------------------- main -- */

int main(void) {
  relais_params_default(&g_params);
  relais_key_clear(&g_key); /* level S: CRC-32C; level C sets a key */
  relais_udp_attach(&g_transport, &g_udp);

  relais_tr_cfg cfg = {0};
  static char actuator[32], peer[32], local[32];
  snprintf(actuator, sizeof actuator, "%s", CONFIG_RELAIS_ACTUATOR);
  snprintf(peer, sizeof peer, "%s", CONFIG_RELAIS_PEER);
  snprintf(local, sizeof local, ":%d", CONFIG_RELAIS_LOCAL_PORT);
  cfg.actuator = actuator;
  cfg.peer = peer;
  cfg.local = local;
  cfg.timeout_ms = g_params.period_ms; /* the net thread polls at the tick rate */
  if (relais_tr_init(&g_transport, &cfg) != RELAIS_TR_OK) {
    LOG_ERR("transport: cannot open the UDP endpoint");
    return 1;
  }

  k_thread_create(&ctl_tid, ctl_stack, STACK_CTL, ctl_thread, NULL, NULL, NULL,
                  RELAIS_PRIO_CTL, 0, K_NO_WAIT);
  k_thread_create(&core_tid, core_stack, STACK_CORE, core_thread, NULL, NULL, NULL,
                  RELAIS_PRIO_CORE, 0, K_NO_WAIT);
  k_thread_create(&net_tid, net_stack, STACK_NET, net_thread, NULL, NULL, NULL,
                  RELAIS_PRIO_NET, 0, K_NO_WAIT);
  k_thread_name_set(&ctl_tid, "relais-ctl");
  k_thread_name_set(&core_tid, "relais-core");
  k_thread_name_set(&net_tid, "relais-net");
  LOG_INF("relais: three threads up on Zephyr, T=%u ms", (unsigned)g_params.period_ms);
  return 0;
}
