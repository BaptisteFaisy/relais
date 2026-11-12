"""Relais — a world whose setpoint comes from a table, not from the C generator.

Idea 62 (real dataset replay) needs steering-wheel angles read from a CSV file,
played as the setpoint of the protocol. The C trajectory generator (plant.c)
only knows analytic profiles, and the mission forbids touching core/, so this
module drives the REAL C arbiter through the differential-test bridge
(host/difftest/bridge.c) and runs the two computers as the independent Python
mirror of host/difftest/node_py.py, extended with the float32 control law of
host/physics/plant_law.py (validated tick by tick against C journals).

The tick pipeline mirrors host/simulate/sim.c exactly:
  faults -> pending frames -> C arbiter step -> rack step -> STATUT published
  with post-step measurements -> holder computes first, its ETAT reaches the
  second inside the same tick (REP-01) -> the second shadows on the replicated
  state, so order and shadow agree bit for bit (CMD-03, DET-09).

The journal written out has the exact C format (CONTRACT.md), so
build/relais-verify and host/report/analysis.py read it unchanged.

Known simplifications versus sim.c (documented, they do not touch safety):
  - no frame loss / delay / duplication model (the study is a clean replay);
  - ETAT carries the full law state as a dict instead of 30 packed bytes;
  - node_py does not implement REP-04 reconstruction: after a freeze, the
    second resynchronises through the next ETAT instead (same end state);
  - road disturbance is zero unless the caller passes one.
"""
from __future__ import annotations

import json
import os
import random
import sys
from typing import Callable, Dict, List, Optional, Sequence, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_HOST = os.path.dirname(_HERE)
for _inc in (os.path.join(_HOST, "difftest"), _HERE):
    if _inc not in sys.path:
        sys.path.insert(0, _inc)

import node_py                      # noqa: E402  the independent node mirror
import plant_law as pl              # noqa: E402  the float32 law/rack mirror
import difftest                     # noqa: E402  for the bridge binary path
from difftest import Bridge         # noqa: E402  the C arbiter over JSON lines

MSG_ETAT = node_py.MSG_ETAT
MSG_SUIVI = node_py.MSG_SUIVI
MSG_DEMANDE = node_py.MSG_DEMANDE
ACTUATOR_TYPES = (node_py.MSG_ORDRE, node_py.MSG_OMBRE, MSG_DEMANDE,
                  node_py.MSG_CESSION)

NODE_NAMES = {0: "", 1: "A", 2: "B", 3: "C", 4: "actuator"}
SOURCE_NAMES = {0: "none", 1: "holder", 2: "shadow", 3: "testament", 4: "fallback"}
LAMP_NAMES = {0: "off", 1: "yellow", 2: "red"}
CAUSE_NAMES = {0: "none", 1: "node_missing", 2: "link_lost",
               3: "second_uncertified", 4: "disagreement", 5: "fallback",
               6: "stop_requested"}
EVENT_NAMES = {0: "none", 1: "mandate_granted", 2: "mandate_revoked",
               3: "mandate_auto_switch", 4: "request_denied", 5: "certified",
               6: "decertified", 7: "order_rejected", 8: "disagreement",
               9: "fallback_enter", 10: "fallback_exit", 11: "cession",
               12: "lamp_change", 13: "stop_requested", 14: "forged",
               15: "node_back"}


def protocol_params(study: Dict[str, float]) -> Dict[str, float]:
    """Translate a study-style param dict (T, p, N, ...) into the protocol
    keys the bridge and node_py expect (period_ms, patience, ...)."""
    p = node_py.default_params()
    table = {"T": "period_ms", "p": "patience", "N": "testament_len",
             "certify": "certify_ticks", "bail": "bail_ticks",
             "eps": "epsilon", "delta": "delta", "degraded": "degraded_ticks",
             "amp": "max_amplitude", "slew": "max_slew"}
    for short, long in table.items():
        if short in study:
            p[long] = study[short]
    return p


