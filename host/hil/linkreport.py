#!/usr/bin/env python3
"""Relais — analysis of one link-cut run (mission M41, INJ-04).

Reads the actuator journal of a run whose data path was cut, and reports
what the protocol did, in ticks and in protocol microseconds:

  cut tick        the last tick the actuator saw a fresh order from the
                  holder before the cut (the bench's own hint is checked
                  against the journal);
  effective cut   the first tick the actuator itself saw without a valid
                  holder order (misses >= 1): the protocol-side instant of
                  the cut;
  revoke          the tick of the mandate_revoked event that follows;
  takeover        the first tick served by the new holder's own order.

Usage: linkreport.py <journal.jsonl> --cut-tick <n> [--t 15]
"""
import argparse
import json
import os
import sys


def load(path):
    records = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            try:
                records.append(json.loads(line))
            except ValueError:
                continue
    return records


def find_cut(records, hint):
    """The instant of the cut, read from the journal alone: the last tick the
    actuator served from a fresh order, and the first tick after it that it
    served without one. The bench's own hint is only reported."""
    last_fresh, first_missing = None, None
    for rec in records:
        if rec.get("kind") != "tick":
            continue
        tick = int(rec.get("tick", 0))
        if rec.get("source") in ("holder", "shadow"):
            last_fresh = tick
            first_missing = None
        elif last_fresh is not None and first_missing is None:
            first_missing = tick
    return last_fresh, first_missing


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("journal")
    parser.add_argument("--cut-tick", type=int, required=True)
    parser.add_argument("--t", type=int, default=15)
    args = parser.parse_args()
    if not os.path.exists(args.journal):
        print("linkreport: %s missing" % args.journal)
        return 2
    records = load(args.journal)
    last_fresh, first_missing = find_cut(records, args.cut_tick)
    revoke = None
    takeover = None
    new_holder = None
    base = first_missing if first_missing is not None else args.cut_tick
    for rec in records:
        kind = rec.get("kind")
        tick = int(rec.get("tick", 0))
        if kind == "event" and rec.get("event") == "mandate_revoked" and revoke is None:
            if tick >= base:
                revoke = tick
        if kind == "tick" and takeover is None and tick > base:
            if rec.get("source") == "holder" and rec.get("holder"):
                takeover = tick
                new_holder = rec.get("holder")
    out = {
        "journal": args.journal,
        "ticks": sum(1 for r in records if r.get("kind") == "tick"),
        "cut_tick_hint": args.cut_tick,
        "last_fresh_tick": last_fresh,
        "effective_cut_tick": first_missing,
        "revoke_tick": revoke,
        "takeover_tick": takeover,
        "new_holder": new_holder,
    }
    if first_missing is not None and revoke is not None:
        out["revoke_ticks"] = revoke - first_missing + 1
        out["proto_revoke_us"] = out["revoke_ticks"] * args.t * 1000
    if first_missing is not None and takeover is not None:
        out["takeover_ticks"] = takeover - first_missing + 1
        out["proto_takeover_us"] = out["takeover_ticks"] * args.t * 1000
    print(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
