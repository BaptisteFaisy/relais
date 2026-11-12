/* Relais — alternative control laws for machine B (mission M37, DEG-08, CMD-09).
 *
 * The built-in law of core/src/control.c is a PID with a filtered measurement,
 * an anti-windup integrator and a bounded setpoint extrapolation. Running that
 * very same law on both machines is what makes the redundancy identical: one
 * software defect then takes both of them down at once (see docs/CCF.md).
 *
 * These laws are written apart, on purpose, with a different structure:
 *
 *   LAW_ALT_FIRST_ORDER  a first order reference model plus a proportional and
 *                        derivative action. No integrator at all, so no windup
 *                        and a permanent tracking offset on a ramp.
 *   LAW_ALT_RAMP         a ramp follower: feed forward on the measured slope of
 *                        the steering request plus a weak position feedback. No
 *                        filter, no integrator, no derivative of the measurement.
 *
 * They are plugged through the three-function interface of CMD-09
 * (relais_node_set_law); core/ is untouched. Their exported state carries its
 * own version byte, so the state replicated by a machine running the built-in
 * law is REFUSED and the second keeps its own state (REP-02).
 */
#ifndef RELAIS_DIV_LAW_ALT_H
#define RELAIS_DIV_LAW_ALT_H

#include "relais/node.h"

typedef enum {
  LAW_ALT_NONE = 0,        /* keep the built-in law */
  LAW_ALT_FIRST_ORDER = 1, /* first order reference model + PD */
  LAW_ALT_RAMP = 2         /* ramp follower: feed forward + weak feedback */
} law_alt_kind;

/* State of an alternative law. `ref` doubles as the previous setpoint for the
 * ramp follower and as the model output for the first order law; `rate` holds
 * the bounded slope it measured. Four floats, so the exported state is 18 bytes
 * against 30 for the built-in one. */
typedef struct {
  law_alt_kind kind;
  float ref;
  float last;
  float rate;
  uint32_t ticks;
} law_alt_state;

law_alt_kind law_alt_parse(const char *name);
const char *law_alt_name(law_alt_kind kind);
/* Fill the CMD-09 hooks for the kind already stored in *ctx. */
void law_alt_hooks(law_alt_state *ctx, relais_law *out);

#endif /* RELAIS_DIV_LAW_ALT_H */