class PhysicsNode(node_py.Node):
    """The node mirror with the real control law and full state replication."""

    def __init__(self, self_node: int, params: Dict[str, float]):
        super().__init__(self_node, params)
        self.phys = {"T": params["period_ms"], "N": params["testament_len"],
                     "amp": params["max_amplitude"], "slew": params["max_slew"]}
        self.ctl = pl.CtlState()
        self._last_meas = {"pos": 0.0, "vel": 0.0, "applied": 0.0, "setpoint": 0.0}

    def _compute(self, meas):  # CMD-02: the float32 mirror of relais_ctl_step
        self._last_meas = dict(meas)
        return self.ctl.step(meas, self.phys)

    def _will(self, value):  # ACT-02: model prediction, like the C core
        return pl.testament_model(self.ctl, self._last_meas, self.phys, [])

    @staticmethod
    def _fix_will(frame: Dict) -> Dict:
        frame["testament_len"] = len(frame.get("testament") or [])
        return frame

    def _ordre(self, tick, value):
        return self._fix_will(super()._ordre(tick, value))

    def _demande(self, tick, with_order, value):
        return self._fix_will(super()._demande(tick, with_order, value))

    def on_status(self, statut):
        """Pre-compute snapshot rides along the ETAT (REP-01, see node.c)."""
        pre = self.ctl.snapshot()
        frames = super().on_status(statut)
        for f in frames:
            if f.get("type") == MSG_ETAT:
                f["ctl"] = pre
        # DET-02 parity: the C second also asks when the mandate is VACANT,
        # node_py only asks on misses > 0; without this the world never boots.
        if (statut.get("holder", 0) == node_py.NODE_NONE
                and self.role == node_py.ROLE_SECOND
                and not any(f.get("type") == MSG_DEMANDE for f in frames)):
            frames.append(self._demande(self.now + 1, True, self.last_value))
            self.demandes_sent += 1
        return frames

    def on_etat(self, etat):
        before = self.last_etat
        out = super().on_etat(etat)
        if self.last_etat != before and "ctl" in etat:
            self.ctl.restore(etat["ctl"])  # REP-02 on the full law state
        return out


