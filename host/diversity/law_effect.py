#!/usr/bin/env python3
"""Relais — measured effect of a DESIGN-diverse control law on the handover.

Mission M37, deliverable 3. The compilation axis is measured by compare.py; here
the diversity is in the DESIGN of the control law, plugged through the existing
three-function interface of CMD-09 (`relais_node_set_law`) on one machine only.
core/ is untouched: the alternative laws live in host/diversity/law_alt.c.

Configurations, all on the same corpus of handover runs:
  identical          both machines run the built-in PID: what ships today
  alt-second-narrow  machine B runs the first order law, band NOT widened
  alt-second-wide    the same, with the DEG-08 band published (deps/ddelta)
  ramp-second-wide   machine B runs the ramp follower instead, band published
  alt-holder-wide    the HOLDER runs the alternative law: CMD-09 has no hook for
                     the will, so the testament stays the built-in law's; this
                     configuration measures what that gap costs at the handover
  alt-both-wide      both machines run the SAME alternative law: a control that
                     shows a different law is not, by itself, diversity

Measured per configuration: certification, disagreement, the order/shadow gap
against epsilon and the published band, and — from the core's own invariant
checker — the gap in ticks without a fresh order, the deviation from the twin at
the handover, the fallback run and the verdict.

Usage: python3 host/diversity/law_effect.py [--binary PATH] [--out-dir DIR]
"""
import argparse
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

BASE_PARAMS = "T=5,p=1,N=8"
# DEG-08: the widened band is a PUBLISHED number, not a tolerance invented after
# the measurement. 0.10 of full scale on the order/shadow comparison, 0.05 on the
# continuity bump at the handover (against 0.005 and 0.02 for an identical backup).
WIDE_PARAMS = "T=5,p=1,N=8,deps=0.10,ddelta=0.05"
# The ramp follower is further from the built-in PID than the first order
# compensator is (measured order/shadow gap: see gap_p99 below). A published
# band smaller than the real design difference leaves the second uncertified,
# so this second configuration sizes the band to what was measured.
WIDER_PARAMS = "T=5,p=1,N=8,deps=0.25,ddelta=0.10"
EPSILON = 0.005
TRAJ = "kind=slalom amp=0.6 freq=0.5"
TICKS = 1500
WINDOW = 60

CONFIGS = [
    ("identical", []),
    ("alt-second-narrow", ["--law-b", "first-order"]),
    ("alt-second-wide", ["--law-b", "first-order"]),
    ("ramp-second-wide", ["--law-b", "ramp"]),
    ("ramp-second-wider", ["--law-b", "ramp"]),
    ("alt-holder-wide", ["--law-a", "first-order"]),
    ("alt-both-first-order", ["--law-a", "first-order", "--law-b", "first-order"]),
    ("alt-both-ramp", ["--law-a", "ramp", "--law-b", "ramp"]),
    # Level S has no shadow at all: the handover is always served by the will.
    # CMD-09 has no hook for the will, so relais_testament_fill() keeps running
    # the BUILT-IN law on the built-in state, which an installed law never
    # updates. These two configurations measure what that gap costs.
    ("socle-builtin", ["--level", "socle"]),
    ("socle-alt-holder", ["--law-a", "first-order", "--level", "socle"]),
]
# configuration name -> parameter block published to the arbiter
PARAMS_OF = {
    "identical": BASE_PARAMS,
    "alt-second-narrow": BASE_PARAMS,
    "alt-second-wide": WIDE_PARAMS,
    "ramp-second-wide": WIDE_PARAMS,
    "ramp-second-wider": WIDER_PARAMS,
    "alt-holder-wide": WIDE_PARAMS,
    "alt-both-first-order": WIDE_PARAMS,
    "alt-both-ramp": WIDER_PARAMS,
    "socle-builtin": BASE_PARAMS,
    "socle-alt-holder": BASE_PARAMS,
}

# name, faults, tick of the handover (the window measured around it)
HANDOVER_RUNS = [
    ("death-A", ["800:death:0"], 800),
    ("death-B", ["800:death:1"], 800),
    ("cession-A", ["900:handover:0"], 900),
    ("freeze-A-20", ["700:freeze:0:20"], 700),
    ("crash-A", ["700:crash:0"], 700),
]


def bits(text):
    try:
        return struct.unpack(">f", bytes.fromhex(text))[0]
    except (ValueError, struct.error):
        return 0.0


def pick_binary(explicit, out_dir):
    if explicit:
        return explicit
    manifest = os.path.join(out_dir, "variants.tsv")
    if os.path.exists(manifest):
        with open(manifest, encoding="utf-8") as fh:
            for line in fh.read().splitlines()[1:]:
                fields = line.split("\t")
                if fields[1] == "BUILT" and fields[0] == "ref-O2":
                    return fields[6]
    fallback = os.path.join(out_dir, "ref-O2", "relais-trace")
    if os.path.exists(fallback):
        return fallback
    sys.exit("no relais-trace built: run host/diversity/build_variants.sh first")


def run_args(config_name, law_flags, faults):
    args = ["--ticks", str(TICKS), "--seed", "5", "--level", "complet",
            "--trajectory", TRAJ, "--params", PARAMS_OF[config_name]]
    args += list(law_flags)
    for fault in faults:
        args += ["--fault", fault]
    return args


def parse_verdict(stderr):
    """The core invariant checker's own numbers, parsed from its report line."""
    out = {"violations": None, "max_gap": None, "max_deviation": None,
           "fallback_run": None, "lamp_delay": None}
    keys = {"violations": int, "max_gap": int, "max_deviation": float,
            "fallback_run": int, "lamp_delay": int}
    for token in stderr.replace("(", " ").split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        if key in keys:
            try:
                out[key] = keys[key](value)
            except ValueError:
                pass
    return out


