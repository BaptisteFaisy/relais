/* Relais on FreeRTOS — one computer, three tasks (mission M32, idea 73).
 *
 * Same split as the QNX image and as the Zephyr port: net owns the socket, core
 * owns the protocol state machine, ctl owns the control law. The core is the
 * shared one, compiled a fifth time, and nothing is re-implemented here.
 *
 * Priority mapping. QNX SCHED_FIFO: lower is stronger (net 62, core 61, ctl 60).
 * FreeRTOS: higher is stronger, so the port applies one rule:
 *
 *     freertos_prio(qnx) = configMAX_PRIORITIES - 1 - (qnx - 60)
 *     60 -> 7 (ctl), 61 -> 6 (core), 62 -> 5 (net)      with configMAX_PRIORITIES = 8
 *
 * Ordering preserved, and all three are above any application task.
 *
 * NO DYNAMIC ALLOCATION: configSUPPORT_STATIC_ALLOCATION is 1 and
 * configSUPPORT_DYNAMIC_ALLOCATION is 0, so every queue and every task buffer
 * below is a static object. The core's "no allocation" rule is therefore true of
 * the whole image, not just of core/ (QNX-01, SEC-05, CMD-04).
 *
 * NOT COMPILED HERE: no FreeRTOS kernel source on this machine. Delivered with
 * its build files, the exact commands and the adaptation list in
 * docs/PORT-FREERTOS.md; `make -C ports/freertos check` reports what is missing
 * and exits 0.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include "relais/node.h"
#include "relais/params.h"
#include "relais_port.h"
#include "udp.h"

#define RELAIS_PRIO_CTL  (configMAX_PRIORITIES - 1) /* QNX 60 */
#define RELAIS_PRIO_CORE (configMAX_PRIORITIES - 2) /* QNX 61 */
#define RELAIS_PRIO_NET  (configMAX_PRIORITIES - 3) /* QNX 62 */

#define STACK_NET  (512u)  /* words, like every FreeRTOS stack size */
#define STACK_CORE (1024u)
#define STACK_CTL  (512u)
#define QUEUE_DEPTH (8u)

/* The RPC between the core and the law: one request in flight, so a pair of
 * depth-1 queues is a lock-step rendez-vous and needs no mutex. */
typedef struct {
  relais_meas meas;
  bool want_state; /* REP-01: the core asks for the state to replicate */
  bool import;     /* REP-02: the core pushes a state back */
  uint8_t state[RELAIS_CTL_STATE_MAX];
  uint8_t state_len;
} ctl_req_t;

typedef struct {
  float value;
  uint8_t state[RELAIS_CTL_STATE_MAX];
  uint8_t state_len;
} ctl_rep_t;

static StaticQueue_t q_net_to_core_buf, q_core_to_net_buf, q_req_buf, q_rep_buf;
static uint8_t q_net_to_core_mem[QUEUE_DEPTH * sizeof(relais_msg)];
static uint8_t q_core_to_net_mem[QUEUE_DEPTH * sizeof(relais_msg)];
static uint8_t q_req_mem[sizeof(ctl_req_t)];
static uint8_t q_rep_mem[sizeof(ctl_rep_t)];
static QueueHandle_t net_to_core, core_to_net, ctl_requests, ctl_replies;

static StaticTask_t tcb_net, tcb_core, tcb_ctl;
static StackType_t stack_net[STACK_NET], stack_core[STACK_CORE], stack_ctl[STACK_CTL];

static relais_params g_params;
static relais_key g_key;
static relais_udp g_udp;
static relais_transport g_transport;
static relais_node_sm g_node;
static relais_node g_self = RELAIS_NODE_A;

/* ----------------------------------------------------------- relais-ctl ---- */

static void ctl_task(void *arg) {
  (void)arg;
  relais_ctl_state state;
  relais_ctl_init(&state, &g_params);
  for (;;) {
    ctl_req_t req;
    if (xQueueReceive(ctl_requests, &req, portMAX_DELAY) != pdTRUE) continue;
    ctl_rep_t rep;
    memset(&rep, 0, sizeof rep);
    if (req.import) {
      (void)relais_ctl_import(&state, req.state, req.state_len);
    } else if (req.want_state) {
      rep.state_len = relais_ctl_export(&state, rep.state, sizeof rep.state);
      rep.value = state.last_value;
    } else {
      /* CMD-06: bounded here, before the value ever reaches the wire. */
      rep.value = relais_clamp_value(relais_ctl_step(&state, &req.meas, &g_params), &g_params);
      rep.state_len = relais_ctl_export(&state, rep.state, sizeof rep.state);
    }
    (void)xQueueSend(ctl_replies, &rep, portMAX_DELAY);
  }
}

/* The relais_law hooks of the core, answered by the ctl task (CMD-09). */
static void law_init(void *ctx, const relais_params *p) { (void)ctx; (void)p; }

static float law_step(void *ctx, const relais_meas *m, const relais_params *p) {
  (void)ctx; (void)p;
  ctl_req_t req;
  ctl_rep_t rep;
  memset(&req, 0, sizeof req);
  memset(&rep, 0, sizeof rep);
  req.meas = *m;
  if (xQueueSend(ctl_requests, &req, portMAX_DELAY) != pdTRUE) return 0.0f;
  if (xQueueReceive(ctl_replies, &rep, portMAX_DELAY) != pdTRUE) return 0.0f;
  return rep.value;
}

