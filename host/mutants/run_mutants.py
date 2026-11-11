#!/usr/bin/env python3
"""Relais — the mutant bench (spec 7.6, INJ-11).

A mutant is the very same core, compiled with -DRELAIS_MUTANT=n, carrying one
deliberate defect. Each mutant faces a scenario chosen so that the healthy
software passes it: if the bench does not flag the mutant, the bench is at
fault, not the software. That is the whole point of this script.

Two detection channels, because a defect is not always an invariant breach:
  * the invariant checker (I1, I2, I3, I4, I5, I6, I8, I10, DET-03);
  * the actuator's journal, for the defence-in-depth cases where the arbiter
    must absorb a defective machine and hold (M7, spec 7.6).

Usage: python3 host/mutants/run_mutants.py [--only 1,4] [--keep]
Exit code 0 when every mutant is flagged.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASE_FLAGS = "-D_POSIX_C_SOURCE=200809L {mut} -Icore/include -Icore/tests -MMD -MP"

# mutant: defect, what the bench must report, scenario, extra flags
MUTANTS = {
    1: ("the actuator applies an order from anybody", "I1 / I3",
        ["--fault", "600:forge:1:0:-0.9"], ["--level", "complet"]),
    2: ("the actuator ignores the tick label", "I3 / I5",
        ["--fault", "600:forge-stale:0:0:-0.9"], ["--level", "complet"]),
    3: ("the mandate is granted with no evidence of silence", "DET-03",
        ["--fault", "600:freeze:0:1"], ["--level", "complet"]),
    4: ("the will is played past its horizon", "I6 / I8",
        ["--fault", "400:death:0", "--fault", "430:death:1"], []),
    5: ("the second keeps its own state instead of the replica", "I4 / DET-09",
        ["--fault", "640:restart:1", "--fault", "650:death:0"], ["--level", "complet"]),
    6: ("the mandate restarts at zero when the actuator restarts", "I2",
        ["--restart-actuator", "800"], ["--level", "complet"]),
    7: ("the second orders on a mere suspicion, without the grant", "MAN-08 (journal)",
        ["--fault", "600:freeze:0:20"], ["--level", "complet"]),
    8: ("the lamp goes out while redundancy is still lost", "I10",
        ["--fault", "400:death:1"], ["--level", "complet"]),
}
COMMON = ["--ticks", "1500", "--seed", "5"]


def build(mutant):
    """Build one simulator: mutant n, or the nominal core when mutant is 0."""
    build_dir = "build-m%d" % mutant if mutant else "build-nominal"
    flags = BASE_FLAGS.format(mut="-DRELAIS_MUTANT=%d" % mutant if mutant else "")
    cmd = ["make", "BUILD=%s" % build_dir, "CPPFLAGS=%s" % flags,
           "%s/relais-simulate" % build_dir]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write((proc.stdout + proc.stderr)[-2500:])
        raise SystemExit("build failed for mutant %s" % mutant)
    return os.path.join(ROOT, build_dir, "relais-simulate")


def run(binary, scenario, flags):
    """Run one scenario, return (exit_code, first violation, journal events)."""
    with tempfile.TemporaryDirectory() as tmp:
        journal = os.path.join(tmp, "actuator.jsonl")
        proc = subprocess.run([binary] + COMMON + flags + scenario +
                              ["--journal", journal],
                              cwd=ROOT, capture_output=True, text=True, timeout=600)
        out = proc.stdout + proc.stderr
        match = re.search(r"first: (.+)", out)
        first = match.group(1).strip() if match else ""
        events = collections.Counter()
        not_holder = 0
        with open(journal, encoding="utf-8") as fh:
            for line in fh:
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if record.get("kind") != "event":
                    continue
                events[record.get("event", "?")] += 1
                if record.get("event") == "order_rejected" and record.get("detail") == 1:
                    not_holder += 1
    return proc.returncode, first, events, not_holder


def flagged(mutant, code, first, events, not_holder):
    """Did the bench see the defect? M7 is defence in depth: the arbiter holds
    (no violation) while the journal shows a machine that is not the holder
    trying to command (spec 7.6, MAN-08)."""
    if mutant == 7:
        return code == 0 and not_holder > 0
    if mutant == 5:
        return code != 0 or events["disagreement"] + events["decertified"] > 0
    return code != 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="comma separated mutant ids, e.g. 1,4")
    ap.add_argument("--keep", action="store_true", help="keep the mutant builds")
    args = ap.parse_args()
    wanted = [int(x) for x in args.only.split(",") if x.strip()] or sorted(MUTANTS)

    print("== Relais mutant bench (spec 7.6) ==")
    nominal = build(0)
    rows = []
    for mutant in wanted:
        defect, expected, scenario, flags = MUTANTS[mutant]
        code, first, events, not_holder = run(nominal, scenario, flags)
        if mutant != 7 and code != 0:
            rows.append((mutant, "BAD-PROBE", defect, expected,
                         "the nominal core fails this scenario: %s" % first))
            continue
        if mutant == 7 and not_holder != 0:
            rows.append((mutant, "BAD-PROBE", defect, expected,
                         "the nominal core already orders without a grant"))
            continue
        binary = build(mutant)
        code, first, events, not_holder = run(binary, scenario, flags)
        ok = flagged(mutant, code, first, events, not_holder)
        detail = first or "orders from a non-holder: %d, disagreements: %d" % (
            not_holder, events["disagreement"])
        rows.append((mutant, "FLAGGED" if ok else "MISSED", defect, expected, detail))
        if not args.keep:
            subprocess.run(["rm", "-rf", os.path.join(ROOT, "build-m%d" % mutant)])

    width = max(len(r[2]) for r in rows)
    print("\n%-4s %-10s %-*s %-18s %s" % ("M", "verdict", width, "defect",
                                           "must report", "evidence"))
    for mutant, verdict, defect, expected, detail in rows:
        print("M%-3d %-10s %-*s %-18s %s" % (mutant, verdict, width, defect,
                                             expected, detail[:80]))
    caught = sum(1 for r in rows if r[1] == "FLAGGED")
    print("\n%d/%d mutants flagged" % (caught, len(rows)))
    if not args.keep:
        subprocess.run(["rm", "-rf", os.path.join(ROOT, "build-nominal")])
    return 0 if caught == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
