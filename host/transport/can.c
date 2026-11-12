/* Relais — the CAN transport: segmentation and reassembly (mission M32).
 *
 * Nothing here knows what a mandate, an order or a testament is: a Relais frame
 * goes on the bus as opaque bytes, split into 8-byte CAN frames with the
 * protocol byte of ISO 15765-2, and comes back whole or not at all. A segment
 * that is lost, out of sequence or interrupted is counted and the flow is
 * abandoned — the protocol above sees a silence, which is exactly what MAN-04
 * and DET-01 are written for (ROB-01: never invent a frame).
 */
#include "can.h"

#include <string.h>

/* Pull at most this many CAN frames per recv: enough for the largest Relais
 * frame (512 bytes -> 74 fragments) with room for foreign traffic, small enough
 * that a flooded bus cannot trap the caller in this loop (CMD-04). */
#define CAN_MAX_PULL 256u

uint32_t relais_can_fragment_count(size_t len) {
  if (len == 0) return 0u;
  if (len <= RELAIS_CAN_SF_MAX) return 1u;
  size_t rest = len - RELAIS_CAN_FF_MAX;
  uint32_t cf = (uint32_t)((rest + RELAIS_CAN_CF_MAX - 1u) / RELAIS_CAN_CF_MAX);
  return 1u + cf;
}

void relais_can_accept(relais_can *c, uint32_t id, relais_link link) {
  if (c == NULL || c->rx_filters >= RELAIS_CAN_RX_FILTERS) return;
  if ((unsigned)link >= (unsigned)RELAIS_LINK_COUNT) return;
  c->rx_id[c->rx_filters] = id;
  c->rx_link[c->rx_filters] = link;
  c->rx_filters++;
}

/* One CAN frame on the bus: `n` bytes of payload after the protocol byte. */
static int can_xmit(const relais_can *c, uint32_t id, const uint8_t *data, size_t n) {
  relais_can_frame f;
  memset(&f, 0, sizeof f);
  f.id = id;
  f.len = (uint8_t)(n > RELAIS_CAN_DLC ? RELAIS_CAN_DLC : n);
  if (n > 0) memcpy(f.data, data, f.len);
  if (c->bus.xmit(c->bus.ctx, &f) != RELAIS_TR_OK) return RELAIS_TR_ERR_IO;
  return RELAIS_TR_OK;
}

/* One Relais frame on the bus. Segmentation is synchronous: the whole frame is
 * emitted before the caller gets back, so two flows can never interleave on one
 * identifier. */
static int can_send(void *ctx, relais_link link, const uint8_t *frame, size_t len) {
  relais_can *c = (relais_can *)ctx;
  if (c == NULL || c->closed) return RELAIS_TR_CLOSED;
  if ((unsigned)link >= (unsigned)RELAIS_LINK_COUNT) return RELAIS_TR_ERR_CONFIG;
  if (frame == NULL || len == 0 || len > RELAIS_FRAME_MAX) return RELAIS_TR_ERR_FULL;
  uint32_t id = c->tx_id[link];
  uint8_t head[RELAIS_CAN_DLC];

  if (len <= RELAIS_CAN_SF_MAX) {
    head[0] = (uint8_t)(RELAIS_CAN_PCI_SF | (uint8_t)len);
    memcpy(&head[1], frame, len);
    if (can_xmit(c, id, head, len + 1u) != RELAIS_TR_OK) return RELAIS_TR_ERR_IO;
    c->tx_frames++;
    return RELAIS_TR_OK;
  }
  /* First frame: 12-bit total length, then the first 6 payload bytes. */
  head[0] = (uint8_t)(RELAIS_CAN_PCI_FF | (uint8_t)((len >> 8) & 0x0fu));
  head[1] = (uint8_t)(len & 0xffu);
  memcpy(&head[2], frame, RELAIS_CAN_FF_MAX);
  if (can_xmit(c, id, head, RELAIS_CAN_DLC) != RELAIS_TR_OK) return RELAIS_TR_ERR_IO;
  c->tx_frames++;

  size_t at = RELAIS_CAN_FF_MAX;
  uint8_t seq = 0;
  while (at < len) {
    size_t n = len - at < RELAIS_CAN_CF_MAX ? len - at : RELAIS_CAN_CF_MAX;
    head[0] = (uint8_t)(RELAIS_CAN_PCI_CF | (uint8_t)(seq & RELAIS_CAN_SEQ_MASK));
    memcpy(&head[1], frame + at, n);
    if (can_xmit(c, id, head, n + 1u) != RELAIS_TR_OK) return RELAIS_TR_ERR_IO;
    c->tx_frames++;
    at += n;
    seq = (uint8_t)((seq + 1u) & RELAIS_CAN_SEQ_MASK);
  }
  return RELAIS_TR_OK;
}

