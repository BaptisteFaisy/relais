#!/usr/bin/env python3
"""rta.py — response-time analysis and MARGIN to the deadline, from measurements.

This is the M40 half of the temporal argument. It does NOT re-implement the
analysis: the fixed-point iteration, the stage model and the verdict come from
host/timing/rta.py, imported by path (see host/wcet/wcet_lib.py). What this file
adds is the substitution that docs/TIMING.md §6 asks for — the *example* stage
costs of 150/250/100 us are replaced by the costs MEASURED on a real machine by
host/wcet/measure.py, the inflation caused by the measured interferences
(journal, network, lower-priority CPU load) is folded in, and the MARGIN to each
published deadline is printed instead of left implicit.

Stage mapping (docs/TIMING.md §1), from the probe's segments:

    relais-net  (prio 62) = decode + encode      frame in / frame out, SEC-02
    relais-core (prio 61) = node - law           protocol state machine
    relais-ctl  (prio 60) = law                  relais_ctl_step, timed by the
                                                 CMD-09 law wrapper of the probe

so that C_net + C_core + C_ctl = the measured PATH, exactly.

Margins published (PERF-01, docs/SPEC-2.md):
    p99.9 of the path <= 1000 us, worst case <= 2000 us, and the cycle rule
    2*Delta + C + J <= T of spec 4.2, with C the RTA chain response.

Honesty: the verdict names the machine it was computed from. A PC number is a
PC number; the Pi and the QNX target are BLOCKED until measured there.

Usage:
  python3 host/wcet/rta.py --measure runs/wcet/linux-x86_64/measure.json
  python3 host/wcet/rta.py --stat max --period-us 5000 --sweep
"""
import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wcet_lib as W  # noqa: E402

STATS = ("p50", "p90", "p99", "p99_9", "max")
STAGES = ("relais-net", "relais-core", "relais-ctl")


def stage_costs(segments, stat):
    """Split the measured segments into the three QNX stages of docs/TIMING.md."""
    def pick(name):
        entry = segments.get(name)
        return float(entry[stat]) if entry else 0.0
    ctl = pick("law")
    core = max(0.0, pick("node") - ctl)
    net = pick("decode") + pick("encode")
    return {"relais-net": net, "relais-core": core, "relais-ctl": ctl}


def worst_stages(payload, stat):
    """Per stage, the worst cost over the measured interference modes."""
    worst = {name: 0.0 for name in STAGES}
    per_mode = {}
    for mode, block in sorted(payload.get("modes", {}).items()):
        segments = block.get("aggregate", {}).get("segments") or {}
        costs = stage_costs(segments, stat)
        per_mode[mode] = costs
        for name in STAGES:
            worst[name] = max(worst[name], costs[name])
    return worst, per_mode


def build_stages(costs):
    """host/timing/rta.py takes integer microseconds: every measured cost is
    rounded UP, so a sub-microsecond stage is never silently turned into 0."""
    return [W.TIMING_RTA.Stage(name, W.STAGE_PRIORITY[name],
                               int(math.ceil(costs[name])))
            for name in STAGES]


def inflation_table(per_mode):
    lines = ["stage costs per interference mode (us); the delta against the"
             " 'none' mode is the measured inflation:"]
    base = per_mode.get("none")
    for mode, costs in sorted(per_mode.items()):
        if base is None or mode == "none":
            extra = ""
        else:
            extra = "".join("  %s:%+.3f us" % (name.split("-")[1],
                                               costs[name] - base[name])
                            for name in STAGES)
        lines.append("  %-8s %s%s" % (mode, "  ".join(
            "%s=%.3f" % (name.split("-")[1], costs[name]) for name in STAGES),
            extra))
    return lines


def mode_paths(payload):
    """The published path descriptor of every interference mode."""
    out = {}
    for mode, block in sorted(payload.get("modes", {}).items()):
        segments = block.get("aggregate", {}).get("segments") or {}
        path = segments.get("path")
        if not path:
            continue
        bounds = [run["evt_bound_us"] for run in block.get("runs", [])
                  if run.get("evt_bound_us")]
        entry = dict(path)
        entry["evt_bound_us"] = max(bounds) if bounds else None
        out[mode] = entry
    return out


