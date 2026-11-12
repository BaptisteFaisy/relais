#!/usr/bin/env python3
"""Relais -- fault-injection campaign (M18): fine hardware faults on simulated runs.

The C driver (host/faultinject/campaign.c, built as build/relais-faultinject)
runs N seeded simulated worlds, injects exactly one bit flip into each of them
and prints one JSON line per run. This script drives it and turns the lines
into the published balance: how many faults were detected, how many were
masked, which invariants held, which counters moved.

Usage:
    python3 host/faultinject/campaign.py --runs 2000 --seed 1
    python3 host/faultinject/campaign.py --runs 2000 --seed 1 --markdown

Standard library only (rule 6 of the project).
"""
import argparse
import json
import os
import subprocess
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DRIVER = os.path.join(ROOT, "build", "relais-faultinject")

# Safety invariants kept enforced during a campaign: a fine hardware fault is
# outside the modelled fault taxonomy, so continuity (I4/I5) is relaxed from
# the injection tick on, but the safety ones are not (see campaign.c).
SAFETY_INVARIANTS = "I1, I2, I3, I6, I8, I9, I10, I11, DET-03"
ZONES = ("any", "mandate", "tick", "ctlstate", "value")
MODES = ("bit", "byte", "word")
TARGETS = ("node", "ctl", "frame")


def ensure_driver(build=True):
    """Build the driver when it is missing (the tree is expected to be built)."""
    if os.path.exists(DRIVER) or not build:
        return
    print("building the campaign driver...", file=sys.stderr)
    subprocess.run(["make", "build/relais-faultinject"], cwd=ROOT, check=True)


def run_campaign(runs, seed, ticks, warmup, level):
    """Run the driver and return its JSON lines as dictionaries."""
    cmd = [DRIVER, "--runs", str(runs), "--seed", str(seed),
           "--ticks", str(ticks), "--warmup", str(warmup), "--level", level]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    rows = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line:
            rows.append(json.loads(line))
    return rows


def _blank():
    return {"runs": 0, "injected": 0, "detected": 0, "masked": 0, "violations": 0,
            "deviated": 0, "rejects": 0, "handovers": 0}


def aggregate(rows, delta=0.02):
    """Aggregate the per-run lines into the published balance."""
    total = _blank()
    by_target = defaultdict(_blank)
    by_zone = defaultdict(_blank)
    by_mode = defaultdict(_blank)
    reasons = Counter()
    invariants = Counter()
    max_dev = 0.0
    for row in rows:
        buckets = (total, by_target[row["target"]], by_zone[row["zone"]],
                   by_mode[row["mode"]])
        for bucket in buckets:
            bucket["runs"] += 1
            bucket["violations"] += row.get("violations", 0)
            if row.get("injected"):
                bucket["injected"] += 1
                if row.get("detected"):
                    bucket["detected"] += 1
                else:
                    bucket["masked"] += 1
                if row.get("max_deviation", 0.0) > delta:
                    bucket["deviated"] += 1
        total["rejects"] += row.get("rejected_delta", 0)
        total["handovers"] += row.get("handover_delta", 0)
        if row.get("injected"):
            reasons[row.get("reason", "masked")] += 1
        if row.get("violations"):
            invariants[row.get("inv", "?")] += 1
        max_dev = max(max_dev, row.get("max_deviation", 0.0))
    total["max_deviation"] = max_dev
    total["no_target"] = total["runs"] - total["injected"]
    return {"total": total, "target": by_target, "zone": by_zone, "mode": by_mode,
            "reasons": reasons, "invariants": invariants}


def _table(title, mapping, keys):
    """One markdown table: axis, value, injections, detected, masked, violations."""
    lines = ["| %s | injections | detectes | masques | violations |" % title,
             "|---|---|---|---|---|"]
    for key in keys:
        b = mapping.get(key)
        if b is None:
            continue
        lines.append("| %s | %d | %d | %d | %d |" %
                     (key, b["injected"], b["detected"], b["masked"], b["violations"]))
    return lines


