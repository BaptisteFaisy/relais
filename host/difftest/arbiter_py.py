"""Relais — independent reimplementation of the actuator's arbiter (M13).

Written from the frozen contract (CONTRACT.md, docs/PROTOCOL.md §5 and
docs/TRUTH-TABLE.md) and from the public header `core/include/relais/arbiter.h`,
NOT transliterated from `core/src/arbiter.c`. The point of this file is to be a
second opinion: it is driven by the same decoded frames as the C arbiter and the
confrontation (difftest.py) compares both after every tick.

The protocol state machine lives here; the shared vocabulary and float helpers
live in protocol_py.py. The driver compares floating fields with a small
tolerance while every integer/enum/bool field is compared exactly.

Requirement IDs cited where they anchor a decision: MAN-02, MAN-03, MAN-04,
MAN-05, REL-05, REL-06, DET-01, DET-09, REP-05, DEG-01, DEG-05, SEC-06.
"""

# Shared enums, structural limits and float helpers (ACT-02/03/04/05, I6).
from protocol_py import *  # noqa: F401,F403


class Arbiter:
    """One full copy of the actuator's protocol state (mirrors relais_arb)."""

    def __init__(self, params=None):
        self.p = dict(default_params())
        if params is not None:
            self.p.update(params)
        self.mandate = 0  # MAN-06: vacant at boot, first grant makes it 1
        self.holder = NODE_NONE
        self.misses = 0
        self.switch_pending = False
        self.certified = False
        self.certified_node = NODE_NONE
        self.certify_streak = 0
        self.shadow_misses = 0
        self.last_order = 0.0
        self.last_shadow = 0.0
        self.have_order = False
        self.have_shadow = False
        self.disagree_streak = 0
        self.disagreement = False
        self.testament = [0.0] * TESTAMENT_MAX
        self.testament_len = 0
        self.testament_base = TICK_NONE
        self.fb_state = 0.0
        self.in_fallback = False
        self.last_heard = [0] * 5
        self.present = [False] * 5
        self.ever_seen = [False] * 5
        self.lamp = LAMP_OFF
        self.cause = CAUSE_NONE
        self.degraded_ticks = 0
        self.stop_requested = False
        self.cession_pending = False
        self.request_window_tick = 0
        self.request_seen = 0
        self.rejected = [0] * REJECT_SLOTS
        self.forged = 0
        self.applied_by_source = [0] * 5
        self.ticks = 0
        self.handovers = 0

    # ---- helpers ----

    def _push_event(self, out, kind, node, mandate, tick, detail=0):
        if len(out["events"]) >= EVENTS_MAX:
            return
        out["events"].append({"kind": kind, "node": node, "mandate": mandate,
                              "tick": tick, "detail": detail})

    def _reject(self, out, tick, why, m):
        if 0 <= why < REJECT_SLOTS:
            self.rejected[why] += 1
        self._push_event(out, EV_ORDER_REJECTED, m["sender"], m.get("mandate", 0),
                         tick, why)

    def _order_is_valid(self, m, tick):
        """MAN-02: only the holder's order, current mandate, this tick, counts."""
        if m["sender"] != self.holder:
            return REJ_NOT_HOLDER
        if m["mandate"] > self.mandate:
            return REJ_MANDATE_FUTURE  # SEC-04: nobody appoints itself
        if m["mandate"] != self.mandate:
            return REJ_MANDATE_STALE  # the returning machine
        if m["tick"] != tick:
            return REJ_TICK_MISSED
        return REJ_NONE

    def _release_mandate(self, out, tick, kind):
        """Mandate goes vacant, or straight to the certified second (MAN-10)."""
        evidence = self.misses
        self._push_event(out, kind, self.holder, self.mandate, tick, evidence)
        if (self.certified and self.certified_node != NODE_NONE
                and self.certified_node != self.holder):
            self.holder = self.certified_node
            self.mandate += 1  # MAN-03: exactly one step per change of holder
            self.misses = 0
            self.handovers += 1
            self.switch_pending = True  # REL-05: shadow still fresh, cleared after pick
            self._push_event(out, EV_MANDATE_AUTO_SWITCH, self.holder, self.mandate,
                             tick, evidence)
            return
        self.holder = NODE_NONE
        self.certified = False
        self.certified_node = NODE_NONE
        self.certify_streak = 0

    def _request_allowed(self, tick):
        """SEC-06: a flood of DEMANDE must not delay the orders."""
        w = self.p["request_window"] or 1
        window = tick // w
        if window != self.request_window_tick:
            self.request_window_tick = window
            self.request_seen = 0
        if self.request_seen >= self.p["request_burst"]:
            return False
        self.request_seen += 1
        return True

    def _handle_demande(self, out, tick, s):
        m = s["demande"]
        if m is None:
            return
        vacant = self.holder == NODE_NONE
        grant = vacant
        if not grant or not self._request_allowed(tick):
            self._push_event(out, EV_REQUEST_DENIED, m["sender"], self.mandate,
                             tick, 1 if grant else 0)
            return
        self.holder = m["sender"]
        self.mandate += 1
        self.misses = 0
        self.handovers += 1
        self.certified = False
        self.certified_node = NODE_NONE
        self.certify_streak = 0
        self._push_event(out, EV_MANDATE_GRANTED, self.holder, self.mandate, tick,
                         m.get("reason", 0))
        # MAN-05: the demande may carry the first order, for this very tick.
        if m.get("has_order") and m["tick"] == tick:
            s["order"] = m
            s["order_valid"] = True

    def _pick_value(self, out, tick, s, meas):
        # spec 4.4: exactly one value per tick, in this order.
        if s["order_valid"] and s["order"] is not None:
            order = s["order"]
            out["value"] = clamp_value(order["value"], self.p)
            out["source"] = SRC_HOLDER
            self.applied_by_source[SRC_HOLDER] += 1
            # ACT-02: the will covers the next N ticks, base = tick + 1.
            tlen = order.get("testament_len", 0)
            self.testament_len = min(tlen, TESTAMENT_MAX)
            for i in range(self.testament_len):
                self.testament[i] = order.get("testament", [0.0] * TESTAMENT_MAX)[i]
            self.testament_base = tick + 1
            if self.in_fallback:
                self.in_fallback = False
                self._push_event(out, EV_FALLBACK_EXIT, self.holder, self.mandate,
                                 tick, 0)
            return
        if self.certified and s["shadow"] is not None and \
                s["shadow"]["sender"] == self.certified_node:
            out["value"] = clamp_value(s["shadow"]["value"], self.p)
            out["source"] = SRC_SHADOW  # REL-05: substitution, no stale tick
            self.applied_by_source[SRC_SHADOW] += 1
            return
        planned = testament_at(self.testament, self.testament_len,
                               self.testament_base, tick)
        if planned is not None:
            out["value"] = clamp_value(planned, self.p)
            out["source"] = SRC_TESTAMENT
            self.applied_by_source[SRC_TESTAMENT] += 1
            return
        # ACT-03: the will is exhausted, the local damped loop takes over.
        self.fb_state = fallback_step(self.fb_state, meas, self.p)
        out["value"] = clamp_value(self.fb_state, self.p)
        out["source"] = SRC_FALLBACK
        self.applied_by_source[SRC_FALLBACK] += 1
        if not self.in_fallback:
            self.in_fallback = True
            self._push_event(out, EV_FALLBACK_ENTER, NODE_NONE, self.mandate, tick, 0)

    def _certify(self, out, tick, s):
        if self.holder == NODE_NONE:
            self.certify_streak = 0
            return
        second = NODE_B if self.holder == NODE_A else NODE_A
        if s["shadow"] is None or s["shadow"]["sender"] != second:
            self.certify_streak = 0
            if self.certified:
                self.shadow_misses += 1
                if self.shadow_misses > self.p["patience"]:
                    self.certified = False
                    self.certified_node = NODE_NONE
                    self.shadow_misses = 0
                    self._push_event(out, EV_DECERTIFIED, second, self.mandate, tick, 0)
            return
        self.shadow_misses = 0
        if s["order_valid"] and s["order"] is not None:
            order, shadow = s["order"], s["shadow"]
            gap = abs(order["value"] - shadow["value"])
            self.last_order = order["value"]
            self.last_shadow = shadow["value"]
            if gap <= self.p["epsilon"]:
                self.disagree_streak = 0
                if self.certify_streak < 255:
                    self.certify_streak += 1
            else:
                self.certify_streak = 0
                if self.certified:
                    self.certified = False
                    self.certified_node = NODE_NONE
                    self._push_event(out, EV_DECERTIFIED, second, self.mandate, tick, 0)
                self.disagree_streak += 1
                if self.disagree_streak >= 3 and not self.disagreement:
                    self.disagreement = True  # DET-09: three ticks above epsilon
                    self._push_event(out, EV_DISAGREEMENT, second, self.mandate,
                                     tick, int(gap * 1000.0))
                return
        if not self.certified and self.certify_streak >= self.p["certify_ticks"]:
            self.certified = True
            self.certified_node = second
            self._push_event(out, EV_CERTIFIED, second, self.mandate, tick,
                             self.certify_streak)

    def _liveness(self, out, tick):
        for node in COMPUTERS:
            seen = (self.last_heard[node] != 0
                    and (tick - self.last_heard[node]) <= LIVENESS_TICKS)
            if seen and not self.present[node]:
                self.present[node] = True
                if self.ever_seen[node]:
                    self._push_event(out, EV_NODE_BACK, node, self.mandate, tick, 0)
            elif not seen and self.present[node]:
                self.present[node] = False
            if seen:
                self.ever_seen[node] = True

    def _update_lamp(self, out, tick):
        second = NODE_B if self.holder == NODE_A else NODE_A
        peer_present = self.present[second]
        lamp = LAMP_OFF
        cause = CAUSE_NONE
        if out["source"] == SRC_FALLBACK:
            lamp, cause = LAMP_RED, CAUSE_FALLBACK
        elif self.disagreement:
            lamp, cause = LAMP_RED, CAUSE_DISAGREEMENT
        elif out["source"] != SRC_HOLDER:
            lamp, cause = LAMP_YELLOW, CAUSE_NODE_MISSING  # DEG-01
        elif self.holder == NODE_NONE or not peer_present:
            lamp = LAMP_YELLOW
            cause = CAUSE_NODE_MISSING if self.holder == NODE_NONE else CAUSE_LINK_LOST
        elif not self.certified:
            lamp, cause = LAMP_YELLOW, CAUSE_SECOND_UNCERTIFIED
        if self.stop_requested:
            lamp, cause = LAMP_RED, CAUSE_STOP_REQUESTED
        if lamp != self.lamp:
            self._push_event(out, EV_LAMP_CHANGE, self.holder, self.mandate, tick, lamp)
            self.lamp = lamp
            self.cause = cause
        else:
            self.cause = cause
        # DEG-05: degraded mode is limited in time.
        if lamp == LAMP_OFF:
            self.degraded_ticks = 0
        elif self.degraded_ticks < self.p["degraded_ticks"]:
            self.degraded_ticks += 1
            if self.degraded_ticks >= self.p["degraded_ticks"] and not self.stop_requested:
                self.stop_requested = True
                self._push_event(out, EV_STOP_REQUESTED, self.holder, self.mandate,
                                 tick, self.degraded_ticks)

    def _new_out(self, tick, meas):
        return {"applied": True, "value": 0.0, "source": SRC_NONE, "mandate": 0,
                "holder": NODE_NONE, "misses": 0, "certified": False,
                "certified_node": NODE_NONE, "lamp": LAMP_OFF, "cause": CAUSE_NONE,
                "stop_requested": False,
                "statut": {"type": MSG_STATUT, "sender": NODE_ACTUATOR, "tick": tick,
                           "mandate": 0, "holder": NODE_NONE, "source": SRC_NONE,
                           "misses": 0, "certified": False, "certified_node": NODE_NONE,
                           "lamp": LAMP_OFF, "cause": CAUSE_NONE, "stop_requested": False,
                           "meas": dict(meas)},
                "events": []}

    def _scan(self, msgs, tick, out):
        """Step 1: scan the frames received during the previous period."""
        s = {"order": None, "shadow": None, "cession": None, "demande": None,
             "order_valid": False}
        for m in msgs:
            if m.get("sender") in COMPUTERS:
                self.last_heard[m["sender"]] = tick
            t = m.get("type", 0)
            if t == MSG_ORDRE:
                why = self._order_is_valid(m, tick)
                if why == REJ_NONE:
                    s["order"] = m
                    s["order_valid"] = True
                else:
                    self._reject(out, tick, why, m)
            elif t == MSG_OMBRE:
                if m["tick"] == tick:
                    s["shadow"] = m
            elif t == MSG_CESSION:
                if m["sender"] == self.holder and m["mandate"] == self.mandate:
                    s["cession"] = m
                    self.cession_pending = True  # DET-05, takes effect next tick
                    self._push_event(out, EV_CESSION, m["sender"], self.mandate,
                                     tick, m.get("reason", 0))
            elif t == MSG_DEMANDE:
                if s["demande"] is None:
                    s["demande"] = m
        return s

    def _fill_out(self, out, tick, meas, cert_picked, cert_node_picked):
        """Step 7: what both computers are told (MAN-07)."""
        out["mandate"] = self.mandate
        out["holder"] = self.holder
        out["misses"] = self.misses
        out["certified"] = cert_picked
        out["certified_node"] = cert_node_picked
        out["lamp"] = self.lamp
        out["cause"] = self.cause
        out["stop_requested"] = self.stop_requested
        st = out["statut"]
        st["tick"] = tick
        st["mandate"] = self.mandate
        st["holder"] = self.holder
        st["source"] = out["source"]
        st["misses"] = self.misses
        st["certified"] = self.certified
        st["certified_node"] = self.certified_node
        st["lamp"] = self.lamp
        st["cause"] = self.cause
        st["stop_requested"] = self.stop_requested
        st["meas"] = dict(meas)
        st["meas"]["applied"] = out["value"]

    # ---- the one call per tick, in the exact order of CONTRACT.md ----

    def step(self, tick, msgs, meas=None):
        if meas is None:
            meas = {"pos": 0.0, "vel": 0.0, "applied": 0.0, "setpoint": 0.0}
        out = self._new_out(tick, meas)
        self.ticks += 1
        # REL-06: a cession recorded at the previous tick frees the mandate now.
        if self.cession_pending:
            self.cession_pending = False
            self._release_mandate(out, tick, EV_MANDATE_REVOKED)
        # 1. scan the frames received during the previous period.
        s = self._scan(msgs, tick, out)
        # 2. the miss is counted first: the actuator's own evidence (DET-01).
        self.misses = 0 if s["order_valid"] else min(self.misses + 1, 255)
        # 3. revoke on p+1 consecutive misses (MAN-04).
        if self.holder != NODE_NONE and self.misses > self.p["patience"]:
            self._release_mandate(out, tick, EV_MANDATE_REVOKED)
        # 4. a DEMANDE can now be granted.
        self._handle_demande(out, tick, s)
        # 5. the one value applied at this tick.
        self._pick_value(out, tick, s, meas)
        cert_picked = self.certified
        cert_node_picked = self.certified_node
        if self.switch_pending:
            # The new holder has no certified second any more (DEG-07).
            self.switch_pending = False
            self.certified = False
            self.certified_node = NODE_NONE
            self.certify_streak = 0
        # 6. certification, liveness, lamp.
        self._certify(out, tick, s)
        self._liveness(out, tick)
        self._update_lamp(out, tick)
        # 7. fill the STATUT to publish and the journal events.
        self._fill_out(out, tick, meas, cert_picked, cert_node_picked)
        return self.state_dict(), out

    def state_dict(self):
        """The whole arbiter, matching the bridge's `state` object field by field."""
        return {
            "mandate": self.mandate,
            "holder": self.holder,
            "misses": self.misses,
            "switch_pending": self.switch_pending,
            "certified": self.certified,
            "certified_node": self.certified_node,
            "certify_streak": self.certify_streak,
            "shadow_misses": self.shadow_misses,
            "last_order": self.last_order,
            "last_shadow": self.last_shadow,
            "have_order": self.have_order,
            "have_shadow": self.have_shadow,
            "disagree_streak": self.disagree_streak,
            "disagreement": self.disagreement,
            "testament": list(self.testament),
            "testament_len": self.testament_len,
            "testament_base": self.testament_base,
            "fb_state": self.fb_state,
            "in_fallback": self.in_fallback,
            "last_heard": list(self.last_heard),
            "present": list(self.present),
            "ever_seen": list(self.ever_seen),
            "lamp": self.lamp,
            "cause": self.cause,
            "degraded_ticks": self.degraded_ticks,
            "stop_requested": self.stop_requested,
            "cession_pending": self.cession_pending,
            "request_window_tick": self.request_window_tick,
            "request_seen": self.request_seen,
            "rejected": list(self.rejected),
            "forged": self.forged,
            "applied_by_source": list(self.applied_by_source),
            "ticks": self.ticks,
            "handovers": self.handovers,
        }