/* Relais — the internals of the comparison world (mission M43).
 *
 * The world is the part of the harness that is shared by the three
 * implementations: the two computers, the actuator, the links, the plant,
 * the fault schedule and the channel model. It is separated from the tick
 * loop (harness.c) so that each file stays small and each function short.
 */
#ifndef RELAIS_BASELINE_WORLD_H
#define RELAIS_BASELINE_WORLD_H

#include "harness.h"
#include "proto.h"

#define BL_QUEUE_MAX 128u
#define BL_MSGS_MAX 16u

/* The faults of one computer, as the bench sees them. */
typedef struct {
  bool dead;          /* F1: the machine stops dead */
  uint32_t frozen_until; /* F2: frozen until that tick */
  bool cut_act;       /* F5: its frames never reach the actuator */
  bool deaf;          /* it no longer hears the actuator */
  bool cut_peer;      /* F4: its frames never reach the peer */
  uint32_t slow_every; /* F3: one tick out of slow_every is lost */
} bl_nfault;

/* A frame on its way to the actuator. */
typedef struct {
  relais_msg msg;
  uint32_t deliver_at;
  bool used;
} bl_pending;

typedef struct {
  bl_config cfg;
  const bl_impl *impl;
  relais_params params;
  relais_traj traj;
  bl_act act;
  bl_node node[BL_NODES];
  bl_nfault nf[BL_NODES];
  bl_pending queue[BL_QUEUE_MAX];
  uint32_t qcount;
  uint64_t rng;
  uint64_t *draws;   /* one draw per (tick, computer), drawn before the run */
  bool lost_now[BL_NODES];
  bool lost_prev[BL_NODES];
  relais_plant plant;
  uint8_t commander; /* the commander of the previous tick */
  bool twin;         /* the fault free reference */
} bl_world;

/* What one tick of the world produced. */
typedef struct {
  float value;
  bool fresh;
  uint8_t fresh_node;
  uint8_t commander;
  bool commands[BL_NODES];
  float command[BL_NODES];
  float pos;
  float setpoint;
} bl_tick;

bl_world *bl_world_new(const bl_config *cfg);
void bl_world_free(bl_world *w);
/* The faults of this tick are applied to the state of the computers. */
void bl_world_faults(bl_world *w, uint32_t tick);
/* The channel decisions of this tick: the losses, the delays, the copies. */
void bl_world_channel(bl_world *w, uint32_t tick);
/* The frames the actuator receives at this tick. */
size_t bl_world_collect(bl_world *w, uint32_t tick, relais_msg *out, size_t cap);
/* A frame towards the actuator: one tick of pipeline, plus the drawn delay. */
void bl_world_send(bl_world *w, int idx, const relais_msg *m, uint32_t tick, bool same_tick);
/* A frame towards the peer: same tick, unless the link is cut or lossy. */
size_t bl_world_peer(bl_world *w, int idx, const bl_node_out *out, relais_msg *buf);
float bl_world_setpoint(const bl_world *w, uint32_t tick);
bool bl_world_handover_at(const bl_world *w, uint32_t tick, int idx);

#endif /* RELAIS_BASELINE_WORLD_H */