/* Which reassembly channel an identifier belongs to, or -1 for foreign traffic. */
static int rx_filter(const relais_can *c, uint32_t id) {
  for (uint8_t i = 0; i < c->rx_filters; i++)
    if (c->rx_id[i] == id) return (int)i;
  return -1;
}

static void rx_abort(relais_can *c, relais_can_flow *f) {
  f->active = false;
  f->len = 0;
  f->total = 0;
  c->lost_flows++;
}

/* Single frame: the whole Relais frame fits in one CAN payload. */
static int rx_single(relais_can *c, relais_can_flow *f, const relais_can_frame *frame) {
  size_t payload = frame->len > 0 ? (size_t)frame->len - 1u : 0u;
  if (f->active) rx_abort(c, f); /* a new frame interrupts an unfinished one */
  if (payload == 0 || payload > RELAIS_CAN_SF_MAX) {
    c->dropped_pci++;
    return RELAIS_TR_TIMEOUT;
  }
  memcpy(f->buf, &frame->data[1], payload);
  f->len = payload;
  f->active = false;
  c->delivered++;
  return RELAIS_TR_OK;
}

/* First frame of a segmented transfer: 12-bit total length, then 6 bytes. */
static int rx_first(relais_can *c, relais_can_flow *f, const relais_can_frame *frame) {
  if (f->active) rx_abort(c, f);
  if (frame->len < 3) {
    c->dropped_pci++;
    return RELAIS_TR_TIMEOUT;
  }
  size_t total = (size_t)(((uint32_t)(frame->data[0] & 0x0fu) << 8) | (uint32_t)frame->data[1]);
  if (total <= RELAIS_CAN_SF_MAX || total > RELAIS_FRAME_MAX) {
    c->dropped_pci++;
    return RELAIS_TR_TIMEOUT;
  }
  memcpy(f->buf, &frame->data[2], RELAIS_CAN_FF_MAX);
  f->len = RELAIS_CAN_FF_MAX;
  f->total = total;
  f->seq = 0;
  f->active = true;
  return RELAIS_TR_TIMEOUT;
}

/* Consecutive frame: the sequence number must follow, or the flow is gone. */
static int rx_consecutive(relais_can *c, relais_can_flow *f, const relais_can_frame *frame) {
  if ((frame->data[0] & RELAIS_CAN_PCI_MASK) != RELAIS_CAN_PCI_CF || !f->active) {
    c->dropped_pci++;
    return RELAIS_TR_TIMEOUT;
  }
  if ((uint8_t)(frame->data[0] & RELAIS_CAN_SEQ_MASK) != f->seq) {
    rx_abort(c, f); /* out of sequence: say so, never patch it up */
    c->dropped_pci++;
    return RELAIS_TR_TIMEOUT;
  }
  size_t payload = frame->len > 0 ? (size_t)frame->len - 1u : 0u;
  size_t room = f->total - f->len;
  size_t n = payload < room ? payload : room;
  memcpy(f->buf + f->len, &frame->data[1], n);
  f->len += n;
  f->seq = (uint8_t)((f->seq + 1u) & RELAIS_CAN_SEQ_MASK);
  if (f->len < f->total) return RELAIS_TR_TIMEOUT;
  f->active = false;
  c->delivered++;
  return RELAIS_TR_OK;
}

