#!/usr/bin/env python3
"""Relais — C against Rust, tick by tick (mission M32, idea 74).

The Rust arbiter of ports/rust speaks the SAME JSON-lines protocol as
host/difftest/bridge.c, so this driver reuses the existing machinery instead of
inventing a second one: the sequence generators (nominal + random), the
persistent-child `Bridge` class and the recursive comparator `diff_paths` all
come from host/difftest/difftest.py. The only thing swapped is the executable.

Every sequence is replayed through both children and the FULL state and the FULL
decision are compared after every tick. Floats are compared with the comparator's
own tolerance (1e-6 relative); integers, enums and booleans exactly.

    python3 ports/rust/difftest/run_difftest.py --runs 200 --seed 1

Standard library only. Exit status: 0 on agreement, 1 on a disagreement, 0 with
a MISSING verdict when one of the two bridges is not built.
"""

import argparse
import json
import os
import random
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, "host", "difftest"))

from difftest import Bridge, diff_paths, nominal_sequence, random_sequence  # noqa: E402
from arbiter_py import default_params  # noqa: E402

C_BRIDGE = os.path.join(REPO, "build", "relais-bridge")
RUST_BRIDGE = os.path.join(REPO, "ports", "rust", "target", "debug", "relais-bridge-rs")


def missing():
    """Honest verdict when a side is not built: report it, do not fail red."""
    gaps = []
    if not os.path.exists(C_BRIDGE):
        gaps.append("C bridge %s (run `make -j4 all`)" % C_BRIDGE)
    if not os.path.exists(RUST_BRIDGE):
        gaps.append("Rust bridge %s (run `cargo build` in ports/rust, or `make ports-rust`)"
                    % RUST_BRIDGE)
    if gaps:
        print("RUST-DIFFTEST-SKIPPED: missing " + "; ".join(gaps))
        return True
    return False


def compare_sequences(seqs, params, verbose):
    """Replay every sequence through both children; return a counterexample."""
    c_side = Bridge(path=C_BRIDGE, params=params)
    rs_side = Bridge(path=RUST_BRIDGE, params=params)
    ticks = 0
    try:
        for seq in seqs:
            c_side.reset(params)
            rs_side.reset(params)
            for step in seq:
                tick, msgs, meas = step["tick"], step["msgs"], step["meas"]
                got_c = c_side.step(tick, msgs, meas)
                got_rs = rs_side.step(tick, msgs, meas)
                ticks += 1
                diffs = diff_paths(got_c.get("state", {}), got_rs.get("state", {}), "state")
                diffs += diff_paths(got_c.get("out", {}), got_rs.get("out", {}), "out")
                if diffs:
                    return {"tick": tick, "diffs": diffs, "c": got_c, "rs": got_rs, "ticks": ticks}
    finally:
        c_side.close()
        rs_side.close()
    return {"tick": None, "ticks": ticks}


def main(argv=None):
    ap = argparse.ArgumentParser(description="C vs Rust arbiter confrontation")
    ap.add_argument("--runs", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--ticks", type=int, default=60)
    ap.add_argument("--mode", default="both", choices=["random", "nominal", "both"])
    ap.add_argument("--json", action="store_true", help="dump the counterexample as JSON")
    args = ap.parse_args(argv)

    if missing():
        return 0

    sequences = []
    if args.mode in ("nominal", "both"):
        sequences.append(nominal_sequence())
    if args.mode in ("random", "both"):
        rng = random.Random(args.seed)
        for _ in range(args.runs):
            sequences.append(random_sequence(rng, args.ticks))

    params = default_params()
    result = compare_sequences(sequences, params, verbose=False)
    total_ticks = sum(len(s) for s in sequences)
    if result["tick"] is not None:
        print("DISAGREEMENT at tick %d (after %d compared ticks)"
              % (result["tick"], result["ticks"]))
        for d in result["diffs"][:12]:
            print("  " + d)
        if args.json:
            print(json.dumps(result, indent=2, default=str))
        return 1
    print("RUST-DIFFTEST-OK: %d sequences, %d ticks, C == Rust on every state field "
          "and every decision" % (len(sequences), total_ticks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
