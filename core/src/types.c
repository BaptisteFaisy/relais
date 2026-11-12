/* Relais — name tables for the enumerations. One place, used by the journal,
 * the explorer, the verifier and the reports. */
#include "relais/types.h"

const char *relais_msg_type_name(relais_msg_type t) {
  switch (t) {
    case RELAIS_MSG_STATUT: return "statut";
    case RELAIS_MSG_ORDRE: return "ordre";
    case RELAIS_MSG_OMBRE: return "ombre";
    case RELAIS_MSG_DEMANDE: return "demande";
    case RELAIS_MSG_CESSION: return "cession";
    case RELAIS_MSG_ETAT: return "etat";
    case RELAIS_MSG_SUIVI: return "suivi";
  }
  return "inconnu";
}

const char *relais_source_name(relais_source s) {
  switch (s) {
    case RELAIS_SRC_NONE: return "none";
    case RELAIS_SRC_HOLDER: return "holder";
    case RELAIS_SRC_SHADOW: return "shadow";
    case RELAIS_SRC_TESTAMENT: return "testament";
    case RELAIS_SRC_FALLBACK: return "fallback";
  }
  return "none";
}

const char *relais_reject_name(relais_reject r) {
  switch (r) {
    case RELAIS_REJ_NONE: return "none";
    case RELAIS_REJ_NOT_HOLDER: return "not_holder";
    case RELAIS_REJ_MANDATE_STALE: return "mandate_stale";
    case RELAIS_REJ_MANDATE_FUTURE: return "mandate_future";
    case RELAIS_REJ_TICK_MISSED: return "tick_missed";
    case RELAIS_REJ_CHECK: return "check";
    case RELAIS_REJ_MALFORMED: return "malformed";
    case RELAIS_REJ_RATE_LIMITED: return "rate_limited";
    case RELAIS_REJ_NOT_CERTIFIED: return "not_certified";
    case RELAIS_REJ_VERSION: return "version";
    case RELAIS_REJ_REVOKED: return "revoked";
    case RELAIS_REJ_AUTH_BANNED: return "auth_banned";
  }
  return "inconnu";
}

const char *relais_event_name(relais_event_kind k) {
  switch (k) {
    case RELAIS_EV_NONE: return "none";
    case RELAIS_EV_MANDATE_GRANTED: return "mandate_granted";
    case RELAIS_EV_MANDATE_REVOKED: return "mandate_revoked";
    case RELAIS_EV_MANDATE_AUTO_SWITCH: return "mandate_auto_switch";
    case RELAIS_EV_REQUEST_DENIED: return "request_denied";
    case RELAIS_EV_CERTIFIED: return "certified";
    case RELAIS_EV_DECERTIFIED: return "decertified";
    case RELAIS_EV_ORDER_REJECTED: return "order_rejected";
    case RELAIS_EV_DISAGREEMENT: return "disagreement";
    case RELAIS_EV_FALLBACK_ENTER: return "fallback_enter";
    case RELAIS_EV_FALLBACK_EXIT: return "fallback_exit";
    case RELAIS_EV_CESSION: return "cession";
    case RELAIS_EV_LAMP_CHANGE: return "lamp_change";
    case RELAIS_EV_STOP_REQUESTED: return "stop_requested";
    case RELAIS_EV_FORGED: return "forged";
    case RELAIS_EV_NODE_BACK: return "node_back";
    case RELAIS_EV_NODE_REVOKED: return "node_revoked";
    case RELAIS_EV_AUTH_BANNED: return "auth_banned";
  }
  return "none";
}

const char *relais_node_name(relais_node n) {
  switch (n) {
    case RELAIS_NODE_NONE: return "";
    case RELAIS_NODE_A: return "A";
    case RELAIS_NODE_B: return "B";
    case RELAIS_NODE_C: return "C";
    case RELAIS_NODE_ACTUATOR: return "actuator";
  }
  return "";
}

const char *relais_lamp_name(relais_lamp l) {
  switch (l) {
    case RELAIS_LAMP_OFF: return "off";
    case RELAIS_LAMP_YELLOW: return "yellow";
    case RELAIS_LAMP_RED: return "red";
  }
  return "off";
}

const char *relais_cause_name(relais_cause c) {
  switch (c) {
    case RELAIS_CAUSE_NONE: return "none";
    case RELAIS_CAUSE_NODE_MISSING: return "node_missing";
    case RELAIS_CAUSE_LINK_LOST: return "link_lost";
    case RELAIS_CAUSE_SECOND_UNCERTIFIED: return "second_uncertified";
    case RELAIS_CAUSE_DISAGREEMENT: return "disagreement";
    case RELAIS_CAUSE_FALLBACK: return "fallback";
    case RELAIS_CAUSE_STOP_REQUESTED: return "stop_requested";
  }
  return "none";
}

bool relais_node_is_computer(relais_node n) {
  return n == RELAIS_NODE_A || n == RELAIS_NODE_B || n == RELAIS_NODE_C;
}
