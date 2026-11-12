/* Relais — the comparison harness: one world, three handover protocols.
 *
 * The world holds two computers, one actuator, the links, the plant and the
 * faults. The three implementations (Relais, heartbeat, lease) are plugged
 * into it through proto.h. Everything the comparison measures is measured
 * here, so no implementation can be flattered by a private bench.
 *
 * Docs: docs/COMPARISON-MEASURED.md.
 */
#ifndef RELAIS_BASELINE_HARNESS_H
#define RELAIS_BASELINE_HARNESS_H

#include "proto.h"

#define BL_MAX_FAULTS 32u
#define BL_TICK_NONE 0xFFFFFFFFu

/* The faults the three protocols can be compared on. A fault means the same
 * thing for all of them: the machine dies, it freezes, it loses a link, it
 * slows down, or it has a local failure and steps down. */
typedef enum {
  BL_F_DEATH = 0,  /* F1: the machine stops dead */
  BL_F_FREEZE,     /* F2: frozen for arg ticks */
  BL_F_CUT_ACT,    /* F5: the link machine -> actuator is cut */
  BL_F_CUT_PEER,   /* F4: the link between the two computers is cut */
  BL_F_SLOW,       /* F3: one tick out of arg is lost */
  BL_F_RESTORE,    /* the links come back */
  BL_F_HANDOVER    /* a local process died: the machine steps down (DET-05) */
} bl_fault_kind;

typedef struct {
  uint32_t at;
  bl_fault_kind kind;
  uint8_t node;
  uint32_t arg;
} bl_fault;

typedef struct {
  const bl_impl *impl;
  uint32_t ticks;
  uint64_t seed;
  uint32_t timeout;  /* watchdog timeout (heartbeat) or lease (lease), ticks */
  double loss;       /* frame loss probability on the links */
  bool burst_guard;  /* ROB-03: no two losses in a row on the same computer */
  uint32_t delay;    /* extra delay, in ticks, on the machine -> actuator link */
  uint32_t window;   /* measurement window, in ticks, after the fault */
  uint32_t measure_at;/* the instant the measurement starts, 0 = the first fault */
  bool twin;         /* internal: the fault free reference run */
  bl_fault faults[BL_MAX_FAULTS];
  size_t fault_count;
} bl_config;

/* What one run measured. */
typedef struct {
  bool handover_done;   /* did the actuator change of commander at all */
  uint32_t handover_ticks;  /* ticks between the fault and the change */
  bool fresh_done;      /* did a fresh command from the other computer arrive */
  uint32_t fresh_ticks;
  uint32_t stale_ticks; /* ticks served without any fresh command */
  uint32_t double_cmds; /* ticks with two computers believing they command */
  uint32_t split_cmds;  /* ticks with two commanders whose commands differ */
  float twin_gap_max;   /* max |pos - pos_twin|, the loss of setpoint */
  float track_err_max;  /* max |pos - setpoint| */
  float cmd_jump_max;   /* max |value(t) - value(t-1)| in the window */
  float handover_jump;   /* the discontinuity at the change of commander */
  uint32_t counters[2]; /* implementation counters (Relais rejections) */
} bl_metrics;

void bl_run(const bl_config *cfg, bl_metrics *out);
/* Print one line per tick on stdout: the tick by tick replay of a scenario. */
void bl_trace_enable(bool on);
/* "at:kind:node[:arg]" -> a fault of the shared schedule. */
bool bl_fault_parse(bl_config *cfg, const char *spec);
const char *bl_fault_kind_name(bl_fault_kind kind);

#endif /* RELAIS_BASELINE_HARNESS_H */