def worst_path(paths):
    """Worst tail over the modes: a margin may not average a worst case away."""
    if not paths:
        return None
    merged = W.merge_descriptors(list(paths.values()))
    bounds = [p["evt_bound_us"] for p in paths.values() if p.get("evt_bound_us")]
    merged["evt_bound_us"] = max(bounds) if bounds else None
    return merged


def margins(path, chain_us, period_us, delta_us, jitter_us):
    """Every margin the spec publishes, in microseconds (negative = exceeded)."""
    out = {}
    if path:
        out["perf01_p99_9"] = W.PERF01_P999_US - float(path["p99_9"])
        out["perf01_worst_max"] = W.PERF01_WORST_US - float(path["max"])
        if path.get("evt_bound_us"):
            out["perf01_worst_evt"] = W.PERF01_WORST_US - float(path["evt_bound_us"])
    if chain_us is not None:
        out["rta_chain_vs_period"] = float(period_us) - float(chain_us)
        out["cycle_rule_2d_c_j"] = float(period_us) - (
            2.0 * float(delta_us) + float(chain_us) + float(jitter_us))
    return out


def render_margins(found, period_us):
    lines = []
    labels = {"perf01_p99_9": "PERF-01 p99.9 <= %.0f us" % W.PERF01_P999_US,
              "perf01_worst_max": "PERF-01 worst (measured max) <= %.0f us"
                                  % W.PERF01_WORST_US,
              "perf01_worst_evt": "PERF-01 worst (EVT bound 1e-6) <= %.0f us"
                                  % W.PERF01_WORST_US,
              "rta_chain_vs_period": "RTA chain response <= T (%.0f us)" % period_us,
              "cycle_rule_2d_c_j": "cycle rule 2D+C+J <= T (%.0f us)" % period_us}
    for key, label in labels.items():
        if key not in found:
            continue
        value = found[key]
        verdict = "TENABLE" if value >= 0 else "EXCEEDED"
        lines.append("  %-42s margin = %10.3f us   %s" % (label, value, verdict))
    return lines


def smallest_period(costs, start=1.0, stop=100000.0):
    """PERF-08: the breaking point, the smallest T the chain still meets."""
    stages = build_stages(costs)
    period = start
    tenable = None
    while period <= stop:
        analysis = W.TIMING_RTA.analyze(stages, period)
        if analysis["schedulable"]:
            tenable = period
            break
        period *= 1.25
    return tenable


def load_measure(path):
    if path is None:
        path = os.path.join(W.ROOT, "runs", "wcet", W.host_label(), "measure.json")
    if not os.path.exists(path):
        print("BLOCKED(no measurement at %s)" % path)
        print("run first:  python3 host/wcet/measure.py --samples 20000 --runs 2")
        return None, None
    return W.read_json(path), path


def verdict_line(analysis, found):
    """One loud line: the harness publishes a violation, it never hides one."""
    exceeded = sorted(key for key, value in found.items() if value < 0)
    if not analysis["schedulable"]:
        return "VERDICT: NOT TENABLE - the RTA chain does not meet T (%s)" % "; ".join(
            analysis["reasons"])
    if exceeded:
        return ("VERDICT: measured, but %d published budget(s) EXCEEDED on this "
                "machine: %s" % (len(exceeded), ", ".join(exceeded)))
    return "VERDICT: every published budget is met by this measurement"


def render_paths(paths):
    lines = ["measured path per interference mode (us):",
             "  %-8s %8s %9s %9s %9s %10s %10s"
             % ("mode", "n", "p50", "p99", "p99.9", "max", "EVT 1e-6")]
    for mode, path in sorted(paths.items()):
        evt = path.get("evt_bound_us")
        lines.append("  %-8s %8d %9.3f %9.3f %9.3f %10.3f %10s"
                     % (mode, path["n"], path["p50"], path["p99"], path["p99_9"],
                        path["max"], "n/a" if evt is None else "%.3f" % evt))
    return lines


def render_stages(costs, stat):
    lines = ["stage costs from the measured segments (stat=%s, worst over modes):"
             % stat]
    detail = {"relais-net": "decode+encode", "relais-core": "node-law",
              "relais-ctl": "law"}
    for name in STAGES:
        lines.append("  %-12s prio %2d  C=%9.3f us   (%s)"
                     % (name, W.STAGE_PRIORITY[name], costs[name], detail[name]))
    return lines


