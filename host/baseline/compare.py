#!/usr/bin/env python3
"""Relais - the measured comparison with the state of the art (mission M43).

The same scenarios, the same faults and the same seeds are played on the
Relais protocol and on two baselines implemented for real:

  * heartbeat : a static primary, a heartbeat every tick, a silence timeout;
  * lease     : a leader holding a lease, renewed with a heartbeat, with a
                term that only grows.

The tables this script publishes land in docs/COMPARISON-MEASURED.md, between
the GENERATED markers. Everything else (the method, the threats on the
validity) is written by hand in that document.

Python standard library only (rule 6 of the project).
"""
import json
import math
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build" / "relais-baseline"
DOC = ROOT / "docs" / "COMPARISON-MEASURED.md"
OUT = ROOT / "build" / "compare"

IMPLS = ["relais", "heartbeat", "lease"]
TICK_US = 5000          # T = 5 ms, the default period of the project
REF_TIMEOUT = 20        # 100 ms: the silence timeout quoted for the field
SWEEP = [2, 4, 10, 20, 40, 400]
TUNING = ["nominal", "loss-5pc", "slow-machine", "one-tick-gap"]

SCENARIOS = [
    {"key": "nominal", "title": "no fault at all",
     "origin": "scenarios/slalom-nominal.json",
     "faults": [], "at": 400, "window": 1000},
    {"key": "death-A", "title": "the holder dies",
     "origin": "scenarios/slalom-death-A.json",
     "faults": ["800:death:0"], "at": 800, "window": 200},
    {"key": "death-B", "title": "the second dies",
     "origin": "scenarios/slalom-death-B.json",
     "faults": ["800:death:1"], "at": 800, "window": 200},
    {"key": "freeze-A-200ms", "title": "the holder freezes 200 ms and comes back",
     "origin": "scenarios/slalom-freeze-A-200ms.json",
     "faults": ["800:freeze:0:40"], "at": 800, "window": 200},
    {"key": "freeze-B-1s", "title": "the second freezes one second",
     "origin": "scenarios/slalom-freeze-B-1s.json",
     "faults": ["800:freeze:1:200"], "at": 800, "window": 200},
    {"key": "cut-A-to-actuator", "title": "the holder no longer reaches the actuator",
     "origin": "scenarios/slalom-cut-A-to-actuator.json",
     "faults": ["700:cut-actuator:0", "1300:restore:0"], "at": 700, "window": 600},
    {"key": "cut-between-computers", "title": "the link between the computers is cut",
     "origin": "scenarios/slalom-cut-between-computers.json",
     "faults": ["700:cut-peer:0", "1100:restore:0"], "at": 700, "window": 400},
    {"key": "cut-peer-then-death-A", "title": "the link is cut, then the holder dies",
     "origin": "scenarios/slalom-cut-between-computers.json + a death",
     "faults": ["700:cut-peer:0", "900:death:0", "1100:restore:0"], "at": 900, "window": 200},
    {"key": "double-death", "title": "both machines die",
     "origin": "scenarios/slalom-double-death.json",
     "faults": ["700:death:0", "760:death:1"], "at": 700, "window": 200},
    {"key": "slow-machine", "title": "the holder loses one tick out of three",
     "origin": "scenarios/slalom-slow-machine.json",
     "faults": ["600:slow:0:3"], "at": 600, "window": 1000},
    {"key": "handover-A", "title": "the holder steps down for maintenance",
     "origin": "scenarios/rolling-update.json (the cession of REL-09)",
     "faults": ["800:handover:0:40"], "at": 800, "window": 200},
    {"key": "loss-5pc", "title": "5 % of the frames are lost",
     "origin": "derived: the same channel, with losses",
     "faults": [], "loss": 0.05, "at": 400, "window": 1000},
    {"key": "one-tick-gap", "title": "the holder misses one single tick",
     "origin": "derived: the shortest fault a timeout can mistake for a death",
     "faults": ["800:freeze:0:0"], "at": 800, "window": 1000},
    {"key": "death-A-phase", "title": "the holder dies, at a swept instant of the cycle",
     "origin": "scenarios/sweep-death-all-phases.json (the instant is swept)",
     "faults": ["800:death:0"], "at": 0, "window": 200, "jitter": 200},
    {"key": "blackout-20ticks", "title": "the actuator link of both machines is cut 100 ms",
     "origin": "derived: a correlated outage, the two computers lose at the same time",
     "faults": ["900:cut-actuator:0", "900:cut-actuator:1", "920:restore:0",
                "920:restore:1"], "at": 900, "window": 200},
]

