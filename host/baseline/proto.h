/* Relais — common interface of the measured comparison (mission M43).
 *
 * Three implementations of the SAME problem are driven by ONE harness
 * (host/baseline/harness.c): the Relais protocol itself (the real core, used
 * through core/src/arbiter.c and core/src/node.c), a heartbeat + watchdog
 * baseline and a lease + term baseline.
 *
 * Only the handover logic differs. The plant, the trajectory, the control law,
 * the channel model and the fault model are shared, so the numbers compare
 * protocols and not benches.
 *
 * Docs: docs/COMPARISON-MEASURED.md.
 */
#ifndef RELAIS_BASELINE_PROTO_H
#define RELAIS_BASELINE_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "relais/control.h"
#include "relais/frame.h"
#include "relais/params.h"

#define BL_NODES 2u      /* two computers, A and B */
#define BL_NONE 0xFFu    /* nobody */
#define BL_OUT_MAX 8u    /* frames a computer may emit in one tick */
#define BL_STATE_MAX 1024u /* bytes reserved for one implementation state */

/* Measurements published by the actuator to the two computers every tick.
 * This is the STATUT of Relais; the baselines read the same numbers, so all
 * three run the same control law on the same inputs (CMD-03). The type is
 * the one of the core: the baselines run the very law of core/src/control.c,
 * nothing is re-implemented here. */
typedef relais_meas bl_meas;

/* What the actuator publishes about the protocol state. */
typedef struct {
  uint32_t tick;
  uint8_t commander;    /* the computer the actuator obeys, BL_NONE if none */
  uint32_t term;        /* mandate number (Relais), lease term (lease) */
  uint32_t expiry;      /* lease expiry tick (lease baseline), 0 if unused */
  uint8_t misses;       /* ticks without a valid command (Relais evidence) */
  bool certified;       /* the second is certified (Relais) */
  uint8_t certified_node;
  bool heard;           /* did THIS computer hear the actuator this tick */
  bl_meas meas;
} bl_status;

/* What the actuator decided for this tick. */
typedef struct {
  float value;          /* the value applied */
  bool fresh;           /* it comes from a live computer's own command */
  uint8_t fresh_node;   /* which one, BL_NONE if not fresh */
  uint8_t commander;
  uint32_t term;
  uint32_t expiry;      /* the lease expiry tick, 0 when unused */
  uint8_t misses;
  bool certified;
  uint8_t certified_node;
} bl_act_out;

/* What a computer emits during one tick. */
typedef struct {
  relais_msg m[BL_OUT_MAX];
  uint8_t count;
  bool commands;  /* it believes it commands and sent an order */
  float command;  /* the value it commands */
} bl_node_out;

/* Opaque per-implementation state, owned by the harness. */
typedef union {
  uint8_t raw[BL_STATE_MAX];
  long double align_; /* keeps the storage aligned for any state struct */
} bl_state;

typedef struct {
  bl_state s;
} bl_act;

typedef struct {
  bl_state s;
} bl_node;

/* The interface every implementation honours. `timeout` is the only knob of
 * the two baselines: the watchdog timeout (heartbeat) or the lease duration
 * (lease), in ticks. Relais ignores it: its evidence is counted, not waited
 * for. */
typedef struct {
  const char *name;
  void (*act_init)(bl_act *a, const relais_params *p, uint32_t timeout);
  void (*act_step)(bl_act *a, uint32_t tick, const relais_msg *msgs, size_t n,
                   const bl_meas *m, bl_act_out *out);
  void (*act_counters)(const bl_act *a, uint32_t *out, size_t n);
  void (*node_init)(bl_node *nd, int idx, const relais_params *p, uint32_t timeout);
  void (*node_step)(bl_node *nd, int idx, const bl_status *st, const relais_msg *peer,
                    size_t n_peer, bl_node_out *out);
  void (*node_fault)(bl_node *nd, int idx, uint8_t reason, bl_node_out *out);
} bl_impl;

const bl_impl *bl_impl_relais(void);
const bl_impl *bl_impl_heartbeat(void);
const bl_impl *bl_impl_lease(void);
const bl_impl *bl_impl_by_name(const char *name);

#endif /* RELAIS_BASELINE_PROTO_H */
