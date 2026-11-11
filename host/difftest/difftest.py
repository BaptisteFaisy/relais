#!/usr/bin/env python3
"""Relais — differential confrontation of the arbiter: C vs Python (M13).

Generates frame/measurement sequences, replays them through the C arbiter (via
bridge.c) and through the independent Python reimplementation (arbiter_py.py),
and compares the full state and the applied value after EVERY tick. On the first
disagreement it dumps both states, the replayable sequence, and the faulty tick
— a comparator that never finds a divergence proves nothing, so finding one (or
convincingly not finding one over millions of ticks) is the point.

Usage:
    python3 host/difftest/difftest.py --runs 200 --seed 1
Standard library only.
"""

import argparse
import json
import math
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from arbiter_py import Arbiter, default_params  # noqa: E402
from arbiter_py import (NODE_A, NODE_B, NODE_NONE, NODE_ACTUATOR,  # noqa: E402
                        MSG_ORDRE, MSG_OMBRE, MSG_DEMANDE, MSG_CESSION,
                        TESTAMENT_MAX)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BRIDGE = os.path.join(REPO_ROOT, "build", "relais-bridge")

FLOAT_TOL = 1e-6


def floats_equal(a, b, tol=FLOAT_TOL):
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


def diff_paths(left, right, path=""):
    """Recursive structural comparison; returns the list of differing paths.

    Every JSON number is compared as a float with a small tolerance, so a 1-ULP
    difference in the control math does not mask a real protocol divergence.
    Nonexistent keys, type changes and length changes are reported verbatim.
    """
    diffs = []
    if isinstance(left, dict) and isinstance(right, dict):
        for k in sorted(set(left) | set(right)):
            if k not in left or k not in right:
                diffs.append(path + "." + k + " (missing on one side)")
            else:
                diffs.extend(diff_paths(left[k], right[k], path + "." + k))
    elif isinstance(left, list) and isinstance(right, list):
        if len(left) != len(right):
            diffs.append("%s (length %d vs %d)" % (path, len(left), len(right)))
        for i in range(min(len(left), len(right))):
            diffs.extend(diff_paths(left[i], right[i], "%s[%d]" % (path, i)))
    elif isinstance(left, (int, float)) and isinstance(right, (int, float)):
        if not floats_equal(float(left), float(right)):
            diffs.append("%s (%r vs %r)" % (path, left, right))
    elif isinstance(left, bool) and isinstance(right, bool):
        if left != right:
            diffs.append("%s (%r vs %r)" % (path, left, right))
    else:
        if left != right:
            diffs.append("%s (%r vs %r)" % (path, left, right))
    return diffs


def make_order(sender, tick, mandate, value, testament=None, tlen=0):
    m = {"type": MSG_ORDRE, "sender": sender, "tick": tick, "mandate": mandate,
         "value": value, "testament": testament or [0.0] * TESTAMENT_MAX,
         "testament_len": tlen}
    return m


def make_shadow(sender, tick, value):
    return {"type": MSG_OMBRE, "sender": sender, "tick": tick, "value": value,
            "replica_age": 0}


def make_demande(sender, seen, tick=0, with_order=False, value=0.0, testament=None,
                 tlen=0, reason=1):
    m = {"type": MSG_DEMANDE, "sender": sender, "seen_mandate": seen,
         "reason": reason, "has_order": with_order, "tick": tick, "value": value,
         "testament": testament or [0.0] * TESTAMENT_MAX, "testament_len": tlen}
    return m


def make_cession(sender, mandate, reason=1):
    return {"type": MSG_CESSION, "sender": sender, "mandate": mandate, "reason": reason}


def nominal_sequence():
    """A deterministic scenario that walks grant -> certify -> handover ->
    testament -> fallback, so the confrontation reaches the interesting states."""
    seq = []
    meas = {"pos": 0.0, "vel": 0.0, "applied": 0.0, "setpoint": 0.3}

    def tick(t, msgs, m=None):
        seq.append({"tick": t, "msgs": msgs, "meas": dict(m or meas)})

    a_will = [0.20 + 0.01 * i for i in range(8)]
    b_will = [0.26 + 0.01 * i for i in range(8)]

    # Grant: A asks (and carries its first order targeting tick 1).
    tick(1, [make_demande(NODE_A, 0, tick=1, with_order=True, value=0.20,
                          testament=a_will, tlen=8)])
    # A holds, B shadows in agreement for three ticks -> certification.
    for t in range(2, 5):
        v = 0.20 + 0.01 * (t - 1)
        tick(t, [make_order(NODE_A, t, 1, v), make_shadow(NODE_B, t, v)])
    # A still holds, certified.
    tick(5, [make_order(NODE_A, 5, 1, 0.24), make_shadow(NODE_B, 5, 0.24)])
    # A dies: B's shadow substitutes (certified second), one miss.
    tick(6, [make_shadow(NODE_B, 6, 0.25)])
    # Second consecutive miss -> auto-switch, mandate to B.
    tick(7, [make_shadow(NODE_B, 7, 0.26)])
    # B is holder now, A is the second; B's order carries its will.
    for t in range(8, 11):
        v = 0.26 + 0.01 * (t - 7)
        tick(t, [make_order(NODE_B, t, 2, v, testament=b_will, tlen=8),
                 make_shadow(NODE_A, t, v)])
    # BOTH die after tick 10: the will of B plays out (ticks 11..18), then the
    # local fallback law takes over past the horizon.
    for t in range(11, 26):
        tick(t, [])
    return seq


