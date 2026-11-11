"""Relais — independent reimplementation of a computer's state machine (M13).

Written from the frozen contract (`core/include/relais/node.h`) and
`docs/PROTOCOL.md` §6, NOT transliterated from `core/src/node.c`. It mirrors the
protocol part: role (NONE/SECOND/HOLDER), the mandate read from STATUT (a node
never invents one), and the frames it emits per tick. The control law stays in C
(the mission allows it): the value computed here is a stub, because the object of
the differential test is the PROTOCOL, not the arithmetic.

Requirement IDs: MAN-05, MAN-06, MAN-07, MAN-08, MAN-09, MAN-10, REL-04, REL-05,
REL-06, REL-09, DET-02, DET-05, REP-01, REP-03, CMD-02, CMD-06.
"""

from arbiter_py import (NODE_NONE, NODE_A, NODE_B, NODE_ACTUATOR, MSG_STATUT,
                        MSG_ORDRE, MSG_OMBRE, MSG_DEMANDE, MSG_CESSION, MSG_ETAT,
                        MSG_SUIVI, TESTAMENT_MAX, default_params)

ROLE_NONE, ROLE_SECOND, ROLE_HOLDER = 0, 1, 2


class Node:
    """One computer's protocol state machine (mirrors relais_node_sm)."""

    def __init__(self, self_node, params=None):
        self.p = dict(default_params())
        if params is not None:
            self.p.update(params)
        self.self = self_node
        self.role = ROLE_NONE
        self.mandate = 0
        self.seen_mandate = 0
        self.seen_holder = NODE_NONE
        self.last_statut = 0
        self.last_etat = 0
        self.now = 0
        self.replica_age = 0
        self.bail_ticks = 0
        self.ready = False
        self.link_lost = False
        self.deposed = False
        self.without_actuator = False
        self.last_value = 0.0
        self.orders_sent = 0
        self.shadows_sent = 0
        self.demandes_sent = 0
        self.cessions_sent = 0
        self.stale_dropped = 0

    # ---- the control law is a stub: the protocol is what is compared ----

    def _compute(self, meas):
        """A trivial, bounded value so the frames are well-formed (CMD-06)."""
        v = meas.get("setpoint", 0.0)
        amp = self.p["max_amplitude"]
        return max(-amp, min(amp, v))

    def _will(self, value):
        return [value] * TESTAMENT_MAX

    def _ordre(self, tick, value):
        return {"type": MSG_ORDRE, "sender": self.self, "tick": tick,
                "mandate": self.mandate, "value": value,
                "testament": self._will(value), "testament_len": TESTAMENT_MAX}

    def _ombre(self, tick, value):
        return {"type": MSG_OMBRE, "sender": self.self, "tick": tick,
                "value": value, "replica_age": self.replica_age}

    def _demande(self, tick, with_order, value):
        return {"type": MSG_DEMANDE, "sender": self.self, "seen_mandate": self.seen_mandate,
                "reason": 1, "has_order": with_order, "tick": tick, "value": value,
                "testament": self._will(value), "testament_len": TESTAMENT_MAX}

    def _cession(self, reason=1):
        return {"type": MSG_CESSION, "sender": self.self, "mandate": self.mandate,
                "reason": reason}

    def _apply_role(self):
        """Role transitions read from the STATUT (a node never invents a mandate)."""
        # NONE -> SECOND on the first STATUT: learn holder and mandate.
        if self.role == ROLE_NONE:
            self.role = ROLE_SECOND
        # HOLDER -> SECOND when deposed or the mandate moved (MAN-08).
        if self.role == ROLE_HOLDER and self.seen_holder != self.self:
            self.role = ROLE_SECOND
            self.deposed = True
            self.mandate = self.seen_mandate
        # NONE/SECOND -> HOLDER when the actuator appointed us (MAN-05/06/10).
        if self.seen_holder == self.self and self.role != ROLE_HOLDER:
            self.role = ROLE_HOLDER
            self.mandate = self.seen_mandate
            self.ready = False

    def _emit(self, statut, value):
        """Frames emitted for tick now+1, per role (CMD-02, REP-01, REP-03)."""
        out = []
        nxt = self.now + 1
        if self.role == ROLE_HOLDER:
            out.append(self._ordre(nxt, value))
            self.orders_sent += 1
            peer = NODE_B if self.self == NODE_A else NODE_A
            out.append({"type": MSG_ETAT, "sender": self.self, "tick": nxt,
                        "last_value": value, "to": peer})
        elif self.role == ROLE_SECOND:
            out.append(self._ombre(nxt, value))
            self.shadows_sent += 1
            if self.seen_holder != NODE_NONE:
                out.append({"type": MSG_SUIVI, "sender": self.self, "tick": nxt,
                            "replica_age": self.replica_age, "ready": self.ready,
                            "to": self.seen_holder})
            # DET-02: on suspicion we may ask, but it has NO effect without a
            # grant (REL-04).
            if statut.get("misses", 0) > 0 and self.seen_holder != self.self:
                out.append(self._demande(nxt, with_order=True, value=value))
                self.demandes_sent += 1
        self.last_value = value
        return out

    # ---- the driver: called on every STATUT (CMD-02) ----

    def on_status(self, statut):
        """Process one STATUT and return the list of frames to emit."""
        self.now = statut.get("tick", self.now)
        self.last_statut = self.now
        self.bail_ticks = 0
        self.without_actuator = False
        self.seen_holder = statut.get("holder", NODE_NONE)
        self.seen_mandate = statut.get("mandate", self.seen_mandate)
        value = self._compute(statut.get("meas", {}))
        self._apply_role()
        out = self._emit(statut, value)
        self.deposed = False
        return out

    def on_etat(self, etat):
        """State replication from the holder (REP-01/REP-02). An older ETAT is
        dropped. Returns the list of frames to emit (none by default)."""
        if etat.get("tick", 0) < self.last_etat:
            return []
        self.last_etat = etat.get("tick", self.last_etat)
        self.replica_age = 0
        self.last_value = etat.get("last_value", self.last_value)
        return []

    def on_silence(self, now):
        """No STATUT for bail_ticks: the node deposes itself (MAN-09).
        Diagnostics only — safety rests on MAN-02."""
        self.now = now
        self.bail_ticks += 1
        if self.bail_ticks >= self.p["bail_ticks"] and self.role == ROLE_HOLDER:
            self.role = ROLE_SECOND
            self.deposed = True
            return [self._cession(reason=2)]
        return []

    def on_local_fault(self, reason=1):
        """A critical local process died: cede within the tick (DET-05)."""
        if self.role == ROLE_HOLDER:
            self.role = ROLE_SECOND
            self.cessions_sent += 1
            return [self._cession(reason=reason)]
        return []

    def on_handover(self):
        """Voluntary handover for maintenance (REL-09)."""
        return self.on_local_fault(reason=3)