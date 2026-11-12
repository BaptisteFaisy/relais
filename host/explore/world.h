/* Relais — the explored world: two computers, one actuator, six half-links,
 * the frames in flight and the faults (spec 7.3).
 *
 * The world is a plain structure with no pointer and no allocation, so a state
 * is copied by assignment and hashed by bytes. Every field it contains is
 * memset-initialised by the core, which is what makes byte hashing sound.
 */
#ifndef RELAIS_EXPLORE_WORLD_H
#define RELAIS_EXPLORE_WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "relais/invariants.h"
#include "relais/journal.h"
#include "relais/node.h"

#define EXPLORE_NODES 2
#define EXPLORE_LINKS 6   /* A->act, B->act, act->A, act->B, A->B, B->A */
#define EXPLORE_SLOTS 2   /* frames in flight per link, enough to duplicate */
#define EXPLORE_MUT_MAX 3 /* adversary moves allowed between two ticks */
#define EXPLORE_MSG_MAX 8

typedef enum {
  LINK_A_ACT = 0, LINK_B_ACT, LINK_ACT_A, LINK_ACT_B, LINK_A_B, LINK_B_A
} explore_link;

typedef struct {
  relais_msg msg;
  uint32_t deliver_at;
  bool busy;
} explore_slot;

typedef struct {
  relais_params params;
  relais_traj traj;
  relais_arb arb;
  relais_node_sm node[EXPLORE_NODES];
  relais_plant plant;
  relais_inv inv;
  uint32_t tick;
  relais_source source; /* where the value applied at this tick came from (spec 4.4) */
  explore_slot slot[EXPLORE_LINKS][EXPLORE_SLOTS];
  bool dead[EXPLORE_NODES];
  uint32_t frozen_until[EXPLORE_NODES];
  bool cut[EXPLORE_LINKS];
  uint8_t pending_mutations;
  /* The single-fault hypothesis, applied to the channel: one disrupted link at
   * a time, and no burst of losses longer than p (spec 3.3, 7.3 item 5, ROB-03).
   * Both counters are reset by every tick. */
  int8_t channel_link;
  uint8_t burst_this_tick;  /* losses and cuts already inflicted in this window */
  bool complete;
} explore_world;

typedef enum {
  EX_TICK = 0, EX_DROP, EX_DELAY, EX_DUP, EX_KILL, EX_FREEZE1, EX_FREEZE3,
  EX_THAW, EX_RESTART, EX_CUT, EX_RESTORE, EX_KIND_COUNT
} explore_kind;

typedef struct {
  explore_kind kind;
  int link;
  int node;
} explore_event;

void explore_world_init(explore_world *w, const relais_params *p, const relais_traj *t,
                        bool complete, bool expect_certified);
/* One adversary move, or the tick itself. Mutations do not advance time. */
void explore_world_apply(explore_world *w, const explore_event *ev);
/* Every move allowed in this state, within the fault budget. */
int explore_world_enumerate(const explore_world *w, explore_event *out, int cap,
                            int fault_budget);
uint64_t explore_world_hash(const explore_world *w);
bool explore_world_ok(const explore_world *w);
uint32_t explore_active_faults(const explore_world *w);
const char *explore_kind_name(explore_kind k);
const char *explore_link_name(explore_link l);
/* Half-link as the scenario format of CONTRACT.md names it (link + direction). */
void explore_link_scenario(int link, const char **name, const char **direction);

#endif /* RELAIS_EXPLORE_WORLD_H */