def random_sequence(rng, ticks):
    """Random frames: delivered, lost, duplicated, reordered, plus faults."""
    seq = []
    senders = [NODE_A, NODE_B]
    for t in range(1, ticks + 1):
        msgs = []
        count = rng.randint(0, 8)
        for _ in range(count):
            sender = rng.choice(senders)
            kind = rng.choice(["order", "shadow", "demande", "cession"])
            if kind == "order":
                msgs.append(make_order(sender, t + rng.randint(-2, 2),
                                       rng.randint(0, 3),
                                       round(rng.uniform(-1.5, 1.5), 6)))
            elif kind == "shadow":
                msgs.append(make_shadow(sender, t + rng.randint(-1, 1),
                                        round(rng.uniform(-1.5, 1.5), 6)))
            elif kind == "demande":
                msgs.append(make_demande(sender, rng.randint(0, 3),
                                         tick=t, reason=rng.randint(0, 3)))
            else:
                msgs.append(make_cession(sender, rng.randint(0, 3)))
        # Reorder the frames: the arbiter must be insensitive to delivery order
        # except where the protocol says the last frame wins.
        rng.shuffle(msgs)
        meas = {"pos": round(rng.uniform(-1.0, 1.0), 6),
                "vel": round(rng.uniform(-1.0, 1.0), 6),
                "applied": round(rng.uniform(-1.0, 1.0), 6),
                "setpoint": round(rng.uniform(-1.0, 1.0), 6)}
        seq.append({"tick": t, "msgs": msgs, "meas": meas})
    return seq


class Counterexample:
    def __init__(self, tick, diffs, c_state, py_state, c_out, py_out, seq):
        self.tick = tick
        self.diffs = diffs
        self.c_state = c_state
        self.py_state = py_state
        self.c_out = c_out
        self.py_out = py_out
        self.seq = seq

    def dump(self):
        lines = ["=== DISAGREEMENT at tick %d ===" % self.tick,
                 "differences:"] + ["  " + d for d in self.diffs]
        lines.append("--- C state ---")
        lines.append(json.dumps(self.c_state, sort_keys=True))
        lines.append("--- Python state ---")
        lines.append(json.dumps(self.py_state, sort_keys=True))
        lines.append("--- C out ---")
        lines.append(json.dumps(self.c_out, sort_keys=True))
        lines.append("--- Python out ---")
        lines.append(json.dumps(self.py_out, sort_keys=True))
        return "\n".join(lines)


class Bridge:
    """A persistent child process exposing the C arbiter over JSON lines."""

    def __init__(self, path=BRIDGE, params=None):
        if not os.path.exists(path):
            raise RuntimeError("bridge binary missing: %s (run `make -j4 all`)" % path)
        self.proc = subprocess.Popen([path], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True,
                                     bufsize=1)
        self._send_init(params or default_params())

    def _send_init(self, params):
        self._write({"cmd": "init", "params": params})
        self._read()

    def reset(self, params=None):
        """Re-initialise the C arbiter in place, to reuse one child process."""
        self._send_init(params or default_params())

    def _write(self, obj):
        self.proc.stdin.write(json.dumps(obj) + "\n")
        self.proc.stdin.flush()

    def _read(self):
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("bridge exited early")
        return json.loads(line)

    def step(self, tick, msgs, meas):
        self._write({"cmd": "step", "tick": tick, "msgs": msgs, "meas": meas})
        return self._read()

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.stdout.close()
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()


def _run_one(bridge, py, seq):
    """Replay one sequence on an already-open bridge and Python arbiter."""
    for step_cfg in seq:
        t = step_cfg["tick"]
        msgs = step_cfg["msgs"]
        meas = step_cfg["meas"]
        c = bridge.step(t, msgs, meas)
        py_state, py_out = py.step(t, msgs, meas)
        diffs = []
        diffs.extend(diff_paths(c.get("state", {}), py_state, "state"))
        diffs.extend(diff_paths(c.get("out", {}), py_out, "out"))
        if diffs:
            return Counterexample(t, diffs, c.get("state", {}), py_state,
                                  c.get("out", {}), py_out, seq)
    return None


def run_sequence(seq, bridge=None, py=None, params=None):
    """Replay a sequence on both implementations; return None or a Counterexample."""
    own_bridge = bridge is None
    if own_bridge:
        bridge = Bridge(params=params)
    if py is None:
        py = Arbiter(params)
    try:
        return _run_one(bridge, py, seq)
    finally:
        if own_bridge:
            bridge.close()


def run_many(seqs, params=None):
    """Replay many sequences reusing ONE bridge child (fast path for big runs)."""
    bridge = Bridge(params=params)
    try:
        for seq in seqs:
            bridge.reset(params)
            cex = _run_one(bridge, Arbiter(params), seq)
            if cex is not None:
                return cex
        return None
    finally:
        bridge.close()


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--ticks", type=int, default=60)
    ap.add_argument("--mode", default="random", choices=["random", "nominal"])
    ap.add_argument("--json", action="store_true", help="dump counterexample as JSON")
    args = ap.parse_args(argv)

    rng = random.Random(args.seed)
    sequences = []
    if args.mode == "nominal" or args.runs < 1:
        sequences.append(nominal_sequence())
    for _ in range(args.runs):
        sequences.append(random_sequence(rng, args.ticks))

    total_ticks = sum(len(s) for s in sequences)
    cex = run_many(sequences)
    if cex is not None:
        print("DISAGREEMENT (seed=%d)" % args.seed)
        print(cex.dump())
        if args.json:
            print(json.dumps({"tick": cex.tick, "diffs": cex.diffs,
                              "c_state": cex.c_state, "py_state": cex.py_state,
                              "c_out": cex.c_out, "py_out": cex.py_out,
                              "seq": cex.seq}, indent=2))
        return 1
    print("AGREEMENT: %d sequences, %d ticks, C == Python" % (len(sequences), total_ticks))
    return 0


if __name__ == "__main__":
    sys.exit(main())