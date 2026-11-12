"""Relais — shared protocol vocabulary and float helpers for the Python mirror.

Split out of arbiter_py.py so each file stays under the 500-line rule. The
constants mirror the enums of `core/include/relais/types.h` exactly; the float
helpers reproduce the control-side bounds of `core/src/control.c` (ACT-02/03/04/
05, I6). They are used by both arbiter_py.py and node_py.py (no duplication).
"""

# ---- node identities (relais_node) ----
NODE_NONE, NODE_A, NODE_B, NODE_C, NODE_ACTUATOR = 0, 1, 2, 3, 4
COMPUTERS = (NODE_A, NODE_B, NODE_C)

# ---- message types (relais_msg_type) ----
MSG_STATUT, MSG_ORDRE, MSG_OMBRE, MSG_DEMANDE, MSG_CESSION, MSG_ETAT, MSG_SUIVI = range(1, 8)

# ---- value source (relais_source) ----
SRC_NONE, SRC_HOLDER, SRC_SHADOW, SRC_TESTAMENT, SRC_FALLBACK = 0, 1, 2, 3, 4

# ---- lamp (relais_lamp) ----
LAMP_OFF, LAMP_YELLOW, LAMP_RED = 0, 1, 2

# ---- cause (relais_cause) ----
CAUSE_NONE = 0
CAUSE_NODE_MISSING = 1
CAUSE_LINK_LOST = 2
CAUSE_SECOND_UNCERTIFIED = 3
CAUSE_DISAGREEMENT = 4
CAUSE_FALLBACK = 5
CAUSE_STOP_REQUESTED = 6

# ---- reject reasons (relais_reject) ----
REJ_NONE = 0
REJ_NOT_HOLDER = 1
REJ_MANDATE_STALE = 2
REJ_MANDATE_FUTURE = 3
REJ_TICK_MISSED = 4
REJ_REVOKED = 10       # M20: sender on the revocation list
REJ_AUTH_BANNED = 11   # M20: sender banned after authenticator strikes

# ---- journal events (relais_event_kind) ----
EV_MANDATE_GRANTED = 1
EV_MANDATE_REVOKED = 2
EV_MANDATE_AUTO_SWITCH = 3
EV_REQUEST_DENIED = 4
EV_CERTIFIED = 5
EV_DECERTIFIED = 6
EV_ORDER_REJECTED = 7
EV_DISAGREEMENT = 8
EV_FALLBACK_ENTER = 9
EV_FALLBACK_EXIT = 10
EV_CESSION = 11
EV_LAMP_CHANGE = 12
EV_STOP_REQUESTED = 13
EV_FORGED = 14
EV_NODE_BACK = 15
EV_NODE_REVOKED = 16   # M20
EV_AUTH_BANNED = 17    # M20

# ---- structural limits (types.h) ----
TESTAMENT_MAX = 8
EVENTS_MAX = 8
IN_MSG_MAX = 8
REJECT_SLOTS = 12
LIVENESS_TICKS = 8
TICK_NONE = (1 << 64) - 1

FALLBACK_DAMP = 0.5


def default_params():
    """Defaults, matching relais_params_default() in core/src/params.c."""
    return {
        "period_ms": 5,
        "patience": 1,
        "testament_len": 8,
        "certify_ticks": 3,
        "bail_ticks": 4,
        "request_burst": 4,
        "request_window": 4,
        "epsilon": 0.005,
        "delta": 0.02,
        "degraded_ticks": 6000,
        "max_amplitude": 1.0,
        "max_slew": 0.25,
        "slow_motion": False,
        "slow_factor": 10,
    }


def clamp_value(v, p):
    """ACT-05 amplitude bound; NaN collapses to -max_amplitude like the C code."""
    amp = p["max_amplitude"]
    if not (v > -amp):
        return -amp
    if v > amp:
        return amp
    return v


def clamp_slew(previous, wanted, p):
    """ACT-05 slew bound, one tick."""
    delta = wanted - previous
    slew = p["max_slew"]
    if delta > slew:
        return previous + slew
    if delta < -slew:
        return previous - slew
    return wanted


def fallback_step(fb_state, meas, p):
    """ACT-03/ACT-04: local damped hold of the position, rate limited."""
    velocity = meas["vel"]
    wanted = clamp_value(-FALLBACK_DAMP * velocity, p)
    return clamp_slew(fb_state, wanted, p)


def testament_at(testament, length, base, tick):
    """I6: the will only plays inside its horizon (nothing stale)."""
    if length == 0 or base == TICK_NONE or tick < base:
        return None
    offset = tick - base
    if offset >= length:
        return None
    return testament[offset]