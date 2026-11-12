/* Relais — counter-example reduction by delta-debugging (spec 7.3, idea 7).
 *
 * The exhaustive explorer stops at the FIRST state that breaches an invariant
 * and hands back the whole path from the root. That path is rarely the shortest
 * witness: it carries the frames, the faults and the ticks the search happened
 * to try first. This module takes such a path and removes events -- one at a
 * time, then by blocks -- as long as the SAME breach survives, so what is left
 * is a minimal scenario a human can actually read and the simulator can replay.
 *
 * The oracle is the explorer's own world: a reduced trace is kept only if
 * replaying it from a fresh world still breaches a safety invariant. Reduction
 * therefore cannot invent a violation, and cannot keep a trace that no longer
 * has one.
 */
#ifndef RELAIS_EXPLORE_SHRINK_H
#define RELAIS_EXPLORE_SHRINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "world.h"

/* Upper bound on the trace length the reducer buffers. The explorer's path is
 * at most MAX_DEPTH events; 256 leaves room for a deeper search. */
#define SHRINK_TMP_MAX 256

/* Replay a trace from a fresh world. Returns true as soon as a safety
 * invariant is breached; when it does, *report (if not NULL) holds the first
 * violation line, exactly like the explorer prints it. */
bool shrink_violates(const relais_params *p, const relais_traj *t, bool complete,
                     const explore_event *ev, size_t n, char *report, size_t cap);

/* Delta-debugging: greedily drop single events, then blocks of decreasing size,
 * while the breach persists. ev[0..n-1] is the input trace, out receives the
 * reduced one (out may alias ev). Returns the reduced length, <= n. */
size_t shrink_trace(const relais_params *p, const relais_traj *t, bool complete,
                    const explore_event *ev, size_t n, explore_event *out, size_t cap);

/* Write a trace as a replayable scenario in the format of CONTRACT.md: machine
 * faults and cuts go to "faults", channel moves to "channel". The tick of each
 * move is recovered by replaying the trace, so a reduced trace gets its own
 * (shorter) timeline. Used by the explorer and by test_shrink alike. */
void shrink_write_scenario(FILE *f, const relais_params *p, const relais_traj *t,
                           bool complete, const explore_event *ev, size_t n);

#endif /* RELAIS_EXPLORE_SHRINK_H */