BY_KEY = {s["key"]: s for s in SCENARIOS}


def run_case(impl, sc, timeout, seeds, burst=True, binary=None):
    """One process plays the scenario for every seed."""
    cmd = [str(binary or BIN), "--impl", impl, "--ticks", str(sc.get("ticks", 2500)),
           "--seed", str(seeds[0]), "--reps", str(len(seeds)),
           "--timeout", str(timeout), "--window", str(sc.get("window", 200)),
           "--at", str(sc["at"]), "--burst", "1" if burst else "0"]
    if sc.get("loss"):
        cmd += ["--loss", str(sc["loss"])]
    if sc.get("jitter"):
        cmd += ["--jitter", str(sc["jitter"])]
    for fault in sc["faults"]:
        cmd += ["--fault", fault]
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if done.returncode != 0:
        raise RuntimeError("runner failed: %s\n%s" % (" ".join(cmd), done.stderr))
    return [json.loads(line) for line in done.stdout.splitlines() if line.strip()]


def pct(values, p):
    """The nearest rank percentile: no interpolation, no surprise."""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, int(math.ceil(p / 100.0 * len(ordered))))
    return ordered[rank - 1]


def mean(values):
    if not values:
        return 0.0
    return sum(values) / float(len(values))


def fmt(value, digits=3):
    if value is None:
        return "-"
    return ("%." + str(digits) + "f") % value


def summarise(runs):
    handovers = [r["handover"] for r in runs if r["handover"] >= 0]
    fresh = [r["fresh"] for r in runs if r["fresh"] >= 0]
    gaps = [r["gap"] for r in runs]
    jumps = [r["hjump"] for r in runs]
    stale = [r["stale"] for r in runs]
    doubles = [r["double"] for r in runs]
    return {
        "n": len(runs),
        "no_handover": len(runs) - len(handovers),
        "ho_p50": pct(handovers, 50),
        "ho_p99": pct(handovers, 99),
        "ho_max": max(handovers) if handovers else None,
        "fr_p50": pct(fresh, 50),
        "fr_max": max(fresh) if fresh else None,
        "stale_mean": mean(stale),
        "stale_max": max(stale) if stale else 0,
        "double_mean": mean(doubles),
        "double_max": max(doubles) if doubles else 0,
        "split_max": max([r["split"] for r in runs]) if runs else 0,
        "gap_p50": pct(gaps, 50),
        "gap_max": max(gaps) if gaps else 0.0,
        "hjump_p50": pct(jumps, 50),
        "hjump_max": max(jumps) if jumps else 0.0,
    }


def ticks_to_us(ticks):
    if ticks is None:
        return "-"
    return "%d" % (ticks * TICK_US)


def table_handover(results, timeout):
    """The delay of the handover: the headline table."""
    lines = ["| scenario | implementation | N | p50 ticks | p99 ticks | max ticks |"
             " p50 us | max us | no takeover |",
             "|---|---|---|---|---|---|---|---|---|"]
    for sc in SCENARIOS:
        for impl in IMPLS:
            s = results[(sc["key"], impl)]["ref"]
            lines.append("| %s | %s | %d | %s | %s | %s | %s | %s | %d |" % (
                sc["key"], impl, s["n"],
                fmt(s["ho_p50"], 1), fmt(s["ho_p99"], 1), fmt(s["ho_max"], 1),
                ticks_to_us(s["ho_p50"]), ticks_to_us(s["ho_max"]), s["no_handover"]))
    return lines


def table_consequences(results):
    """What the delay costs: stale ticks, double commands, loss of setpoint."""
    lines = ["| scenario | implementation | first fresh value from another computer (p50 ticks) | ticks without a fresh"
             " order (mean) | worst (max) | double commands (mean) | worst (max) |"
             " loss of setpoint, max | discontinuity at the takeover, max |",
             "|---|---|---|---|---|---|---|---|---|"]
    for sc in SCENARIOS:
        for impl in IMPLS:
            s = results[(sc["key"], impl)]["ref"]
            lines.append("| %s | %s | %s | %s | %d | %s | %d | %s | %s |" % (
                sc["key"], impl, fmt(s["fr_p50"], 1), fmt(s["stale_mean"], 1),
                s["stale_max"], fmt(s["double_mean"], 2), s["double_max"],
                fmt(s["gap_max"], 4), fmt(s["hjump_max"], 4)))
    return lines