static uint8_t law_export(void *ctx, uint8_t *buf, size_t cap) {
  (void)ctx;
  ctl_req_t req;
  ctl_rep_t rep;
  memset(&req, 0, sizeof req);
  memset(&rep, 0, sizeof rep);
  req.want_state = true;
  if (xQueueSend(ctl_requests, &req, portMAX_DELAY) != pdTRUE) return 0;
  if (xQueueReceive(ctl_replies, &rep, portMAX_DELAY) != pdTRUE) return 0;
  size_t n = rep.state_len < cap ? rep.state_len : cap;
  for (size_t i = 0; i < n; i++) buf[i] = rep.state[i];
  return (uint8_t)n;
}

static bool law_import(void *ctx, const uint8_t *buf, size_t len) {
  (void)ctx;
  ctl_req_t req;
  ctl_rep_t rep;
  memset(&req, 0, sizeof req);
  memset(&rep, 0, sizeof rep);
  req.import = true;
  req.state_len = len < sizeof req.state ? (uint8_t)len : (uint8_t)sizeof req.state;
  for (size_t i = 0; i < req.state_len; i++) req.state[i] = buf[i];
  if (xQueueSend(ctl_requests, &req, portMAX_DELAY) != pdTRUE) return false;
  return xQueueReceive(ctl_replies, &rep, portMAX_DELAY) == pdTRUE;
}

static const relais_law g_law = {.ctx = NULL,
                                 .init = law_init,
                                 .step = law_step,
                                 .export_state = law_export,
                                 .import_state = law_import};

/* ----------------------------------------------------------- relais-net ---- */

/* The only task that touches the socket (SEC-05). A frame that fails the shape
 * or the authenticator check dies here and is counted (DET-04, SEC-01). */
static void net_task(void *arg) {
  (void)arg;
  uint8_t buf[RELAIS_FRAME_MAX];
  uint8_t scratch[RELAIS_FRAME_MAX];
  for (;;) {
    relais_link link = RELAIS_LINK_ACTUATOR;
    size_t len = 0;
    if (relais_tr_recv_raw(&g_transport, &link, buf, sizeof buf, &len) == RELAIS_TR_OK) {
      relais_msg msg;
      relais_reject why = RELAIS_REJ_NONE;
      if (relais_frame_decode(buf, len, &g_key, &msg, &why))
        (void)xQueueSend(net_to_core, &msg, 0);
    }
    relais_msg out;
    while (xQueueReceive(core_to_net, &out, 0) == pdTRUE)
      (void)relais_tr_send_msg(&g_transport, &g_key, &out, scratch, sizeof scratch);
  }
}

/* ---------------------------------------------------------- relais-core ---- */

static void core_task(void *arg) {
  (void)arg;
  relais_node_init(&g_node, &g_params, g_self);
  relais_node_set_law(&g_node, &g_law); /* CMD-09 */
  const TickType_t bail = pdMS_TO_TICKS(g_params.period_ms * 3u);
  for (;;) {
    relais_msg msg;
    relais_node_out out;
    relais_node_clear_out(&out);
    if (xQueueReceive(net_to_core, &msg, bail) != pdTRUE)
      relais_node_on_silence(&g_node, g_node.now + 1, &out); /* MAN-09 */
    else
      (void)relais_port_dispatch(&g_node, &msg, &out);
    for (uint8_t i = 0; i < out.count; i++)
      (void)xQueueSend(core_to_net, &out.out[i], portMAX_DELAY);
  }
}

/* ------------------------------------------------------------------ main --- */

int main(void) {
  relais_params_default(&g_params);
  relais_key_clear(&g_key);
  relais_udp_attach(&g_transport, &g_udp);

  relais_tr_cfg cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.actuator = RELAIS_ACTUATOR_ENDPOINT; /* from the build, see CMakeLists.txt */
  cfg.peer = RELAIS_PEER_ENDPOINT;
  cfg.local = RELAIS_LOCAL_ENDPOINT;
  cfg.timeout_ms = g_params.period_ms;
  if (relais_tr_init(&g_transport, &cfg) != RELAIS_TR_OK) return 1;

  net_to_core = xQueueCreateStatic(QUEUE_DEPTH, sizeof(relais_msg), q_net_to_core_mem,
                                   &q_net_to_core_buf);
  core_to_net = xQueueCreateStatic(QUEUE_DEPTH, sizeof(relais_msg), q_core_to_net_mem,
                                   &q_core_to_net_buf);
  ctl_requests = xQueueCreateStatic(1, sizeof(ctl_req_t), q_req_mem, &q_req_buf);
  ctl_replies = xQueueCreateStatic(1, sizeof(ctl_rep_t), q_rep_mem, &q_rep_buf);

  (void)xTaskCreateStatic(ctl_task, "relais-ctl", STACK_CTL, NULL, RELAIS_PRIO_CTL,
                          stack_ctl, &tcb_ctl);
  (void)xTaskCreateStatic(core_task, "relais-core", STACK_CORE, NULL, RELAIS_PRIO_CORE,
                          stack_core, &tcb_core);
  (void)xTaskCreateStatic(net_task, "relais-net", STACK_NET, NULL, RELAIS_PRIO_NET,
                          stack_net, &tcb_net);

  vTaskStartScheduler();
  return 0; /* the scheduler does not return */
}
