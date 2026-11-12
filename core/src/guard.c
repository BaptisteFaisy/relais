/* Relais — the guard on the arbiter input (M20, ideas 38 and 39).
 *
 * Two mechanisms sit in front of every protocol rule. Both are OFF by default,
 * so a freshly initialised arbiter behaves exactly as before:
 *
 *  - the revocation list (idea 39): an operator decision. A revoked node sees
 *    every one of its frames rejected and counted, and the revocation survives
 *    a restart of the actuator because the mask travels in the snapshot
 *    (MAN-11). There is no un-revoke: clearing one is an operator act, done by
 *    restoring a snapshot taken before it.
 *
 *  - the authenticator strikes (idea 38, DEG-09): the safe degraded mode. The
 *    transport reports every authenticator failure it sees; after
 *    params.auth_strikes consecutive failures for the same sender, that sender
 *    is banned, its frames are rejected and counted, and the event is
 *    journaled. A success clears the streak. At auth_strikes == 0 (the default)
 *    the mechanism is disabled and nothing changes.
 *
 * The core stays pure: no clock, no file, no socket, no allocation. The
 * transport (actuator/pc, target/net.c) is what calls relais_arb_auth_fail and
 * relais_arb_auth_ok; the arbiter only keeps the state and applies it.
 */
#include "guard.h"

#include <string.h>

uint8_t relais__guard_bit(relais_node n) {
  return (uint8_t)(1u << (unsigned)n);
}

static bool guard_node(relais_node n) {
  /* Only the two computers (and the spare C) can be revoked or banned; the
   * actuator itself and RELAIS_NODE_NONE are not senders of inbound frames. */
  return relais_node_is_computer(n);
}

relais_reject relais__guard_block(const relais_arb *a, relais_node n) {
  if (a == NULL || !guard_node(n)) return RELAIS_REJ_NONE;
  uint8_t bit = relais__guard_bit(n);
  if ((a->revoked_mask & bit) != 0u) return RELAIS_REJ_REVOKED;
  if ((a->banned_mask & bit) != 0u) return RELAIS_REJ_AUTH_BANNED;
  return RELAIS_REJ_NONE;
}

void relais__guard_flush(relais_arb *a, relais_arb_out *out, relais_tick tick) {
  if (a == NULL) return;
  for (int i = RELAIS_NODE_A; i <= RELAIS_NODE_C; i++) {
    uint8_t bit = relais__guard_bit((relais_node)i);
    if ((a->revoke_pending & bit) != 0u) {
      a->revoke_pending = (uint8_t)(a->revoke_pending & (uint8_t)~bit);
      relais__arb_push_event(out, RELAIS_EV_NODE_REVOKED, (relais_node)i, a->mandate, tick, 0);
    }
    if ((a->ban_pending & bit) != 0u) {
      a->ban_pending = (uint8_t)(a->ban_pending & (uint8_t)~bit);
      relais__arb_push_event(out, RELAIS_EV_AUTH_BANNED, (relais_node)i, a->mandate, tick,
                             (uint32_t)a->auth_strike[i]);
    }
  }
}

void relais_arb_revoke(relais_arb *a, relais_node n) {
  if (a == NULL || !guard_node(n)) return;
  uint8_t bit = relais__guard_bit(n);
  a->revoked_mask = (uint8_t)(a->revoked_mask | bit);
  /* Journaled at the next tick: the event needs a tick number and an output
   * block, which only relais_arb_step has. */
  a->revoke_pending = (uint8_t)(a->revoke_pending | bit);
}

bool relais_arb_is_revoked(const relais_arb *a, relais_node n) {
  if (a == NULL || !guard_node(n)) return false;
  return (a->revoked_mask & relais__guard_bit(n)) != 0u;
}

bool relais_arb_auth_fail(relais_arb *a, relais_node n) {
  if (a == NULL || !guard_node(n)) return false;
  /* Disabled by default: the strike is not even counted, so the behaviour and
   * the state of the arbiter are exactly the ones before this mechanism. */
  if (a->params.auth_strikes == 0u) return false;
  uint8_t bit = relais__guard_bit(n);
  if ((a->banned_mask & bit) != 0u) return false; /* already banned */
  uint8_t strikes = (uint8_t)(a->auth_strike[n] < 255u ? a->auth_strike[n] + 1u : 255u);
  a->auth_strike[n] = strikes;
  if (strikes < a->params.auth_strikes) return false;
  a->banned_mask = (uint8_t)(a->banned_mask | bit);
  a->ban_pending = (uint8_t)(a->ban_pending | bit);
  return true;
}

void relais_arb_auth_ok(relais_arb *a, relais_node n) {
  if (a == NULL || !guard_node(n)) return;
  a->auth_strike[n] = 0; /* a success clears the streak, never the ban */
}

bool relais_arb_is_banned(const relais_arb *a, relais_node n) {
  if (a == NULL || !guard_node(n)) return false;
  return (a->banned_mask & relais__guard_bit(n)) != 0u;
}