class DatasetWorld:
    """Two PhysicsNodes, one C arbiter (bridge), one mirrored rack.

    Optional loss model (ROB-03, corrected): a frame of one link is dropped
    with probability `loss`, and the burst guard never lets more than
    max(p, 1) CONSECUTIVE frames of one link be dropped — exactly the
    hypothesis of the claim ("a burst shorter than p must not move the
    mandate"). The C simulator's own guard (sim.c sim_lose) never resets
    lost_last, so after one loss a link never loses again; that defect is
    described in the M31 report and worked around here, not hidden.
    """

    def __init__(self, params: Optional[Dict[str, float]] = None,
                 bridge_path: Optional[str] = None, complete: bool = True,
                 loss: float = 0.0, seed: int = 1):
        self.study_params = dict(pl.default_params())
        if params:
            self.study_params.update(params)
        proto = protocol_params(self.study_params)
        self.proto = proto
        self.complete = complete  # level C keeps OMBRE; level S drops it (sim.c)
        self.bridge = Bridge(path=bridge_path or difftest.BRIDGE, params=proto)
        self.nodes = [PhysicsNode(node_py.NODE_A, proto),
                      PhysicsNode(node_py.NODE_B, proto)]
        self.plant = pl.Plant(0.0)
        self.dt = pl.step_s(self.study_params)
        self.pending: List[Tuple[int, Dict]] = []
        self.rows: List[Dict] = []
        self.faults: Dict[int, List[Tuple[str, int, int]]] = {}
        self.dead = [False, False]
        self.frozen_until = [0, 0]
        self.loss = float(loss)
        self.rng = random.Random(seed)
        self.max_burst = max(int(self.study_params["p"]), 1)
        # ONE burst counter per node link, shared by both directions, exactly
        # like sim.c (sim_lose indexes by node): the timing hypothesis of
        # ROB-03 is per link, not per direction.
        self.link_burst = [0, 0]
        self.dropped_frames = 0

    def _drop(self, link: int) -> bool:
        """One loss draw with the corrected burst guard; True = frame lost."""
        if self.loss <= 0.0:
            return False
        if self.rng.random() >= self.loss:
            self.link_burst[link] = 0
            return False
        if self.link_burst[link] >= self.max_burst:
            return False  # a longer burst is outside the timing hypothesis
        self.link_burst[link] += 1
        self.dropped_frames += 1
        return True

    def add_fault(self, at_tick: int, kind: str, node: int, arg: int = 0) -> None:
        """Schedule "death" or "freeze" (arg = ticks) on node 0 (A) or 1 (B)."""
        self.faults.setdefault(at_tick, []).append((kind, node, arg))

    def _apply_faults(self, tick: int) -> None:
        for kind, node, arg in self.faults.get(tick, []):
            if kind == "death":
                self.dead[node] = True
            elif kind == "freeze":
                self.frozen_until[node] = tick + arg

    def _collect(self, tick: int) -> List[Dict]:
        keep = [p for p in self.pending if p[0] > tick]
        msgs = [m for at, m in self.pending if at <= tick]
        self.pending = keep
        return msgs

    def _route(self, frames: Sequence[Dict], src: int, tick: int) -> None:
        peer = self.nodes[1 - src]
        for f in frames:
            kind = f.get("type")
            if kind == MSG_ETAT:
                if not self._drop(src):  # ETAT rides the node link
                    peer.on_etat(f)
            elif kind == MSG_SUIVI:
                pass  # the C receiver only touches diagnostics (node.c)
            elif kind == node_py.MSG_OMBRE and not self.complete:
                pass  # level S has no shadow: sim.c never delivers the OMBRE
            elif kind in ACTUATOR_TYPES:
                if not self._drop(src):
                    self.pending.append((tick + 1, f))

    def _run_node(self, idx: int, statut: Dict, tick: int) -> None:
        nd = self.nodes[idx]
        if self.dead[idx] or self.frozen_until[idx] >= tick:
            return
        if self._drop(idx):
            nd.on_silence(tick)  # MAN-09 bail; frames dropped, as in sim.c
            return
        self._route(nd.on_status(statut), idx, tick)

    def _tick_row(self, tick: int, out: Dict, setpoint: float) -> Dict:
        return {"kind": "tick", "tick": tick, "ns": 0,
                "mandate": out["mandate"], "holder": NODE_NAMES[out["holder"]],
                "source": SOURCE_NAMES[out["source"]], "value": out["value"],
                "pos": self.plant.pos, "vel": self.plant.vel,
                "setpoint": setpoint, "misses": out["misses"],
                "lamp": LAMP_NAMES[out["lamp"]], "cause": CAUSE_NAMES[out["cause"]],
                "certified": out["certified"],
                "certified_node": NODE_NAMES[out["certified_node"]],
                "stop": out["stop_requested"]}

    def _event_rows(self, tick: int, out: Dict) -> List[Dict]:
        rows = []
        for e in out.get("events", []):
            rows.append({"kind": "event", "tick": tick, "ns": 0,
                         "event": EVENT_NAMES[e["kind"]],
                         "node": NODE_NAMES[e["node"]], "mandate": e["mandate"],
                         "detail": e["detail"],
                         "text": EVENT_NAMES[e["kind"]]})
        return rows

    def run(self, ticks: int, setpoint_at: Callable[[int], float],
            disturbance_at: Optional[Callable[[int], float]] = None) -> List[Dict]:
        """Run `ticks` ticks; setpoint_at(tick) feeds the CSV table (CMD-02)."""
        for tick in range(1, ticks + 1):
            self._apply_faults(tick)
            msgs = self._collect(tick)
            setpoint = float(setpoint_at(tick))
            meas = {"pos": self.plant.pos, "vel": self.plant.vel,
                    "setpoint": setpoint, "applied": 0.0}
            resp = self.bridge.step(tick, msgs, meas)
            out = resp["out"]
            dist = float(disturbance_at(tick)) if disturbance_at else 0.0
            self.plant.step(out["value"], dist, self.dt)
            statut = out["statut"]  # publish post-step measurements, like sim.c
            statut["meas"]["pos"] = self.plant.pos
            statut["meas"]["vel"] = self.plant.vel
            statut["meas"]["applied"] = out["value"]
            self.rows.append(self._tick_row(tick, out, setpoint))
            self.rows.extend(self._event_rows(tick, out))
            first = 1 if statut["holder"] == node_py.NODE_B else 0
            self._run_node(first, statut, tick)
            self._run_node(1 - first, statut, tick)
        return self.rows

    def write_journal(self, path: str, scenario: str = "dataset",
                      scenario_id: int = 0) -> None:
        """Write the run in the authoritative journal format (spec 6.4)."""
        p = self.study_params
        header = {"kind": "header", "version": 1, "mutant": 0,
                  "scenario": scenario, "scenario_id": scenario_id,
                  "params": {"T": int(p["T"]), "p": int(p["p"]), "N": int(p["N"]),
                             "certify": int(p["certify"]), "bail": int(p["bail"]),
                             "eps": float(p["eps"]), "delta": float(p["delta"]),
                             "degraded": int(p["degraded"])}}
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(header) + "\n")
            for row in self.rows:
                fh.write(json.dumps(row) + "\n")

    def close(self) -> None:
        self.bridge.close()