/* One fragment of one channel. RELAIS_TR_OK when a whole Relais frame is ready
 * in f->buf, RELAIS_TR_TIMEOUT while it is still being reassembled. */
static int rx_fragment(relais_can *c, relais_can_flow *f, const relais_can_frame *frame) {
  switch (frame->data[0] & RELAIS_CAN_PCI_MASK) {
    case RELAIS_CAN_PCI_SF: return rx_single(c, f, frame);
    case RELAIS_CAN_PCI_FF: return rx_first(c, f, frame);
    default: return rx_consecutive(c, f, frame);
  }
}

static int can_recv(void *ctx, relais_link *link, uint8_t *buf, size_t cap, size_t *len) {
  relais_can *c = (relais_can *)ctx;
  if (c == NULL || c->closed) return RELAIS_TR_CLOSED;
  if (buf == NULL || len == NULL || cap == 0) return RELAIS_TR_ERR_CONFIG;
  for (uint32_t pulled = 0; pulled < CAN_MAX_PULL; pulled++) {
    relais_can_frame f;
    memset(&f, 0, sizeof f);
    if (c->bus.recv(c->bus.ctx, &f) != RELAIS_TR_OK) return RELAIS_TR_TIMEOUT;
    int idx = rx_filter(c, f.id);
    if (idx < 0) {
      c->dropped_id++; /* another node's traffic: a shared bus is public */
      continue;
    }
    c->rx_frames++;
    if (rx_fragment(c, &c->rx[idx], &f) != RELAIS_TR_OK) continue;
    if (c->rx[idx].len > cap) return RELAIS_TR_ERR_FULL; /* kept, not eaten */
    memcpy(buf, c->rx[idx].buf, c->rx[idx].len);
    *len = c->rx[idx].len;
    if (link != NULL) *link = c->rx_link[idx];
    return RELAIS_TR_OK;
  }
  return RELAIS_TR_TIMEOUT;
}

static void can_close(void *ctx) {
  relais_can *c = (relais_can *)ctx;
  if (c != NULL) c->closed = true;
}

/* Checked at init, once the caller has installed its receive filter with
 * relais_can_accept(): a bus with nothing to listen to is a configuration bug,
 * not a runtime surprise. */
static int can_init(void *ctx, const relais_tr_cfg *cfg) {
  relais_can *c = (relais_can *)ctx;
  if (c == NULL || c->bus.xmit == NULL || c->bus.recv == NULL) return RELAIS_TR_ERR_CONFIG;
  if (cfg != NULL && cfg->mtu != 0 && cfg->mtu < RELAIS_CAN_DLC) return RELAIS_TR_ERR_CONFIG;
  if (c->rx_filters == 0) return RELAIS_TR_ERR_CONFIG;
  for (int l = 0; l < RELAIS_LINK_COUNT; l++)
    if (c->tx_id[l] > RELAIS_CAN_ID_MAX) return RELAIS_TR_ERR_CONFIG;
  for (uint8_t i = 0; i < c->rx_filters; i++)
    if (c->rx_id[i] > RELAIS_CAN_ID_MAX) return RELAIS_TR_ERR_CONFIG;
  c->closed = false;
  return RELAIS_TR_OK;
}

/* The identifiers are given here: on CAN a lower identifier wins the
 * arbitration, so the actuator link is meant to get the lower one. */
void relais_can_attach(relais_transport *t, relais_can *c, const relais_can_bus *bus,
                       uint32_t id_actuator, uint32_t id_peer) {
  if (t == NULL || c == NULL) return;
  memset(c, 0, sizeof *c);
  if (bus != NULL) c->bus = *bus;
  c->tx_id[RELAIS_LINK_ACTUATOR] = id_actuator;
  c->tx_id[RELAIS_LINK_PEER] = id_peer;
  memset(t, 0, sizeof *t);
  t->name = "can";
  t->ctx = c;
  t->init = can_init;
  t->send = can_send;
  t->recv = can_recv;
  t->close = can_close;
}

