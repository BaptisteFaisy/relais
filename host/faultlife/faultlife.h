/* Relais — lifecycle and time fault model (spec 7.4, mission M19).
 *
 * The protocol core has no clock, no file, no socket, no allocation: the only
 * time it sees is the discrete `relais_tick` counter owned by the actuator.
 * This module drives the very same core (arbiter + two nodes + plant) through a
 * minimal world whose *environment* carries the lifecycle faults of ideas
 * 13, 14, 15, 19 and 20:
 *   13 a machine clock that runs fast, slow or jumps: only the diagnostic
 *      (bail, MAN-09) may change, never the actuator's decision;
 *   14 a brutal power loss, possibly mid-send (a truncated frame in flight);
 *   15 a boot loop (restart every k periods);
 *   19 a demand storm (a demande every tick, both machines);
 *   20 an OTA update interrupted at every step.
 *
 * It is intentionally separate from host/simulate/sim.c: that pipeline has no
 * notion of a per-machine clock, an in-flight byte stream, a boot cadence or an
 * OTA life cycle. Reusing it whole would force those concepts into a struct it
 * does not own; a small world here keeps the lifecycle faults in one place.
 */
#ifndef RELAIS_FAULTLIFE_H
#define RELAIS_FAULTLIFE_H

#include <stdbool.h>
#include <stdint.h>

#include "relais/arbiter.h"
#include "relais/control.h"
#include "relais/node.h"
#include "relais/params.h"

#define LW_NODES 2
#define LW_MAX_PENDING 16
/* A reboot keeps the machine down for a few ticks (longer than p+1), so a boot
 * loop genuinely interrupts the command instead of being transparent. */
#define LW_REBOOT_DOWN 3

typedef struct {
  relais_msg msg;
  uint32_t deliver_at;
  bool used;
} lw_pending_t;

typedef struct {
  relais_params params;
  relais_traj traj;
  relais_arb arb;
  relais_plant plant;
  relais_node_sm node[LW_NODES];

  /* Environment (lifecycle) state, per machine. */
  bool dead[LW_NODES];          /* 14/20: brutal power loss, no cession */
  bool deaf[LW_NODES];          /* the machine does not hear the actuator */
  uint32_t boot_period[LW_NODES]; /* 15: restart every k ticks, 0 = none */
  uint32_t boot_dead_until[LW_NODES]; /* 15: the machine is down until this tick */
  bool storm[LW_NODES];         /* 19: a demande is emitted every tick */

  /* 13: the machine's local clock runs wd_num/wd_den of the tick rate; it only
   * drives the silence watchdog (bail), never the stamped order tick. */
  int wd_num[LW_NODES];
  int wd_den[LW_NODES];
  int wd_acc[LW_NODES];
  uint32_t wd_jump_at[LW_NODES]; /* after a freeze: clock jumps, bail at once */
  bool wd_jumped[LW_NODES];

  lw_pending_t pending[LW_MAX_PENDING];
  uint32_t pending_count;

  /* Observability, for the tests. */
  uint32_t mandate_changes;
  relais_node last_holder;
  uint32_t granted;
  uint32_t denied;
  uint32_t rate_limited;
  uint32_t truncated_frames; /* 14: frames dropped mid-send (partial bytes) */
  uint32_t queue_drops;      /* 19: pending queue overflow, must stay 0 */
} lw_t;

void lw_init(lw_t *w, const relais_params *p, const relais_traj *t);
void lw_update(lw_t *w, uint32_t tick, relais_arb_out *out);

/* Lifecycle fault injection (call before lw_update). */
void lw_power_loss(lw_t *w, int idx, bool mid_send); /* 14 */
void lw_set_boot_period(lw_t *w, int idx, uint32_t k); /* 15 */
void lw_set_storm(lw_t *w, int idx, bool on); /* 19 */
void lw_set_watchdog(lw_t *w, int idx, int num, int den); /* 13: clock rate */
void lw_watchdog_jump(lw_t *w, int idx, uint32_t at); /* 13: jump after a gel */
void lw_restart_node(lw_t *w, int idx); /* 15/20: a clean reboot */

/* One-step OTA at the actuator level: holder cedes, frees the mandate at the
 * next tick (REL-06, MAN-11). Returns true once the cession was sent. */
bool lw_ota_cede(lw_t *w, int idx);

#endif /* RELAIS_FAULTLIFE_H */