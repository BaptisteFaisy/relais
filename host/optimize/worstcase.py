#!/usr/bin/env python3
"""Relais — adversarial search for the worst fault scenario (INJ-12, spec 12 #1).

The campaign sweeps faults on a regular grid; this tool replaces the grid by an
adversary. An optimizer chooses the instant, the kind, the duration, the target
machine, the link direction and the nesting of up to two faults, to MAXIMISE the
deviation from the fault-free twin (primary) and the hole between two fresh
orders (secondary). An invariant violation is not a score: it is a counter-
example, ranked above every score and reported first.

Oracle: build/relais-simulate, run in a private folder holding actuator.jsonl,
twin.jsonl and scenario.json — exactly the layout of a campaign run, so the
winner stays replayable by the simulator, by the bench
(host/bench/relais_bench.py --scenario) and by the offline verifier
(build/relais-verify). The summary line of the C invariant checker is the
authority for the numbers; host/report is reused (never re-written) to
cross-check them from the journals.

Search space and encoding: host/optimize/wcspace.py.
Algorithms (evolutionary + annealing fallback): host/optimize/search.py.

Usage:
  python3 host/optimize/worstcase.py --budget 400 --seeds 2 --level both
  python3 host/optimize/worstcase.py --algo anneal --ticks 4000 --cross-check
Exit code: 0 when the search ran (even when it found a counter-example),
2 when the simulator itself cannot be run.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import report_worst        # noqa: E402  the markdown/text formatters
import search as sx        # noqa: E402  generic algorithms, same directory
import wcspace as space    # noqa: E402  the fault space, same directory

ROOT = os.path.dirname(os.path.dirname(HERE))
SIM = os.path.join("build", "relais-simulate")
LEVELS = ("socle", "complet")

# Scoring: deviation from the twin first (normalised by the full scale), hole
# between fresh orders second, an invariant breach above both (spec 12 #1).
FULL_SCALE = space.full_scale("")   # 2.0: RELAIS_FULL_SCALE = 1.0
GAP_WEIGHT = 0.2                    # the hole is the secondary term
VIOLATION_PRIORITY = 1000.0         # a counter-example outranks any deviation
ERROR_FITNESS = -1.0                # an unreadable run is the worst candidate
SUMMARY_RE = re.compile(
    r"ticks=(?P<ticks>\d+) violations=(?P<violations>\d+) "
    r"max_gap=(?P<max_gap>\d+) \(bound (?P<gap_bound>\d+)\) "
    r"max_deviation=(?P<max_deviation>[-+0-9.eE]+) "
    r"\(bound (?P<deviation_bound>[-+0-9.eE]+)\) "
    r"fallback_run=(?P<fallback_run>\d+) lamp_delay=(?P<max_lamp_delay>\d+) "
    r"level=(?P<level>\w+)")
FIRST_RE = re.compile(r"^first: (.*)$", re.MULTILINE)


@dataclass
class Measurement:
    """What one simulator run told us; the numbers come from the C checker."""

    rc: int = 2
    ticks: int = 0
    violations: int = 0
    max_gap: int = 0
    gap_bound: int = 0
    max_deviation: float = 0.0
    deviation_bound: float = 0.02
    fallback_run: int = 0
    lamp_delay: int = 0
    level: str = ""
    first: str = ""
    stderr: str = ""
    wall: float = 0.0
    cross: Dict[str, Any] = field(default_factory=dict)

    @property
    def broken(self) -> bool:
        """True when the run could not be read at all (crash, bad usage)."""
        return self.rc not in (0, 1) or self.ticks == 0

    def to_dict(self) -> Dict[str, Any]:
        """The measurement as JSON. The meaningful zeros (gap_bound = 0, an
        empty hole, a zero deviation) are kept: they say something the report
        must show, not something empty."""
        out = {"rc": self.rc, "ticks": self.ticks, "violations": self.violations,
               "max_gap": self.max_gap, "gap_bound": self.gap_bound,
               "max_deviation": self.max_deviation,
               "deviation_bound": self.deviation_bound,
               "fallback_run": self.fallback_run, "lamp_delay": self.lamp_delay,
               "level": self.level, "wall_ms": round(self.wall * 1000.0, 1)}
        if self.first:
            out["first"] = self.first
        if self.stderr:
            out["stderr"] = self.stderr
        if self.cross:
            out["cross"] = self.cross
        return out


@dataclass(frozen=True)
class Score:
    """The objective value of one run, and whether it is a counter-example."""

    dev_norm: float
    gap_norm: float
    fitness: float
    counter_example: bool
    violations: int

    def to_dict(self) -> Dict[str, Any]:
        return {"dev_norm": round(self.dev_norm, 6),
                "gap_norm": round(self.gap_norm, 6),
                "fitness": round(self.fitness, 6),
                "counter_example": self.counter_example,
                "violations": self.violations}


def objective(m: Measurement, ticks: int, scale: float) -> Score:
    """Deviation from the twin first, hole second, a violation above both."""
    if m.broken:
        return Score(0.0, 0.0, ERROR_FITNESS, False, 0)
    dev_norm = m.max_deviation / scale if scale > 0 else 0.0
    gap_norm = m.max_gap / float(max(1, ticks))
    fitness = (VIOLATION_PRIORITY * m.violations + dev_norm
               + GAP_WEIGHT * gap_norm)
    return Score(dev_norm, gap_norm, fitness, m.violations > 0, m.violations)


def parse_summary(text: str, rc: int, wall: float = 0.0) -> Measurement:
    """Read the invariant report line printed by relais-simulate."""
    found = SUMMARY_RE.search(text or "")
    if found is None:
        return Measurement(rc=rc, wall=wall, stderr=(text or "")[-400:])
    g = found.groupdict()
    first = FIRST_RE.search(text or "")
    return Measurement(rc=rc, ticks=int(g["ticks"]), violations=int(g["violations"]),
                       max_gap=int(g["max_gap"]), gap_bound=int(g["gap_bound"]),
                       max_deviation=float(g["max_deviation"]),
                       deviation_bound=float(g["deviation_bound"]),
                       fallback_run=int(g["fallback_run"]),
                       lamp_delay=int(g["max_lamp_delay"]), level=g["level"],
                       first=first.group(1) if first else "", wall=wall)


def cross_check(folder: str, level: str, m: Measurement) -> Dict[str, Any]:
    """Re-read the journals with host/report (reused, never re-written) and
    compare its own measurement of the gap and of the deviation."""
    report_dir = os.path.join(ROOT, "host", "report")
    if report_dir not in sys.path:
        sys.path.insert(0, report_dir)
    import analysis      # noqa: E402
    import journal_io    # noqa: E402
    run = journal_io.load_run(folder)
    if run is None:
        return {"error": "no journal to cross-check"}
    found = analysis.analyze_run(run, level)
    agrees = (found.max_gap == m.max_gap
              and abs(found.max_deviation - m.max_deviation) <= 1e-5)
    return {"max_gap": found.max_gap, "gap_tick": found.max_gap_tick,
            "max_deviation": round(found.max_deviation, 6),
            "sources": found.source_counts, "agrees": agrees}


def write_json(path: str, doc: Any) -> None:
    """One JSON document, pretty, with its folder created."""
    folder = os.path.dirname(path)
    if folder:
        os.makedirs(folder, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(doc, handle, indent=2)
        handle.write("\n")


class SimOracle:
    """Runs build/relais-simulate and turns its verdict into a fitness."""

    def __init__(self, ticks: int = 2000, seed: int = 7, level: str = "complet",
                 params: str = "", trajectory: str = "", root: str = ROOT,
                 sim: str = SIM, journals: bool = True, cross_check: bool = False,
                 keep_dir: Optional[str] = None, timeout: float = 120.0) -> None:
        self.ticks, self.seed, self.level = ticks, seed, level
        self.params, self.trajectory = params, trajectory
        self.root, self.sim = root, sim
        self.journals, self.cross_check = journals, cross_check
        self.keep_dir, self.timeout = keep_dir, timeout
        self.scale = space.full_scale(params)
        self.runs = 0

    def decode(self, genome: Any) -> space.Scenario:
        return space.decode(genome, self.ticks, self.seed, self.level,
                            self.params, self.trajectory)

    def _folder(self) -> Tuple[str, bool]:
        """Where this run writes; the bool says whether we must remove it."""
        self.runs += 1
        if self.keep_dir:
            path = os.path.join(self.keep_dir, "run-%06d" % self.runs)
            os.makedirs(path, exist_ok=True)
            return path, False
        return tempfile.mkdtemp(prefix="relais-wc-"), True

    def command(self, scenario: space.Scenario, folder: Optional[str]) -> List[str]:
        cmd = [self.sim] + scenario.cli()
        if folder is not None:
            cmd += ["--journal", os.path.join(folder, "actuator.jsonl"),
                    "--twin-journal", os.path.join(folder, "twin.jsonl")]
        return cmd

    def measure(self, scenario: space.Scenario,
                folder: Optional[str] = None) -> Measurement:
        """One run. `folder` pins where the journals go; otherwise the oracle
        uses its keep_dir (published runs) or a temporary folder it removes."""
        temp = False
        if folder is None and self.journals:
            folder, temp = self._folder()
        try:
            if folder is not None:
                write_json(os.path.join(folder, "scenario.json"), scenario.to_dict())
            started = time.monotonic()
            proc = subprocess.run(self.command(scenario, folder), cwd=self.root,
                                  capture_output=True, text=True,
                                  timeout=self.timeout)
            m = parse_summary(proc.stdout, proc.returncode,
                              time.monotonic() - started)
            m.stderr = proc.stderr.strip()[-400:]
            if folder is not None and self.cross_check and not m.broken:
                m.cross = cross_check(folder, self.level, m)
            return m
        except (OSError, subprocess.SubprocessError) as exc:
            return Measurement(rc=2, stderr="oracle failed: %s" % exc)
        finally:
            if temp and folder:
                shutil.rmtree(folder, ignore_errors=True)

    def evaluate(self, genome: Any) -> sx.Candidate:
        """The paid evaluation the search calls: genome -> scored Candidate."""
        scenario = self.decode(genome)
        m = self.measure(scenario)
        score = objective(m, self.ticks, self.scale)
        return sx.Candidate(genome=tuple(genome), fitness=score.fitness,
                            counter_example=score.counter_example,
                            error=m.broken,
                            payload={"scenario": scenario, "measurement": m,
                                     "score": score})


class Ledger:
    """Records every evaluation, so the report can rank them afterwards."""

    def __init__(self, oracle: SimOracle) -> None:
        self.oracle = oracle
        self.entries: List[sx.Candidate] = []

    def evaluate(self, genome: Any) -> sx.Candidate:
        candidate = self.oracle.evaluate(genome)
        self.entries.append(candidate)
        return candidate

    def counter_examples(self) -> List[sx.Candidate]:
        return [c for c in self.entries if c.counter_example]

    def clean_best(self) -> Optional[sx.Candidate]:
        """The worst case that does NOT breach any invariant."""
        clean = [c for c in self.entries if not c.counter_example and not c.error]
        return max(clean, key=lambda c: c.fitness) if clean else None

    def errors(self) -> int:
        return sum(1 for c in self.entries if c.error)

    def best_by_kind(self) -> Dict[str, sx.Candidate]:
        """The worst case found for each combination of fault kinds."""
        out: Dict[str, sx.Candidate] = {}
        for candidate in self.entries:
            key = "+".join(candidate.payload["scenario"].kinds())
            if key not in out or candidate.outranks(out[key]):
                out[key] = candidate
        return out


def entry_dict(candidate: sx.Candidate, folder: str, sim: str = SIM) -> Dict[str, Any]:
    """One candidate as JSON: the scenario, its numbers, how to replay it."""
    scenario: space.Scenario = candidate.payload["scenario"]
    measurement: Measurement = candidate.payload["measurement"]
    score: Score = candidate.payload["score"]
    replay = ["./" + sim] + scenario.cli() + [
        "--journal", folder + "/actuator.jsonl",
        "--twin-journal", folder + "/twin.jsonl"]
    return {
        "label": scenario.label(),
        "fitness": round(candidate.fitness, 6),
        "counter_example": candidate.counter_example,
        "score": score.to_dict(),
        "measurement": measurement.to_dict(),
        "scenario": scenario.to_dict(),
        "genome": [round(g, 6) for g in candidate.genome],
        "replay_command": " ".join(replay),
        "bench_command": "python3 host/bench/relais_bench.py --mode pc "
                         "--scenario %s/scenario.json" % folder,
        "verify_command": "./build/relais-verify %s/actuator.jsonl --level %s "
                          "--twin %s/twin.jsonl%s"
                          % (folder, scenario.level, folder,
                             " --double-fault" if len(scenario.faults) >= 2 else ""),
    }


def make_oracle(args: argparse.Namespace, level: str,
                keep_dir: Optional[str] = None) -> SimOracle:
    """The oracle of one search: one level, one simulator seed."""
    return SimOracle(ticks=args.ticks, seed=args.sim_seed, level=level,
                     params=args.params, trajectory=args.trajectory, root=args.root,
                     sim=args.sim, journals=not args.no_journals,
                     cross_check=args.cross_check, keep_dir=keep_dir,
                     timeout=args.timeout)


def run_one(args: argparse.Namespace, level: str, search_seed: int,
            deadline: Optional[float]) -> Tuple[Dict[str, Any], Ledger, sx.SearchResult]:
    """One independent search: `budget` evaluations, its own random seed."""
    ledger = Ledger(make_oracle(args, level))
    left = None if deadline is None else max(1.0, deadline - time.monotonic())
    result = sx.search(sx.Space(space.GENE_COUNT), ledger.evaluate,
                       random.Random(search_seed), budget=args.budget,
                       time_budget=left, algorithm=args.algo,
                       population=args.population, seeds=space.archetypes())
    record = {"level": level, "search_seed": search_seed,
              "algorithm": result.algorithm, "evaluations": result.evaluations,
              "generations": result.generations, "seconds": round(result.seconds, 2),
              "stalled": result.stalled,
              "best_fitness": round(result.best.fitness, 6) if result.best else None,
              "counter_examples": len(ledger.counter_examples()),
              "oracle_errors": ledger.errors()}
    return record, ledger, result


def append_log(path: str, level: str, search_seed: int,
               history: List[Dict[str, Any]]) -> None:
    """The research journal: one JSON line per generation (score, diversity)."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "a", encoding="utf-8") as handle:
        for row in history:
            line = dict(row)
            line.update({"level": level, "search_seed": search_seed})
            handle.write(json.dumps(line, sort_keys=True) + "\n")


