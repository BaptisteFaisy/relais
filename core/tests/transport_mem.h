/* Relais — a memory transport used by the unit tests of the transport
 * interface (mission M32). Static queues in a shared bus: no socket, no clock,
 * no allocation, exactly the contract of core/include/relais/transport.h.
 *
 * Three endpoints, indexed like the system: 0 = actuator, 1 = computer A,
 * 2 = computer B. Each endpoint declares where its two links send, and reads
 * its own inbox. `corrupt_next` is the test hook that flips one bit of the next
 * queued frame, so the MAC guard of the core can be exercised through the
 * interface (SEC-01, SEC-02, DET-04).
 *
 * Included by core/tests/test_transport.c only; it is not part of the library.
 */
#ifndef RELAIS_TESTS_TRANSPORT_MEM_H
#define RELAIS_TESTS_TRANSPORT_MEM_H

#include <string.h>

#include "relais/transport.h"

/* ------------------------------------------------- a memory transport ----- */

#define MEM_DEPTH 24u
#define MEM_ENDPOINTS 3u /* 0 = actuator, 1 = computer A, 2 = computer B */

typedef struct {
  uint16_t len;
  uint8_t buf[RELAIS_FRAME_MAX];
} mem_slot;

typedef struct {
  mem_slot q[MEM_DEPTH];
  size_t head;
  size_t count;
  uint32_t full_drops;
} mem_queue;

typedef struct {
  mem_queue q[MEM_ENDPOINTS][RELAIS_LINK_COUNT];
  bool corrupt_next; /* test hook: flip one byte of the next queued frame */
  size_t corrupt_at;
  uint32_t bytes;
} mem_bus;

typedef struct {
  mem_bus *bus;
  int self;                  /* our inbox index in the bus */
  int dest[RELAIS_LINK_COUNT]; /* where each link of this endpoint sends */
  bool closed;
  uint32_t sent;
  uint32_t timeouts;
} mem_ctx;

static mem_bus g_bus;

static int mem_init(void *ctx, const relais_tr_cfg *cfg) {
  mem_ctx *m = (mem_ctx *)ctx;
  if (m == NULL || cfg == NULL) return RELAIS_TR_ERR_CONFIG;
  if (cfg->actuator == NULL && cfg->peer == NULL) return RELAIS_TR_ERR_CONFIG;
  m->closed = false;
  return RELAIS_TR_OK;
}

static int mem_send(void *ctx, relais_link link, const uint8_t *frame, size_t len) {
  mem_ctx *m = (mem_ctx *)ctx;
  if (m == NULL || m->closed) return RELAIS_TR_CLOSED;
  if (frame == NULL || len == 0 || len > RELAIS_FRAME_MAX) return RELAIS_TR_ERR_FULL;
  int dest = m->dest[link];
  if (dest < 0) return RELAIS_TR_ERR_CONFIG;
  mem_queue *q = &m->bus->q[dest][link];
  if (q->count >= MEM_DEPTH) {
    q->full_drops++;
    return RELAIS_TR_ERR_FULL;
  }
  size_t at = (q->head + q->count) % MEM_DEPTH;
  memcpy(q->q[at].buf, frame, len);
  q->q[at].len = (uint16_t)len;
  q->count++;
  m->sent++;
  m->bus->bytes += (uint32_t)len;
  if (m->bus->corrupt_next && len > m->bus->corrupt_at) {
    q->q[at].buf[m->bus->corrupt_at] ^= 0x40u; /* one bit: the MAC must see it */
    m->bus->corrupt_next = false;
  }
  return RELAIS_TR_OK;
}

/* Deterministic: link 0 is drained before link 1, oldest frame first. A frame
 * that does not fit stays queued — a transport never eats a frame silently. */
static int mem_recv(void *ctx, relais_link *link, uint8_t *buf, size_t cap, size_t *len) {
  mem_ctx *m = (mem_ctx *)ctx;
  if (m == NULL || m->closed) return RELAIS_TR_CLOSED;
  for (int l = 0; l < RELAIS_LINK_COUNT; l++) {
    mem_queue *own = &m->bus->q[m->self][l];
    if (own->count == 0) continue;
    if ((size_t)own->q[own->head].len > cap) return RELAIS_TR_ERR_FULL;
    memcpy(buf, own->q[own->head].buf, own->q[own->head].len);
    *len = own->q[own->head].len;
    own->head = (own->head + 1) % MEM_DEPTH;
    own->count--;
    if (link != NULL) *link = (relais_link)l;
    return RELAIS_TR_OK;
  }
  m->timeouts++;
  return RELAIS_TR_TIMEOUT;
}

static void mem_close(void *ctx) {
  mem_ctx *m = (mem_ctx *)ctx;
  if (m != NULL) m->closed = true;
}

static relais_transport mem_make(mem_ctx *m, mem_bus *bus, int self, int actuator, int peer) {
  relais_transport t;
  memset(m, 0, sizeof *m);
  m->bus = bus;
  m->self = self;
  m->dest[RELAIS_LINK_ACTUATOR] = actuator;
  m->dest[RELAIS_LINK_PEER] = peer;
  t.name = "memory";
  t.ctx = m;
  t.init = mem_init;
  t.send = mem_send;
  t.recv = mem_recv;
  t.close = mem_close;
  return t;
}
static relais_transport make_endpoint(mem_ctx *m, int self, int actuator, int peer) {
  relais_tr_cfg cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.actuator = "mem:act";
  cfg.peer = "mem:peer";
  cfg.timeout_ms = 0;
  relais_transport t = mem_make(m, &g_bus, self, actuator, peer);
  (void)relais_tr_init(&t, &cfg);
  return t;
}

/* The three endpoints of the system on one fresh bus, ready to be plugged into
 * a world_t: 0 = actuator, 1 = computer A, 2 = computer B. Both transport tests
 * use this, so the wiring exists once. */
static mem_ctx g_mem_ctx[3];

static void wire_memory_endpoints(relais_transport tr[3]) {
  memset(&g_bus, 0, sizeof g_bus);
  memset(g_mem_ctx, 0, sizeof g_mem_ctx);
  tr[0] = make_endpoint(&g_mem_ctx[0], 0, 1, 2);
  tr[1] = make_endpoint(&g_mem_ctx[1], 1, 0, 2);
  tr[2] = make_endpoint(&g_mem_ctx[2], 2, 0, 1);
}

#endif /* RELAIS_TESTS_TRANSPORT_MEM_H */
