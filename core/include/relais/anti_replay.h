/* Relais — sliding-window anti-replay (SEC-03, idea 33).
 *
 * The tick label already rejects an order that targets another tick than the one
 * being applied (SEC-03). This module is the second, per-emitter line of defence:
 * it remembers the ticks a given sender has already been accepted for, so a frame
 * captured and replayed inside the window is refused even when the tick label
 * still matches — the duplicated frame of a lossy link (ROB-02) or a capture
 * replayed in the same tick.
 *
 * One window per emitter: the highest tick seen plus a bitmap of the ticks below
 * it. Fixed size, no allocation, no clock: the window is measured in ticks, which
 * the actuator owns.
 */
#ifndef RELAIS_ANTI_REPLAY_H
#define RELAIS_ANTI_REPLAY_H

#include "relais/types.h"

/* Default window: 64 ticks, i.e. 64 * 5 ms = 320 ms of link memory at T = 5 ms,
 * far more than the 2*delta + C + J a frame may legally be late by. */
#define RELAIS_REPLAY_WINDOW 64u
/* One window per node value of relais_node. NODE_NONE is inside the array so a
 * bad sender value can never index outside it, but it is not an emitter: its
 * slot stays empty and its frames are refused. */
#define RELAIS_REPLAY_SLOTS 5u

typedef struct {
  uint64_t bitmap;   /* bit i set = tick (highest - i) already accepted */
  relais_tick highest;
  bool started;
  uint32_t bits;     /* window width in ticks, 1..64 */
  uint32_t accepted;
  uint32_t rejected_replay; /* a tick already seen inside the window */
  uint32_t rejected_old;    /* a tick that fell out of the window */
} relais_replay;

void relais_replay_init(relais_replay *w, uint32_t bits);
void relais_replay_reset(relais_replay *w);
/* True when the tick would be accepted, without touching the window. */
bool relais_replay_would_accept(const relais_replay *w, relais_tick tick);
/* Record the tick. Returns false when it is a replay or too old. */
bool relais_replay_accept(relais_replay *w, relais_tick tick);

/* The windows of one receiver, indexed by emitter. */
typedef struct {
  relais_replay slot[RELAIS_REPLAY_SLOTS];
  uint32_t bits;
} relais_replay_set;

void relais_replay_set_init(relais_replay_set *s, uint32_t bits);
/* The window of one emitter, or NULL when the node cannot emit (NONE, or a
 * value outside relais_node). */
relais_replay *relais_replay_set_slot(relais_replay_set *s, relais_node who);
bool relais_replay_set_accept(relais_replay_set *s, relais_node who, relais_tick tick);

#endif /* RELAIS_ANTI_REPLAY_H */