def publish_best(args: argparse.Namespace, out_abs: str, out_rel: str,
                 best: sx.Candidate, name: str = "best") -> Dict[str, Any]:
    """Replay the winner once into <out>/<name>, with journals and cross-check."""
    scenario: space.Scenario = best.payload["scenario"]
    folder = os.path.join(out_abs, name)
    oracle = make_oracle(args, scenario.level)
    oracle.cross_check = True
    measured = oracle.measure(scenario, folder)
    entry = entry_dict(best, out_rel + "/" + name, args.sim)
    entry["published"] = measured.to_dict()
    entry["published_agrees"] = measured.cross.get("agrees")
    return entry


def merge_ledgers(ledgers: List[Ledger], args: argparse.Namespace,
                  out_rel: str) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    """Merge every search: the worst case per fault kind, and the counter-
    examples, deduplicated by scenario label and ranked by fitness."""
    by_kind: Dict[str, sx.Candidate] = {}
    found: List[sx.Candidate] = []
    for ledger in ledgers:
        for key, candidate in ledger.best_by_kind().items():
            if key not in by_kind or candidate.outranks(by_kind[key]):
                by_kind[key] = candidate
        found.extend(ledger.counter_examples())
    found.sort(key=lambda c: -c.fitness)
    unique = {c.payload["scenario"].label(): c for c in found}
    table = {k: entry_dict(v, "%s/by-kind/%s" % (out_rel, k), args.sim)
             for k, v in sorted(by_kind.items())}
    examples = [entry_dict(c, "%s/counter-examples" % out_rel, args.sim)
                for c in list(unique.values())[:12]]
    return table, examples


