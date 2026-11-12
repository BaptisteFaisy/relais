#!/usr/bin/env python3
"""Relais — one nightly probe, one JSON artifact (idea 10, nightly.yml).

Each CI job calls this once with a `kind`; the process runs its share of the
work, measures the wall-clock duration, and writes a dated JSON artifact the
final `report` job assembles. A probe never ties itself to a runner: the work is
sliced deterministically (`--shard i --shards k`) so the full scale is replayed
across several jobs, each under the five-minute CI budget (user rule).

Requirement IDs: INJ-08 (sweep), INJ-11 (mutants), OBS-05 (report cites the run),
ROB-06 (replayable campaign), PERF-05 / REP-06 (the long runs).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

SIM = "./build/relais-simulate"
EXPLORE = "./build/relais-explore"
OUT = "results"


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def run(argv, timeout=300):
    start = time.monotonic()
    proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    return proc.returncode, time.monotonic() - start, (proc.stdout + proc.stderr)


def write(artifact, payload):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, artifact + ".json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, sort_keys=True)
    print("ARTIFACT %s" % path)
    return path


def slice_runs(runs, shard, shards):
    n = len(runs)
    start = shard * n // shards
    end = (shard + 1) * n // shards
    return runs[start:end]


def death_runs(args):
    out = []
    for role in (0, 1):
        for i in range(args.sweep_count):
            at = args.sweep_from + i * args.sweep_step
            out.append([SIM, "--ticks", "2500", "--seed", str(at),
                        "--level", "complet", "--fault", "%d:death:%d" % (at, role),
                        "--quiet"])
    return out


def freeze_runs(args):
    out = []
    n = args.durations
    for role in (0, 1):
        for i in range(n):
            dur = 1 + (i * 1596) // (n - 1) if n > 1 else 1
            for level in ("socle", "complet"):
                argv = [SIM, "--ticks", str(600 + dur), "--seed", str(1000 + i),
                        "--fault", "600:freeze:%d:%d" % (role, dur), "--quiet"]
                if level == "complet":
                    argv += ["--level", "complet"]
                out.append(argv)
    return out


_CUT_LINKS = [
    ("cut-actuator", 0, 1), ("cut-actuator", 0, 2),
    ("cut-actuator", 1, 1), ("cut-actuator", 1, 2),
    ("cut-peer", 0, 0), ("cut-peer", 1, 0),
]


def cut_runs(args):
    out = []
    for kind, node, arg in _CUT_LINKS:
        for level in ("socle", "complet"):
            for i in range(args.sweep_count):
                at = 100 + i * 33
                argv = [SIM, "--ticks", "2500", "--seed", str(at),
                        "--fault", "%d:%s:%d:%d" % (at, kind, node, arg), "--quiet"]
                if level == "complet":
                    argv += ["--level", "complet"]
                out.append(argv)
    return out


def probe_campaign(args):
    builders = {"death": death_runs, "freeze": freeze_runs, "cut": cut_runs}
    kind = args.campaign_kind
    runs = builders[kind](args)
    todo = slice_runs(runs, args.shard, args.shards)
    failures = []
    elapsed = 0.0
    for argv in todo:
        rc, dt, out = run(argv)
        elapsed += dt
        if rc != 0:
            failures.append({"argv": argv, "tail": out[-200:]})
    print("%s: %d/%d runs, %d failures, %.1f s" % (
        kind, len(todo), len(runs), len(failures), elapsed))
    write("campaign-%s-%d" % (kind, args.shard), {
        "date": now(), "scale": "campaign-%s" % kind,
        "kind": kind, "shard": args.shard, "shards": args.shards,
        "runs": len(todo), "total_runs": len(runs),
        "failures": len(failures), "violations": len(failures),
        "duration": round(elapsed, 2), "samples": failures[:5],
    })
    return 1 if failures else 0


def probe_sim(args):
    rc, dt, out = run([SIM, "--ticks", str(args.ticks), "--seed", str(args.seed),
                       *((["--level", "complet"]) if args.level else []), "--quiet"],
                      timeout=280)
    write("sim-%d" % args.seed, {
        "date": now(), "scale": "simulate", "seed": args.seed,
        "ticks": args.ticks, "level": "complet" if args.level else "socle",
        "violations": 0 if rc == 0 else 1, "duration": round(dt, 2),
        "tail": out[-200:] if rc != 0 else "",
    })
    return rc


def probe_soak(args):
    scores = []
    for level in (0, 1):
        argv = [SIM, "--ticks", "8640000", "--seed", "99", "--loss", "0.01"]
        if level:
            argv += ["--level", "complet"]
        argv += ["--quiet"]
        rc, dt, out = run(argv, timeout=280)
        scores.append({"level": "complet" if level else "socle",
                       "violations": 0 if rc == 0 else 1, "duration": round(dt, 2),
                       "tail": out[-200:] if rc != 0 else ""})
    write("soak", {"date": now(), "scale": "soak-12h",
                   "runs": len(scores), "violations": sum(s["violations"] for s in scores),
                   "duration": round(sum(s["duration"] for s in scores), 2),
                   "samples": scores})
    return 1 if any(s["violations"] for s in scores) else 0


def probe_explore(args):
    total = {"states": 0, "violations": 0, "duration": 0.0, "runs": []}
    for budget, depth, faults in [(100000, 12, 1), (9999999, 16, 2)]:
        rc, dt, out = run([EXPLORE, "--budget", str(budget), "--depth", str(depth),
                           "--faults", str(faults)], timeout=280)
        states = 0
        for tok in out.split():
            if tok.isdigit() and states == 0 and "EXPLORE-OK" in out:
                pass
        # the last EXPLORE-OK line carries "states, deepest tick u, 0 violation"
        states = int(out.split("EXPLORE-OK")[-1].split()[0]) if "EXPLORE-OK" in out else 0
        bad = 1 if rc != 0 else 0
        total["states"] += states
        total["violations"] += bad
        total["duration"] = round(total["duration"] + dt, 2)
        total["runs"].append({"budget": budget, "depth": depth, "faults": faults,
                              "states": states, "violations": bad,
                              "duration": round(dt, 2)})
    write("explore", {"date": now(), "scale": "explore",
                      "states": total["states"], "violations": total["violations"],
                      "duration": total["duration"], "runs": total["runs"]})
    return 1 if total["violations"] else 0


def probe_unit(args):
    rc, dt, out = run(["make", "-j8", "all"], timeout=280)
    if rc != 0:
        write("unit", {"date": now(), "scale": "unit", "checks": 0,
                       "violations": 1, "duration": round(dt, 2), "tail": out[-300:]})
        return 1
    rc2, dt2, out2 = run(["make", "test"], timeout=280)
    checks = sum(int(n) for n in re.findall(r"OK (\d+) checks", out2))
    write("unit", {"date": now(), "scale": "unit", "checks": checks,
                   "violations": 0 if rc2 == 0 else 1,
                   "duration": round(dt + dt2, 2), "tail": out2[-300:] if rc2 else ""})
    return rc2 if rc2 != 0 else 0


def probe_mutants(args):
    rc, dt, out = run([sys.executable, "host/mutants/run_mutants.py"], timeout=280)
    flagged = 0
    for line in out.splitlines():
        if "/8 mutants flagged" in line:
            flagged = int(line.split("/")[0].strip())
    write("mutants", {"date": now(), "scale": "mutants-bench", "flagged": flagged,
                      "violations": 0 if flagged == 8 else 1,
                      "duration": round(dt, 2), "tail": out[-300:] if flagged != 8 else ""})
    return 0 if flagged == 8 else 1


def probe_score(args):
    argv = [sys.executable, "host/mutants/mutation_score.py", "--max-mutants",
            str(args.max_mutants), "--time-budget", str(args.time_budget),
            "--jobs", str(args.jobs), "--json", "results/mutation-score-raw.json"]
    rc, dt, out = run(argv, timeout=290)
    score = None
    if os.path.exists("results/mutation-score-raw.json"):
        with open("results/mutation-score-raw.json", encoding="utf-8") as fh:
            score = json.load(fh)
    write("mutation-score", {"date": now(), "scale": "mutation-score",
                             "violations": 0, "duration": round(dt, 2),
                             "score_percent": score.get("score_percent") if score else None,
                             "killed": (score.get("counts", {}).get("killed") if score else None),
                             "survived": (score.get("counts", {}).get("survived") if score else None),
                             "equivalent": (score.get("counts", {}).get("equivalent") if score else None),
                             "invalid": (score.get("counts", {}).get("invalid") if score else None),
                             "tail": out[-300:]})
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description="One nightly probe, one JSON artifact.")
    ap.add_argument("kind", choices=["unit", "explore", "sim", "soak", "campaign",
                                     "mutants", "mutation-score"])
    ap.add_argument("--kind", dest="campaign_kind", default="death",
                    choices=["death", "freeze", "cut"],
                    help="campaign family (for kind=campaign)")
    ap.add_argument("--shard", type=int, default=0)
    ap.add_argument("--shards", type=int, default=1)
    ap.add_argument("--sweep-from", type=int, default=100)
    ap.add_argument("--sweep-step", type=int, default=7)
    ap.add_argument("--sweep-count", type=int, default=1986)
    ap.add_argument("--durations", type=int, default=2500)
    ap.add_argument("--ticks", type=int, default=40000000)
    ap.add_argument("--seed", type=int, default=1000)
    ap.add_argument("--level", action="store_true")
    ap.add_argument("--max-mutants", type=int, default=60)
    ap.add_argument("--time-budget", type=int, default=200)
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args(argv)

    handlers = {
        "unit": probe_unit, "explore": probe_explore, "sim": probe_sim,
        "soak": probe_soak, "campaign": probe_campaign, "mutants": probe_mutants,
        "mutation-score": probe_score,
    }
    return handlers[args.kind](args)


if __name__ == "__main__":
    sys.exit(main())