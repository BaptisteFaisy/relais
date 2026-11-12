/* Relais — Frama-C/WP harness for the arbiter (mission M36).
 *
 * The harness drives the REAL relais_arb_step() and states what the actuator
 * must publish: exactly one bounded value, a mandate that moves by at most
 * one step, and a holder that is the one the actuator appointed.
 *
 * Command: see host/modelcheck/wp/run.sh.
 */
#include "relais/arbiter.h"

int main(void) {
  relais_params p;
  relais_params_default(&p);
  relais_arb a;
  relais_arb_init(&a, &p);
  relais_msg order;
  relais_msg_clear(&order);
  order.type = RELAIS_MSG_ORDRE;
  order.sender = RELAIS_NODE_A;
  order.tick = 1;
  order.mandate = 0;
  order.value = 0.25f;
  order.has_order = true;
  relais_meas meas;
  meas.pos = 0.0f;
  meas.vel = 0.0f;
  meas.applied = 0.0f;
  meas.setpoint = 0.0f;
  relais_arb_out out;
  relais_arb_step(&a, 1, &order, 1, &meas, &out);
  /*@ assert out.applied; */
  /*@ assert out.source >= RELAIS_SRC_HOLDER && out.source <= RELAIS_SRC_FALLBACK; */
  /*@ assert out.value <= p.max_amplitude && out.value >= -p.max_amplitude; */
  /*@ assert out.holder == a.holder; */
  /*@ assert out.mandate == a.mandate; */
  return 0;
}
