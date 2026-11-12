"""Deterministic replay of a Relais run (idea 42, OBS-07).

Replays an actuator journal and answers one question: is the journal complete
enough to reproduce the run exactly? Two engines:

  1. intrinsic replay (always, no external input): re-derive the actuator's own
     decision state -- the miss counter and the mandate number -- from the
     recorded evidence and compare it tick by tick. If a field is missing or
     wrong in the journal, the derivation sees an inconsistency and reports
     "divergent at tick N, field X". This is the real completeness test.

  2. simulator replay (when the external inputs are supplied): re-run
     build/relais-simulate with the same parameters and diff the two journals
     field by field.

The journal header records version, mutant, scenario, scenario_id and params,
but NOT the seed, the level (socle/complet) or the fault schedule. Those are
required to reproduce a run byte-for-byte, so the simulator replay needs them
on the command line; their absence from the journal is itself a completeness
finding that the tool reports rather than hides.

Usage:
    python3 host/replay/replay.py --journal /tmp/x.jsonl
    python3 host/replay/replay.py --journal /tmp/x.jsonl --seed 3 --level complet \
        --fault 500:death:0
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from typing import Dict, List, Optional, Sequence, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
for _inc in (os.path.join(_HERE, "..", "report"), os.path.join(_HERE, "..", "edr")):
    if _inc not in sys.path:
        sys.path.insert(0, _inc)

from journal_io import Journal, read_journal  # noqa: E402

# Fields compared in a simulator replay; floats carry a tolerance.
FIELDS = ("tick", "holder", "mandate", "source", "misses", "lamp", "cause")
FLOAT_FIELDS = ("value", "pos", "vel", "setpoint")
COMPARED = FIELDS + FLOAT_FIELDS
EPS = 1e-5


def derive_misses_and_mandate(ticks) -> List[Tuple[int, int]]:
    """Re-derive (misses, mandate) per tick from the evidence (MAN-03, DET-01).

    Matches arbiter.c step order: the miss counter resets to zero when a fresh
    holder order is applied (source == "holder"); it climbs otherwise. On a
    grant or auto-switch (new non-vacant holder) the counter is reset and the
    mandate steps (MAN-03); on a revoke-to-vacant it keeps counting (the old
    holder missed again) and the mandate is unchanged.
    """
    out: List[Tuple[int, int]] = []
    misses = 0
    mandate = 0
    prev_holder = None
    for t in ticks:
        if prev_holder is None:
            mandate = t.mandate
            misses = 0 if t.source == "holder" else 1
        elif t.holder != prev_holder:
            if t.holder:  # grant / auto-switch to a new holder
                mandate += 1
                misses = 0
            else:  # revoke to vacancy: the miss is counted, nothing else moves
                misses = 0 if t.source == "holder" else min(misses + 1, 255)
        else:
            misses = 0 if t.source == "holder" else min(misses + 1, 255)
        out.append((misses, mandate))
        prev_holder = t.holder
    return out


def intrinsic_verdict(ticks) -> Tuple[str, Optional[str]]:
    """Verdict of the decision replay: 'identical' or the first divergence."""
    derived = derive_misses_and_mandate(ticks)
    for i, t in enumerate(ticks):
        dm, dd = derived[i]
        if t.misses != dm:
            return ("divergent", "at tick %d, field misses (recorded %d, expected %d)"
                    % (t.tick, t.misses, dm))
        if t.mandate != dd:
            return ("divergent", "at tick %d, field mandate (recorded %d, expected %d)"
                    % (t.tick, t.mandate, dd))
    return ("identical", None)


def _sim_bin() -> str:
    return os.path.join(_ROOT, "build", "relais-simulate")


def _params_arg(params: Dict[str, float]) -> str:
    keys = ("T", "p", "N", "certify", "bail", "eps", "delta", "degraded")
    items = []
    for k in keys:
        if k in params:
            items.append("%s=%g" % (k, params[k]))
    return ",".join(items)


def _max_tick(act: Journal) -> int:
    if not act.ticks:
        return 0
    return max(t.tick for t in act.ticks)


def run_simulator(act: Journal, seed: int, level: str, faults: Sequence[str]) -> str:
    """Re-run the seeded simulator and return the path of the new journal."""
    ticks = _max_tick(act)
    cmd = [_sim_bin(), "--ticks", str(ticks), "--seed", str(seed),
           "--level", level, "--quiet"]
    if act.header is not None:
        cmd += ["--params", _params_arg(act.header.params)]
    for f in faults:
        cmd += ["--fault", f]
    fd, path = tempfile.mkstemp(suffix=".jsonl")
    os.close(fd)
    cmd += ["--journal", path]
    subprocess.run(cmd, check=True, cwd=_ROOT,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    return path


def compare_journals(act: Journal, replayed: Journal) -> Tuple[str, Optional[str]]:
    """Diff two journals tick by tick; first mismatch wins."""
    replay_ticks = {t.tick: t for t in replayed.ticks}
    for t in act.ticks:
        r = replay_ticks.get(t.tick)
        if r is None:
            return ("divergent", "tick %d absent from the replayed run" % t.tick)
        for f in FIELDS:
            a, b = getattr(t, f), getattr(r, f)
            if a != b:
                return ("divergent", "at tick %d, field %s (recorded %r, replayed %r)"
                        % (t.tick, f, a, b))
        for f in FLOAT_FIELDS:
            if abs(getattr(t, f) - getattr(r, f)) > EPS:
                return ("divergent", "at tick %d, field %s (recorded %g, replayed %g)"
                        % (t.tick, f, getattr(t, f), getattr(r, f)))
    if len(replay_ticks) != len(act.ticks):
        return ("divergent", "tick count differs (%d vs %d)"
                % (len(replay_ticks), len(act.ticks)))
    return ("identical", None)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Deterministic replay of a Relais run")
    ap.add_argument("--journal", required=True, help="actuator JSON-lines journal")
    ap.add_argument("--seed", type=int, default=None, help="seed (required for the simulator replay)")
    ap.add_argument("--level", choices=("socle", "complet"), default="socle")
    ap.add_argument("--fault", action="append", default=[], help="fault spec, repeatable")
    args = ap.parse_args(argv)

    act = read_journal(args.journal)
    verdict, where = intrinsic_verdict(act.ticks)
    print("rejeu intrinseque: %s%s" % (verdict, ("" if where is None else " -> " + where)))

    if args.seed is None:
        print("rejeu simulateur: non joue (seed/level/faults absents du journal)")
        return 0
    path = run_simulator(act, args.seed, args.level, args.fault)
    replayed = read_journal(path)
    verdict2, where2 = compare_journals(act, replayed)
    print("rejeu simulateur: %s%s" % (verdict2, ("" if where2 is None else " -> " + where2)))
    os.unlink(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())