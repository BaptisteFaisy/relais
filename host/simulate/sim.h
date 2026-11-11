/* Relais — private declarations of the seeded simulator (level 2, spec 7.4).
 * Shared by simulate.c and sim_faults.c: one implementation of the tick
 * pipeline, one fault model, used by the plain runs and the campaigns.
 */
#ifndef RELAIS_SIM_H
#define RELAIS_SIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "relais/invariants.h"
#include "relais/journal.h"
#include "relais/node.h"

#define SIM_MAX_FAULTS 64
#define SIM_MAX_PENDING 32
#define SIM_NODE_COUNT 2

typedef enum {
  SIM_F_NONE = 0,
  SIM_F_DEATH,    /* F1: the machine stops dead */
  SIM_F_FREEZE,   /* F2: frozen for arg ticks, then it wakes up */
  SIM_F_THAW,
  SIM_F_CUT_ACT,  /* F5: link machine -> actuator cut */
  SIM_F_CUT_PEER, /* F4: link between the two computers cut */
  SIM_F_RESTORE,
  SIM_F_SLOW,     /* F3: one tick out of arg is lost */
  SIM_F_BIAS,     /* F8: wrong value emitted */
  SIM_F_RESTART,
  SIM_F_CRASH_PROC, /* a process of the machine dies: DET-05 cession */
  SIM_F_UPDATE,     /* REL-10: clean stop for an update, reboot arg ticks later */
  SIM_F_HANDOVER,   /* REL-09: the holder cedes voluntarily, for maintenance */
  SIM_F_FORGE,      /* a machine forges an order it is not entitled to (SEC-04) */
  SIM_F_FORGE_STALE /* the holder replays an order for a past tick (ACT-09) */
} sim_fault_kind;

typedef struct {
  uint32_t at_tick;
  sim_fault_kind kind;
  int node; /* 0 = A, 1 = B */
  uint32_t arg;
  float value;
} sim_fault_t;

typedef struct {
  relais_msg msg;
  uint32_t deliver_at;
  bool used;
} sim_pending_t;

typedef struct {
  relais_node_sm sm;
  bool dead;
  uint32_t frozen_until;
  bool cut_act;   /* its frames never reach the actuator */
  bool cut_peer;  /* ETAT/SUIVI never reach the peer */
  bool deaf;      /* it never hears the actuator */
  uint32_t slow_every; /* drop one tick out of slow_every */
  uint32_t reinit_at;  /* REL-10: tick of the reboot after an update, 0 = none */
  uint32_t tick_count;
} sim_node_t;

typedef struct {
  relais_params params;
  relais_traj traj;
  relais_arb arb;
  relais_plant plant;
  sim_node_t nodes[SIM_NODE_COUNT];
  sim_pending_t to_act[SIM_MAX_PENDING];
  uint32_t pending_count;
  uint64_t rng;
  sim_fault_t faults[SIM_MAX_FAULTS];
  uint32_t fault_count;
  uint32_t faults_applied;
  bool complete;      /* level C: shadows and certification are in play */
  double loss;        /* random frame loss probability, 0..1 */
  uint32_t max_delay; /* random extra delay in ticks */
  bool lost_last[2];  /* per link: the previous frame was lost (burst guard) */
} sim_t;

void sim_init(sim_t *s, const relais_params *p, const relais_traj *t, uint64_t seed,
              bool complete);
bool sim_add_fault(sim_t *s, uint32_t at_tick, const char *kind, int node, uint32_t arg,
                   float value);
bool sim_parse_fault(sim_t *s, const char *spec);
void sim_seed_random_faults(sim_t *s, uint32_t count, uint32_t first_tick, uint32_t spread);
uint32_t sim_apply_faults(sim_t *s, uint32_t tick);
/* One tick of one world: actuator, both machines, the links, the plant.
 * The caller runs the twin without fault in a second sim_t and compares. */
void sim_run_tick(sim_t *s, uint32_t tick, relais_arb_out *out);
void sim_deliver_outputs(sim_t *s, sim_node_t *from, sim_node_t *peer,
                         const relais_node_out *out, uint32_t tick, uint32_t deliver_at);
uint64_t sim_rand(sim_t *s);
bool sim_chance(sim_t *s, double probability);

/* Journal helpers shared by the simulator and the campaign runner. */
void sim_write_header(FILE *f, const sim_t *s, const char *scenario, uint32_t id);
void sim_write_tick(FILE *f, const relais_arb_out *out, uint32_t tick, const relais_plant *pl,
                    float setpoint);
void sim_write_events(FILE *f, const relais_arb_out *out, uint32_t tick);

#endif /* RELAIS_SIM_H */
