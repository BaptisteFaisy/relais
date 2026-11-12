/* Relais — symbolic (quotient) exploration of the world of host/explore.
 *
 * explore.c hashes the whole world, so a state is a concrete point in a very
 * high-dimensional space: the plant position, the filter taps of both computers,
 * the exact bytes of every frame in flight, ... Two points that differ only in
 * a float that no safety invariant ever reads are explored twice, and the DFS
 * runs out of budget long before it runs out of protocol situations.
 *
 * This module collapses those points into EQUVALENCE CLASSES and explores the
 * quotient. The class of a world is a small, discrete signature -- the protocol
 * facts the safety invariants of spec 7.2 actually read -- so the same budget
 * reaches much deeper ticks and covers a much larger share of the reachable
 * protocol behaviour.
 *
 * WHY IT IS SAFE FOR I1,I2,I3,I6,I8,I10,I11,DET-03 (see also docs/EXPLORATION.md)
 *
 *   A quotient never masks a violation iff every field those invariants read --
 *   now AND in every future tick -- is either kept whole, or is provably
 *   irrelevant to their verdict. The projection below keeps:
 *
 *     * the full invariant accumulator w->inv: it IS the memory of every
 *       safety check (counters, last mandate/holder/lamp, testament run,
 *       fallback timers). A state that has already breached an invariant is
 *       never collapsed onto a clean one, and a state whose lamp has been off
 *       for two ticks is never collapsed onto one whose lamp just went off.
 *     * the full arbiter w->arb: the actuator is the only writer of the mandate
 *       and the only judge of the evidence (MAN-02/MAN-04). Its whole state --
 *       mandate, holder, misses, certification streak, testament horizon, lamp,
 *       cession_pending, liveness table -- determines the next decision, and
 *       hence the next tick record that feeds the accumulator.
 *     * the tick number w->tick: I11 (contiguity) and every "last tick seen"
 *       comparison are absolute.
 *     * a discrete projection of each computer (role, believed mandate, the
 *       mandate and holder it last saw, when it last heard the actuator and its
 *       peer, its replication age, its silence counter, its ready/deposed
 *       flags): these gate WHICH frame a computer emits and with which tick and
 *       mandate, i.e. everything the arbiter uses to decide a miss, a grant or
 *       a revocation (DET-03, I2, I3).
 *     * the fault and channel state (dead, frozen_until, cut, channel_link,
 *       burst_this_tick, pending_mutations): they decide which moves the
 *       adversary may still play, so two worlds with a different fault budget
 *       left are never the same class.
 *     * the liveness of the frames in flight (which slot is busy, when it
 *       delivers): whether an order will reach the actuator for tick n+1 is a
 *       discrete fact, and I1/I6/I8/I10 read exactly that.
 *
 *   WHAT IS DELIBERATELY FORGOTTEN, and why it cannot hide a breach:
 *     * the plant (pos/vel) and the trajectory: they only ever reach the safety
 *       checks through the VALUE a machine computes. No invariant in the list
 *       reads a value -- I4/I5 (the value-continuity ones) are explicitly out of
 *       scope here. The setpoint is a pure function of the tick, identical in
 *       every state of one run.
 *     * the float payload of the frames in flight (order value, testament
 *       contents, STATUT measurement, ETAT filter bytes): same reason. The
 *       arbiter reduces the order/shadow comparison to the DISCRETE
 *       certify_streak / disagreement / shadow_misses, all kept inside w->arb.
 *     * the control-law filter taps and the diagnostic counters of the
 *       computers: they only move future values, never a future verdict of a
 *       listed invariant.
 *
 *   RESIDUAL RISK (documented, tested, not hidden): dropping the exact floats
 *   means two worlds of one class could, several ticks later, disagree about
 *   whether an order matched its shadow and so about a FUTURE certification.
 *   The projection keeps the certification state as the actuator holds it after
 *   every tick, so the risk is confined to a divergence that has not yet
 *   happened; test_symbolic checks on every mutant that the quotient still
 *   reaches every violation the brute-force search reaches on the same budget.
 */
#ifndef RELAIS_EXPLORE_SYMBOLIC_H
#define RELAIS_EXPLORE_SYMBOLIC_H

#include <stdint.h>

#include "world.h"

/* One computer, reduced to the discrete protocol facts that decide which frame
 * it will emit and whether the arbiter will accept it. Everything float is
 * dropped on purpose (see the file header). */
typedef struct {
  relais_role role;
  relais_node seen_holder;
  relais_mandate mandate;
  relais_mandate seen_mandate;
  relais_tick last_statut;
  relais_tick last_etat;
  uint8_t replica_age;
  uint8_t bail_ticks;
  uint8_t link_lost_ticks;
  bool ready;
  bool link_lost;
  bool deposed;
  bool without_actuator;
} sym_node;

/* A frame in flight, reduced to its liveness: is a slot occupied, and for which
 * tick does it deliver. The payload floats are dropped (see the file header). */
typedef struct {
  bool busy;
  uint32_t deliver_at;
} sym_slot;

/* The equivalence class of a world: the protocol signature the quotient search
 * hashes and compares instead of the whole state. */
typedef struct {
  uint32_t tick;
  relais_source source;      /* where the value of the last tick came from */
  relais_arb arb;            /* the actuator's whole protocol state, kept */
  relais_inv inv;            /* the invariant accumulator, kept whole */
  sym_node node[EXPLORE_NODES];
  sym_slot slot[EXPLORE_LINKS][EXPLORE_SLOTS];
  bool dead[EXPLORE_NODES];
  uint32_t frozen_until[EXPLORE_NODES];
  bool cut[EXPLORE_LINKS];
  int8_t channel_link;
  uint8_t burst_this_tick;
  uint8_t pending_mutations;
} sym_key;

/* Project a world onto its equivalence class. The key is byte-comparable: it
 * is zero-initialised, so padding never makes two equal worlds differ. */
void sym_project(const explore_world *w, sym_key *k);

/* Hash of an equivalence class. Equal classes hash equal; the projection is
 * what makes the quotient, so this replaces explore_world_hash when symbolic. */
uint64_t sym_hash(const explore_world *w);

/* How many discrete facts the class keeps, for the report and the tests. */
size_t sym_key_bytes(void);

#endif /* RELAIS_EXPLORE_SYMBOLIC_H */