def run_one(binary, config_name, law_flags, faults, out_dir, tag):
    path = os.path.join(out_dir, "law-%s-%s.tsv" % (config_name, tag))
    args = run_args(config_name, law_flags, faults) + ["--out", path]
    proc = subprocess.run([binary] + args, cwd=ROOT, capture_output=True,
                          text=True, timeout=600)
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    columns = lines[0].split("\t")[1:] if lines else []
    rows = [ln.split("\t") for ln in lines[2:] if ln and not ln.startswith("#")]
    os.remove(path)
    if len(rows) != TICKS:
        # A truncated trace means the run did not write what it should have:
        # out of disk space is the usual cause here, and silently reporting
        # zeros would be worse than failing.
        sys.exit("trace %s holds %d rows instead of %d (disk full? df -h %s)"
                 % (path, len(rows), TICKS, out_dir))
    metrics = parse_verdict(proc.stderr)
    metrics.update(scan_trace(columns, rows))
    metrics["exit"] = proc.returncode
    return metrics


def percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(pct / 100.0 * (len(ordered) - 1))))
    return ordered[index]


def scan_trace(columns, rows):
    """Certification, order/shadow gap, source histogram, handover bump."""
    idx = {name: i for i, name in enumerate(columns)}
    gaps, sources, bump = [], {}, 0.0
    tracking = []
    certified = dissimilar = disagree = 0
    previous = None
    for row in rows:
        source = row[idx["source"]] if "source" in idx else "?"
        sources[source] = sources.get(source, 0) + 1
        certified += row[idx["cert"]] == "1"
        dissimilar += row[idx["dissim"]] == "1"
        if "8" in row[idx["events"]].split(","):
            disagree += 1
        order, shadow = bits(row[idx["order"]]), bits(row[idx["shadow"]])
        if order != 0.0 or shadow != 0.0:
            gaps.append(abs(order - shadow))
        value = bits(row[idx["value"]])
        if previous is not None:
            bump = max(bump, abs(value - previous))
        previous = value
        # Does the alternative law still steer? The tracking error is measured
        # against the request published by the actuator, not against the twin.
        tracking.append(abs(bits(row[idx["pos"]]) - bits(row[idx["setpoint"]])))
    return {"ticks": len(rows), "certified_ticks": certified,
            "dissimilar_ticks": dissimilar, "disagreement_events": disagree,
            "gap_max": max(gaps) if gaps else 0.0,
            "gap_p50": percentile(gaps, 50), "gap_p99": percentile(gaps, 99),
            "sources": sources, "bump_max": bump,
            "tracking_max": max(tracking) if tracking else 0.0,
            "tracking_mean": (sum(tracking) / len(tracking)) if tracking else 0.0}


def markdown_table(results):
    head = ("| configuration | run | violations | max_gap | max_deviation | "
            "certified ticks | dissimilar ticks | disagreements | gap p99 | "
            "gap max | tracking max | bump max | sources |\n"
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
    lines = [head]
    for entry in results:
        m = entry["metrics"]
        sources = ",".join("%s:%d" % (k, v) for k, v in sorted(m["sources"].items()))
        lines.append("| %s | %s | %s | %s | %s | %d/%d | %d | %d | %.4f | %.4f | "
                     "%.4f | %.4f | %s |\n"
                     % (entry["config"], entry["run"], m["violations"], m["max_gap"],
                        m["max_deviation"], m["certified_ticks"], m["ticks"],
                        m["dissimilar_ticks"], m["disagreement_events"],
                        m["gap_p99"], m["gap_max"], m["tracking_max"], m["bump_max"],
                        sources))
    return "".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", default="")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "build/diversity"))
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    binary = pick_binary(args.binary, args.out_dir)
    print("binary: %s" % binary)

    results = []
    for config_name, law_flags in CONFIGS:
        for run_name, faults, at_tick in HANDOVER_RUNS:
            entry = {"config": config_name, "run": run_name, "law_flags": law_flags,
                     "params": PARAMS_OF[config_name],
                     "epsilon": EPSILON}
            metrics = run_one(binary, config_name, law_flags, faults,
                              args.out_dir, run_name)
            metrics["handover_at"] = at_tick
            entry["metrics"] = metrics
            results.append(entry)
            m = entry["metrics"]
            print("%-20s %-12s viol=%s gap=%s dev=%.4f cert=%d/%d dissim=%d "
                  "disagree=%d gap_p99=%.4f track_max=%.4f will+fb=%d"
                  % (config_name, run_name, m["violations"], m["max_gap"],
                     m["max_deviation"], m["certified_ticks"], m["ticks"],
                     m["dissimilar_ticks"], m["disagreement_events"], m["gap_p99"],
                     m["tracking_max"],
                     m["sources"].get("testament", 0) + m["sources"].get("fallback", 0)))

    with open(os.path.join(args.out_dir, "law-effect.json"), "w",
              encoding="utf-8") as fh:
        json.dump({"binary": binary, "ticks": TICKS, "epsilon": EPSILON,
                   "base_params": BASE_PARAMS, "wide_params": WIDE_PARAMS,
                   "wider_params": WIDER_PARAMS,
                   "results": results}, fh, indent=1, sort_keys=True)
    table = markdown_table(results)
    with open(os.path.join(args.out_dir, "law-effect.md"), "w",
              encoding="utf-8") as fh:
        fh.write("# Effect of a design-diverse control law on the handover\n\n"
                 "Generated by host/diversity/law_effect.py; quoted by docs/CCF.md.\n\n"
                 + table)
    print("written: %s/law-effect.json and law-effect.md" % args.out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