def table_sweep(sweep):
    """The tuning of the baselines: the price of a shorter timeout."""
    lines = ["| timeout | heartbeat: handover on death-A (p50 ticks) |"
             " heartbeat: ticks without a fresh order | heartbeat: double commands"
             " when the dead machine comes back | heartbeat: false takeovers |"
             " lease: handover on death-A (p50 ticks) |"
             " lease: ticks without a fresh order |"
             " lease: double commands | lease: false takeovers |",
             "|---|---|---|---|---|---|---|---|---|"]
    for timeout in SWEEP:
        hb = sweep[("heartbeat", timeout)]
        ls = sweep[("lease", timeout)]
        lines.append("| %d ticks (%.0f ms) | %s | %s | %s | %d / %d | %s | %s | %s |"
                     " %d / %d |" % (
                         timeout, timeout * TICK_US / 1000.0, fmt(hb["ho_p50"], 1),
                         fmt(hb["stale_mean"], 1), fmt(hb["double_mean"], 2),
                         hb["false_takeovers"], hb["tuning_runs"], fmt(ls["ho_p50"], 1),
                         fmt(ls["stale_mean"], 1), fmt(ls["double_mean"], 2),
                         ls["false_takeovers"], ls["tuning_runs"]))
    return lines


def table_burst(guard_on, guard_off):
    """The sensitivity to the channel hypothesis of the project (ROB-03)."""
    lines = ["| implementation | timeout | false takeovers, burst guard ON |"
             " false takeovers, burst guard OFF | takeover on death-A (p50 ticks) |",
             "|---|---|---|---|---|"]
    for impl in ["heartbeat", "lease"]:
        for timeout in SWEEP:
            on = guard_on[(impl, timeout)]
            off = guard_off[(impl, timeout)]
            lines.append("| %s | %d ticks (%.0f ms) | %d / %d | %d / %d | %s |" % (
                impl, timeout, timeout * TICK_US / 1000.0, on["false_takeovers"],
                on["tuning_runs"], off["false_takeovers"], off["tuning_runs"],
                fmt(on["ho_p50"], 1)))
    return lines


def run_jobs(jobs, workers=8):
    """The jobs are independent processes: they run side by side."""
    with ThreadPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(lambda job: job(), jobs))


def collect(binary, reps, seed0):
    """The whole campaign: every scenario, every implementation, every seed."""
    seeds = [seed0 + i for i in range(reps)]
    keys = []
    jobs = []
    for sc in SCENARIOS:
        for impl in IMPLS:
            keys.append((sc["key"], impl))
            jobs.append(lambda sc=sc, impl=impl: run_case(impl, sc, REF_TIMEOUT, seeds,
                                                          True, binary))
    results = {}
    for key, runs in zip(keys, run_jobs(jobs)):
        results[key] = {"ref": summarise(runs), "runs": runs}
    return results, seeds


def sweep_one(impl, timeout, seeds, binary, burst):
    """One point of the tuning sweep: the delay, the false takeovers, the
    double commands when the dead machine comes back."""
    false_takeovers = 0
    tuning_runs = 0
    for key in TUNING:
        sc = BY_KEY[key]
        runs = run_case(impl, sc, timeout, seeds, burst, binary)
        for r in runs:
            tuning_runs += 1
            if r["handover"] >= 0:
                false_takeovers += 1
    death = run_case(impl, BY_KEY["death-A"], timeout, seeds, burst, binary)
    ho = [r["handover"] for r in death if r["handover"] >= 0]
    freeze = run_case(impl, BY_KEY["freeze-A-200ms"], timeout, seeds, burst, binary)
    return {
        "ho_p50": pct(ho, 50),
        "ho_max": max(ho) if ho else None,
        "false_takeovers": false_takeovers,
        "tuning_runs": tuning_runs,
        "double_mean": mean([r["double"] for r in freeze]),
        "double_max": max([r["double"] for r in freeze]) if freeze else 0,
        "stale_mean": mean([r["stale"] for r in death]),
    }


def collect_sweep(binary, reps, seed0):
    """The tuning sweep of the baselines: the timeout, and its price."""
    return collect_guard(binary, reps, seed0, True)