def parse_args(argv):
    ap = argparse.ArgumentParser(
        description="RTA and margin to the deadline, from measured WCETs (M40)")
    ap.add_argument("--measure", default=None,
                    help="measure.json produced by host/wcet/measure.py")
    ap.add_argument("--stat", default="p99_9", choices=STATS,
                    help="which measured statistic becomes the stage WCET")
    ap.add_argument("--period-us", type=float, default=W.DEFAULT_PERIOD_US,
                    help="tick period T in microseconds (default 5000)")
    ap.add_argument("--delta-us", type=float, default=W.DEFAULT_DELTA_US,
                    help="one-way frame delay Delta of the cycle rule")
    ap.add_argument("--jitter-us", type=float, default=W.DEFAULT_JITTER_US,
                    help="release jitter J of the cycle rule")
    ap.add_argument("--sweep", action="store_true",
                    help="also publish the smallest tenable period (PERF-08)")
    ap.add_argument("--out", default=None, help="write the analysis as JSON here")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero when a published budget is exceeded; "
                         "without it the verdict is published and the exit code "
                         "only reports a harness failure")
    return ap.parse_args(argv)


def build_report(payload, source, args, worst, per_mode, analysis, paths, path,
                 found, breaking):
    lines = ["Relais M40 - response-time analysis from MEASURED costs",
             "measurement: %s" % source,
             "  host=%s  date=%s  samples/run=%d  runs/mode=%d  integrity=%s"
             % (payload.get("host"), payload.get("date"),
                payload.get("config", {}).get("samples", 0),
                payload.get("config", {}).get("runs", 0),
                "mac" if payload.get("config", {}).get("mac") else "crc"),
             "analysis: host/timing/rta.py (Audsley fixed point, no inheritance)"
             "  stat=%s  T=%.0f us" % (args.stat, args.period_us), ""]
    lines += render_paths(paths)
    lines.append("")
    lines += render_stages(worst, args.stat)
    lines += inflation_table(per_mode)
    lines.append("")
    lines.append(W.TIMING_RTA.render(analysis))
    lines.append("")
    lines.append("margins (negative = the deadline is exceeded):")
    lines += render_margins(found, args.period_us)
    if args.sweep:
        lines.append("PERF-08 breaking point: smallest tenable period = %s us"
                     % ("none found" if breaking is None else "%.0f" % breaking))
    lines.append("")
    lines.append(verdict_line(analysis, found))
    lines.append("verdict scope: computed from a measurement on %s. It is NOT a "
                 "target verdict: the Raspberry Pi and the QNX 8.0 target stay "
                 "BLOCKED until measured there (docs/WCET-TARGETS.md)."
                 % payload.get("host"))
    return "\n".join(lines)


def main(argv):
    args = parse_args(argv)
    payload, source = load_measure(args.measure)
    if payload is None:
        return 2
    worst, per_mode = worst_stages(payload, args.stat)
    stages = build_stages(worst)
    analysis = W.TIMING_RTA.analyze(stages, args.period_us)
    paths = mode_paths(payload)
    path = worst_path(paths)
    found = margins(path, analysis["chain_us"], args.period_us, args.delta_us,
                    args.jitter_us)
    breaking = smallest_period(worst) if args.sweep else None
    print(build_report(payload, source, args, worst, per_mode, analysis, paths,
                       path, found, breaking))

    out_path = args.out or os.path.join(os.path.dirname(source), "rta.json")
    W.write_json(out_path, {
        "tool": "host/wcet/rta.py", "source": source, "stat": args.stat,
        "period_us": args.period_us, "delta_us": args.delta_us,
        "jitter_us": args.jitter_us, "stage_costs_us": worst,
        "stages": [{"name": s.name, "priority": s.priority, "wcet_us": s.wcet_us}
                   for s in stages],
        "chain_us": analysis["chain_us"],
        "schedulable": analysis["schedulable"], "reasons": analysis["reasons"],
        "paths": paths, "path_worst": path, "margins_us": found,
        "smallest_tenable_period_us": breaking,
        "measured_on": payload.get("host")})
    if not args.strict:
        return 0
    ok = analysis["schedulable"] and all(v >= 0 for v in found.values())
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