def markdown(agg, meta):
    """The table published in docs/RESULTS.md."""
    total = agg["total"]
    out = []
    out.append("| axis | value | injections | detected | masked | violations |")
    out.append("|---|---|---|---|---|---|")
    for label, mapping, keys in (("target", agg["target"], TARGETS),
                                 ("zone", agg["zone"], ZONES),
                                 ("grain", agg["mode"], MODES)):
        for key in keys:
            b = mapping.get(key)
            if b is None:
                continue
            out.append("| %s | %s | %d | %d | %d | %d |" %
                       (label, key, b["injected"], b["detected"], b["masked"],
                        b["violations"]))
    out.append("| **total** | **%d runs** | **%d** | **%d** | **%d** | **%d** |" %
               (total["runs"], total["injected"], total["detected"], total["masked"],
                total["violations"]))
    out.append("")
    out.append("| reaction of the system | runs |")
    out.append("|---|---|")
    for reason, count in sorted(agg["reasons"].items(), key=lambda kv: -kv[1]):
        out.append("| %s | %d |" % (reason, count))
    out.append("")
    out.append("Seed %d, %d runs, level %s, %d ticks/run, one injection per run; "
               "safety invariants enforced: %s; largest deviation of the applied value "
               "against the twin: %.6g; runs above delta (0.02): %d." %
               (meta["seed"], total["runs"], meta["level"], meta["ticks"],
                SAFETY_INVARIANTS, total["max_deviation"], total["deviated"]))
    return "\n".join(out)


def report(agg, meta):
    total = agg["total"]
    print("=== Fine hardware fault campaign (M18) ===")
    print("runs=%d seed=%d level=%s ticks=%d" %
          (total["runs"], meta["seed"], meta["level"], meta["ticks"]))
    print("injections=%d  (no target available: %d)" %
          (total["injected"], total["no_target"]))
    print("detected=%d  masked=%d" % (total["detected"], total["masked"]))
    print("reactions: %s" %
          ", ".join("%s=%d" % (k, v) for k, v in sorted(agg["reasons"].items(),
                                                        key=lambda kv: -kv[1])))
    print("safety invariants (%s): %d violation(s)" %
          (SAFETY_INVARIANTS, total["violations"]))
    print("value deviation against the twin: max=%.6g, runs above delta: %d" %
          (total["max_deviation"], total["deviated"]))
    print("counters that moved: rejects=%d, holder changes=%d" %
          (total["rejects"], total["handovers"]))
    for title, mapping, keys in (("target", agg["target"], TARGETS),
                                  ("zone", agg["zone"], ZONES),
                                  ("grain", agg["mode"], MODES)):
        for key in keys:
            b = mapping.get(key)
            if b is None:
                continue
            print("  %-6s %-9s injections=%-5d detected=%-5d masked=%-5d violations=%d" %
                  (title, key, b["injected"], b["detected"], b["masked"], b["violations"]))


def main(argv=None):
    ap = argparse.ArgumentParser(description="campagne de fautes materielles fines")
    ap.add_argument("--runs", type=int, default=1000, help="number of simulated runs")
    ap.add_argument("--seed", type=int, default=1, help="master seed")
    ap.add_argument("--ticks", type=int, default=40, help="ticks per run")
    ap.add_argument("--warmup", type=int, default=12, help="ticks before the injection")
    ap.add_argument("--level", choices=("socle", "complet"), default="complet")
    ap.add_argument("--markdown", action="store_true", help="print the RESULTS.md table")
    ap.add_argument("--json", action="store_true", help="print the aggregate as JSON")
    ap.add_argument("--no-build", action="store_true", help="do not build the driver")
    args = ap.parse_args(argv)

    ensure_driver(build=not args.no_build)
    rows = run_campaign(args.runs, args.seed, args.ticks, args.warmup, args.level)
    agg = aggregate(rows)
    meta = {"seed": args.seed, "ticks": args.ticks, "level": args.level}
    if args.json:
        print(json.dumps({
            "meta": meta,
            "total": {k: v for k, v in agg["total"].items()},
            "target": {k: v for k, v in agg["target"].items()},
            "zone": {k: v for k, v in agg["zone"].items()},
            "mode": {k: v for k, v in agg["mode"].items()},
            "reasons": dict(agg["reasons"]),
        }, indent=2))
    elif args.markdown:
        print(markdown(agg, meta))
    else:
        report(agg, meta)
    return 0


if __name__ == "__main__":
    sys.exit(main())
