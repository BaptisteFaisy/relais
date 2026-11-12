/* Relais — sliding-window anti-replay (SEC-03, idea 33). See anti_replay.h.
 *
 * Pure C11: no clock, no file, no socket, no allocation. The window is one
 * 64-bit bitmap plus the highest tick accepted, so its memory cost is constant
 * and its verdict does not depend on the order the frames arrive in.
 */
#include "relais/anti_replay.h"

#include <string.h>

static uint32_t clamp_bits(uint32_t bits) {
  if (bits == 0u || bits > RELAIS_REPLAY_WINDOW) return RELAIS_REPLAY_WINDOW;
  return bits;
}

void relais_replay_init(relais_replay *w, uint32_t bits) {
  if (w == NULL) return;
  memset(w, 0, sizeof *w);
  w->bits = clamp_bits(bits);
}

void relais_replay_reset(relais_replay *w) {
  if (w == NULL) return;
  uint32_t bits = w->bits;
  relais_replay_init(w, bits);
}

/* Locate a tick relative to the window: *mask is its bit when it falls inside,
 * *too_old says it fell out on the old side. The return value is true when the
 * tick is already recorded, i.e. when it is a replay. One classification, used
 * by both the read-only and the recording verdict (no duplicated logic). */
static bool replay_classify(const relais_replay *w, relais_tick tick, uint64_t *mask,
                            bool *too_old) {
  *mask = 0u;
  *too_old = false;
  if (!w->started) return false;      /* empty window: nothing was seen yet */
  if (tick > w->highest) return false; /* ahead of the window: always new */
  uint64_t delta = w->highest - tick;
  if (delta >= w->bits) {
    *too_old = true;
    return false;
  }
  *mask = (uint64_t)1 << delta;
  return (w->bitmap & *mask) != 0u;
}

bool relais_replay_would_accept(const relais_replay *w, relais_tick tick) {
  if (w == NULL) return false;
  uint64_t mask = 0u;
  bool too_old = false;
  bool seen = replay_classify(w, tick, &mask, &too_old);
  return !seen && !too_old;
}

bool relais_replay_accept(relais_replay *w, relais_tick tick) {
  if (w == NULL) return false;
  uint64_t mask = 0u;
  bool too_old = false;
  bool seen = replay_classify(w, tick, &mask, &too_old);
  if (seen) {
    w->rejected_replay++;
    return false;
  }
  if (too_old) {
    w->rejected_old++;
    return false;
  }
  if (!w->started) {
    w->started = true;
    w->highest = tick;
    w->bitmap = 1u;
  } else if (tick > w->highest) {
    /* The window slides forward: what was too far behind falls out. */
    uint64_t delta = tick - w->highest;
    w->bitmap = (delta >= w->bits) ? 0u : (w->bitmap << delta);
    w->bitmap |= 1u;
    w->highest = tick;
  } else {
    w->bitmap |= mask; /* late but inside the window: accepted, now recorded */
  }
  w->accepted++;
  return true;
}

void relais_replay_set_init(relais_replay_set *s, uint32_t bits) {
  if (s == NULL) return;
  s->bits = clamp_bits(bits);
  for (size_t i = 0; i < RELAIS_REPLAY_SLOTS; i++)
    relais_replay_init(&s->slot[i], s->bits);
}

/* NODE_NONE has a slot so that no sender value can index outside the array, but
 * it is not an emitter: a frame that names nobody is refused before any window
 * is touched, and its verdict never pollutes the window of a real sender. */
relais_replay *relais_replay_set_slot(relais_replay_set *s, relais_node who) {
  if (s == NULL || who == RELAIS_NODE_NONE || (size_t)who >= RELAIS_REPLAY_SLOTS)
    return NULL;
  return &s->slot[who];
}

bool relais_replay_set_accept(relais_replay_set *s, relais_node who, relais_tick tick) {
  return relais_replay_accept(relais_replay_set_slot(s, who), tick);
}