def search_all(args: argparse.Namespace, out_abs: str,
               out_rel: str) -> Dict[str, Any]:
    """Every search asked for, then the merged verdict of the adversary."""
    levels = LEVELS if args.level == "both" else (args.level,)
    deadline = None if args.time_budget is None else time.monotonic() + args.time_budget
    started = time.monotonic()
    log_path = os.path.join(out_abs, "search_log.jsonl")
    if os.path.exists(log_path):
        os.remove(log_path)
    searches: List[Dict[str, Any]] = []
    ledgers: List[Ledger] = []
    best: Optional[sx.Candidate] = None
    clean: Optional[sx.Candidate] = None
    for level in levels:
        for index in range(max(1, args.seeds)):
            seed = args.search_seed + index
            record, ledger, result = run_one(args, level, seed, deadline)
            searches.append(record)
            ledgers.append(ledger)
            append_log(log_path, level, seed, result.history)
            if result.best is not None and (best is None or result.best.outranks(best)):
                best = result.best
            found = ledger.clean_best()
            if found is not None and (clean is None or found.outranks(clean)):
                clean = found
    table, examples = merge_ledgers(ledgers, args, out_rel)
    evaluations = sum(r["evaluations"] for r in searches)
    doc: Dict[str, Any] = {
        "tool": "host/optimize/worstcase.py", "requirement": "INJ-12",
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "config": {"ticks": args.ticks, "sim_seed": args.sim_seed,
                   "levels": list(levels), "params": args.params,
                   "trajectory": args.trajectory, "sim": args.sim,
                   "full_scale": space.full_scale(args.params)},
        "budget": {"evaluations_per_search": args.budget, "seeds": args.seeds,
                   "search_seed": args.search_seed, "algorithm": args.algo,
                   "population": args.population,
                   "time_budget_s": args.time_budget,
                   "evaluations_total": evaluations,
                   "wall_s": round(time.monotonic() - started, 2)},
        "searches": searches,
        "best": publish_best(args, out_abs, out_rel, best) if best else None,
        "clean_best": publish_best(args, out_abs, out_rel, clean, "clean-best")
                      if clean else None,
        "worst_by_kind": table,
        "counter_examples": examples,
        "search_log": os.path.join(out_rel, "search_log.jsonl"),
    }
    return doc


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--budget", type=int, default=400,
                    help="evaluations per search (default 400)")
    ap.add_argument("--time-budget", type=float, default=600.0,
                    help="wall-clock seconds for the whole search (default 600)")
    ap.add_argument("--seeds", type=int, default=1,
                    help="independent searches, the best is kept (default 1)")
    ap.add_argument("--search-seed", type=int, default=1,
                    help="seed of the first search; seed i is search-seed + i")
    ap.add_argument("--population", type=int, default=24)
    ap.add_argument("--algo", default="auto", choices=("auto", "evo", "anneal"))
    ap.add_argument("--ticks", type=int, default=2000, help="ticks per run")
    ap.add_argument("--sim-seed", type=int, default=7, help="seed of the simulator")
    ap.add_argument("--level", default="both", choices=("socle", "complet", "both"))
    ap.add_argument("--params", default="", help='relais-simulate --params spec')
    ap.add_argument("--trajectory", default="",
                    help='relais-simulate --trajectory spec')
    ap.add_argument("--sim", default=SIM, help="simulator binary, relative to --root")
    ap.add_argument("--root", default=ROOT, help="repository root")
    ap.add_argument("--out-dir", default=os.path.join("build", "worstcase"))
    ap.add_argument("--no-journals", action="store_true",
                    help="do not write actuator.jsonl/twin.jsonl per evaluation")
    ap.add_argument("--cross-check", action="store_true",
                    help="re-read every journal with host/report and compare")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--report", help="also write the text report to this file")
    ap.add_argument("--quiet", action="store_true")
    return ap


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    binary = args.sim if os.path.isabs(args.sim) else os.path.join(args.root, args.sim)
    if not os.path.isfile(binary):
        sys.stderr.write("no simulator at %s — run `make` first\n" % binary)
        return 2
    out_rel = args.out_dir
    out_abs = out_rel if os.path.isabs(out_rel) else os.path.join(args.root, out_rel)
    os.makedirs(out_abs, exist_ok=True)
    doc = search_all(args, out_abs, out_rel.rstrip("/"))
    write_json(os.path.join(out_abs, "worstcase.json"), doc)
    report = report_worst.text_report(doc)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as handle:
            handle.write(report + "\n")
    if not args.quiet:
        print(report)
    print("\nwrote %s and %s" % (os.path.join(out_rel, "worstcase.json"),
                                 doc["search_log"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