def collect_guard(binary, reps, seed0, burst):
    """The tuning sweep with or without the burst guard of ROB-03."""
    seeds = [seed0 + i for i in range(reps)]
    keys = []
    jobs = []
    for impl in ["heartbeat", "lease"]:
        for timeout in SWEEP:
            keys.append((impl, timeout))
            jobs.append(lambda impl=impl, timeout=timeout: sweep_one(impl, timeout, seeds,
                                                                     binary, burst))
    out = {}
    for key, value in zip(keys, run_jobs(jobs)):
        out[key] = value
    return out


def build_markdown(results, sweep, guard_on, guard_off, reps, seed0):
    lines = ["", "<!-- BEGIN GENERATED TABLES -->", "",
             "Generated by `make compare` (host/baseline/compare.py).",
             "N = %d repetitions per cell, seeds %d..%d, T = 5 ms (5000 us per tick)."
             % (reps, seed0, seed0 + reps - 1),
             "The reference timeout of the two baselines is %d ticks (%.0f ms), the"
             " silence timeout quoted for the field in docs/SPEC.md section 1.4."
             % (REF_TIMEOUT, REF_TIMEOUT * TICK_US / 1000.0), "",
             "### Table 1 - delay of the handover", "",
             "The delay between the fault and the change of commander at the actuator.",
             "A dash means the change never happened in that run: the count is in the"
             " last column.", ""]
    lines += table_handover(results, REF_TIMEOUT)
    lines += ["", "### Table 2 - what the delay costs", "",
              "A tick without a fresh order is a tick served by the fallback law instead"
              " of a value computed by a living computer. The loss of setpoint is the"
              " largest distance between the rack of the faulty run and the rack of the"
              " fault free twin.", ""]
    lines += table_consequences(results)
    lines += ["", "### Table 3 - tuning of the baselines", "",
              "The timeout of the two baselines, swept. A false takeover is a change of"
              " commander in a scenario where no machine died (nominal, 5 % of loss,"
              " slow machine, one missed tick): the timeout was too short.", ""]
    lines += table_sweep(sweep)
    lines += ["", "### Table 4 - the channel hypothesis", "",
              "The same campaign with the burst guard of ROB-03 switched off: the"
              " channel may then lose two frames in a row, which a timeout based"
              " baseline has to survive.", ""]
    lines += table_burst(guard_on, guard_off)
    lines += ["", "<!-- END GENERATED TABLES -->", ""]
    return "\n".join(lines)


def update_doc(markdown, path):
    if not path.exists():
        print("no document at %s: the tables are not published" % path)
        return False
    text = path.read_text(encoding="utf-8")
    start = "<!-- BEGIN GENERATED TABLES -->"
    end = "<!-- END GENERATED TABLES -->"
    if start not in text or end not in text:
        print("the markers are missing from %s" % path)
        return False
    head = text.split(start)[0]
    tail = text.split(end)[1]
    path.write_text(head + markdown.lstrip("\n") + tail, encoding="utf-8")
    return True


def main(argv):
    binary = BIN
    reps = 30
    seed0 = 1000
    doc = DOC
    for arg in argv[1:]:
        if arg.startswith("--bin="):
            binary = Path(arg.split("=", 1)[1])
        elif arg.startswith("--reps="):
            reps = int(arg.split("=", 1)[1])
        elif arg.startswith("--seed="):
            seed0 = int(arg.split("=", 1)[1])
        elif arg.startswith("--doc="):
            doc = Path(arg.split("=", 1)[1])
    if not binary.exists():
        print("the runner is missing: %s (run make baseline)" % binary)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    results, seeds = collect(binary, reps, seed0)
    sweep = collect_sweep(binary, reps, seed0)
    guard_on = collect_guard(binary, reps, seed0, True)
    guard_off = collect_guard(binary, reps, seed0, False)
    payload = {
        "reps": reps,
        "seed0": seed0,
        "tick_us": TICK_US,
        "reference_timeout": REF_TIMEOUT,
        "results": {"%s|%s" % k: v["ref"] for k, v in results.items()},
        "sweep": {"%s|%d" % k: v for k, v in sweep.items()},
        "burst_guard_off": {"%s|%d" % k: v for k, v in guard_off.items()},
    }
    (OUT / "results.json").write_text(json.dumps(payload, indent=1, sort_keys=True),
                                       encoding="utf-8")
    markdown = build_markdown(results, sweep, guard_on, guard_off, reps, seed0)
    (OUT / "tables.md").write_text(markdown, encoding="utf-8")
    update_doc(markdown, doc)
    print(markdown)
    print("results: %s" % (OUT / "results.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
