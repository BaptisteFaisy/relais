#!/usr/bin/env python3
"""Relais — the mutation score of the core (idea 10, docs/MUTATION-SCORE.md).

Answer "are your tests strong?" with a measured number, not with the eight
hand-made mutants of spec 7.6. Small mechanical faults are injected into
`core/src/*.c`, each mutant is compiled in a temporary tree (no -DRELAIS_MUTANT:
the fault is a source patch), then the whole test suite plus reduced
simulator/explorer scenarios decide alive-or-dead. A mutant that builds and
behaves byte-identically on every oracle is reported separately as *equivalent
(suspected)* — it is not a failure, it carries no information.

Requirement IDs: INJ-11 (mutants run the campaign), OBS-05 (the report cites
the command and the bound).

Usage:
  python3 host/mutants/mutation_score.py --max-mutants 40 --time-budget 240
  python3 host/mutants/mutation_score.py --list
"""
import argparse
import hashlib
import json
import multiprocessing
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from mutation_ops import apply_mutation, generate, select

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# --------------------------------------------------------------------------
# One runnable project: what to build, what oracle passes, what kills.
# --------------------------------------------------------------------------
class Project:
    def __init__(self, name, sources, build, tests, scenarios, build_timeout=180,
                 test_timeout=120, root=None):
        self.name = name
        self.root = root or ROOT
        self.sources = sources                # files to mutate (relative to root)
        self.build = build                    # argv, run cwd=workdir
        self.tests = tests                    # list of argv, non-zero = killed
        self.scenarios = scenarios            # list of argv, violation = killed
        self.build_timeout = build_timeout
        self.test_timeout = test_timeout


def repo_project():
    src = sorted(f for f in os.listdir(os.path.join(ROOT, "core", "src"))
                 if f.endswith(".c"))
    sources = ["core/src/" + f for f in src]
    build = ["make", "-j4", "all"]
    tests = [["make", "test"]]
    scenarios = [
        ["./build/relais-simulate", "--ticks", "2000", "--seed", "3"],
        ["./build/relais-simulate", "--ticks", "2000", "--seed", "3", "--level", "complet"],
        ["./build/relais-simulate", "--ticks", "2000", "--seed", "5", "--level", "complet",
         "--fault", "400:death:0"],
        ["./build/relais-simulate", "--ticks", "1500", "--seed", "5", "--level", "complet",
         "--fault", "600:freeze:0:20"],
        ["./build/relais-explore", "--budget", "20000", "--depth", "8", "--faults", "1", "--quiet"],
        ["./build/relais-explore", "--budget", "20000", "--depth", "8", "--faults", "2", "--quiet"],
    ]
    return Project("core", sources, build, tests, scenarios)


# --------------------------------------------------------------------------
# Verdicts and the engine.
# --------------------------------------------------------------------------
def _sha(data):
    return hashlib.sha256(data.encode("utf-8")).hexdigest()


def _run(argv, cwd, timeout):
    proc = subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                          timeout=timeout)
    return proc.returncode, proc.stdout + "\n" + proc.stderr


def _needs_journal(argv):
    base = os.path.basename(argv[0] or "")
    return "simulate" in base


def _run_pipeline(project, workdir, tmp):
    """Run the oracles in an already-built workdir. Returns
    (status in {ok,killed}, evidence_hash, detail)."""
    evidence = []
    for argv in project.tests:
        rc, out = _run(argv, workdir, project.test_timeout)
        evidence.append(out)
        if rc != 0:
            return "killed", None, "test %s failed\n%s" % (argv[0], out[-600:])
    for i, argv in enumerate(project.scenarios):
        argv = list(argv)
        journal = None
        if _needs_journal(argv):
            journal = os.path.join(tmp, "scen_%d.jsonl" % i)
            argv = argv + ["--journal", journal]
        rc, out = _run(argv, workdir, project.test_timeout)
        jdata = ""
        if journal and os.path.exists(journal):
            with open(journal, encoding="utf-8") as fh:
                jdata = fh.read()
        evidence.append(out)
        evidence.append(jdata)
        if rc != 0:
            return "killed", None, "scenario %s failed\n%s" % (
                " ".join(argv), out[-600:])
        if re.search(r"violations=[1-9]|EXPLORE-ECHEC", out):
            return "killed", None, "scenario violation: %s" % out[-300:]
    return "ok", _sha("".join(evidence)), None


def _prepare_template(project, jobs):
    template = tempfile.mkdtemp(prefix="mscore-template-")
    shutil.copytree(project.root, template, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(".git"))
    _run(project.build, template, project.build_timeout)
    return template


def _evaluate(args):
    """Worker body: mutate, rebuild, run, classify. Returns a dict."""
    project, template, mutation, nominal_hash, jobs = args
    dest = tempfile.mkdtemp(prefix="mscore-")
    shutil.copytree(template, dest, dirs_exist_ok=True)
    start = time.monotonic()
    try:
        if mutation is not None:
            apply_mutation(os.path.join(dest, mutation.file), mutation)
            rc, out = _run(project.build, dest, project.build_timeout)
            if rc != 0:
                return _result("invalid", mutation, 0.0, "build failed: " + out[-400:])
        status, h, detail = _run_pipeline(project, dest, dest)
        elapsed = time.monotonic() - start
        if status == "killed":
            return _result("killed", mutation, elapsed, detail or "")
        if h == nominal_hash:
            return _result("equivalent", mutation, elapsed, "byte-identical on every oracle")
        return _result("survived", mutation, elapsed, "behaviour changed but no oracle failed")
    except subprocess.TimeoutExpired:
        return _result("invalid", mutation, 0.0, "timeout")
    finally:
        shutil.rmtree(dest, ignore_errors=True)


def _result(status, mutation, elapsed, detail):
    return {"status": status, "file": mutation.file if mutation else None,
            "line": mutation.line if mutation else None,
            "col": mutation.col if mutation else None,
            "operator": mutation.operator if mutation else None,
            "before": mutation.before if mutation else None,
            "after": mutation.after if mutation else None,
            "elapsed": elapsed, "detail": detail[:200]}


def run_score(project, mutations, jobs, time_budget):
    """Run all mutants under a wall-clock budget. Returns (rows, partial)."""
    template, nominal_hash, _detail = prepare_nominal(project, jobs)
    try:
        rows, partial = _drive(project, template, mutations, jobs,
                               time_budget, nominal_hash)
    finally:
        shutil.rmtree(template, ignore_errors=True)
    return rows, partial


def prepare_nominal(project, jobs):
    """Build once and return (template, nominal_hash, detail)."""
    template = _prepare_template(project, jobs)
    status, h, detail = _run_pipeline(project, template, template)
    if status != "ok":
        raise SystemExit("nominal core is not clean: %s" % detail)
    return template, h, detail


def _drive(project, template, mutations, jobs, time_budget, nominal_hash):
    rows = []
    deadline = time.monotonic() + time_budget if time_budget > 0 else None
    if jobs <= 1:
        for m in mutations:
            if deadline is not None and time.monotonic() >= deadline:
                return rows, True
            rows.append(_evaluate((project, template, m, nominal_hash, 1)))
    else:
        pool = multiprocessing.Pool(jobs)
        try:
            iterable = pool.imap_unordered(
                _evaluate, [(project, template, m, nominal_hash, jobs)
                            for m in mutations])
            for row in iterable:
                rows.append(row)
                if deadline is not None and time.monotonic() >= deadline:
                    pool.terminate()
                    return rows, True
        finally:
            pool.terminate()
    return rows, False


# --------------------------------------------------------------------------
# Scoring and reporting.
# --------------------------------------------------------------------------
def score(rows):
    counts = {"killed": 0, "survived": 0, "equivalent": 0, "invalid": 0}
    for r in rows:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    denominator = counts["killed"] + counts["survived"]
    pct = counts["killed"] / denominator if denominator else None
    return counts, pct


def conservative_pct(counts):
    denominator = counts["killed"] + counts["survived"] + counts["equivalent"]
    return counts["killed"] / denominator if denominator else None


def survivors(rows):
    return [r for r in rows if r["status"] == "survived"]


def equivalents(rows):
    return [r for r in rows if r["status"] == "equivalent"]


def invalid(rows):
    return [r for r in rows if r["status"] == "invalid"]


def to_json(rows, opts, counts, pct, real_seconds):
    return {
        "project": "core", "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "budget_seconds": opts.time_budget, "max_mutants": opts.max_mutants,
        "evaluated": len(rows), "counts": counts, "score_percent": pct,
        "score_conservative_percent": conservative_pct(counts),
        "survivors": [{k: r[k] for k in ("file", "line", "col", "operator",
                                          "before", "after")} for r in survivors(rows)],
        "equivalent": [{k: r[k] for k in ("file", "line", "operator", "before",
                                           "after")} for r in equivalents(rows)],
        "invalid": [{k: r[k] for k in ("file", "line", "operator", "before",
                                        "after")} for r in invalid(rows)],
        "duration_seconds": real_seconds,
    }


def to_markdown(rows, opts, counts, pct, real_seconds, note):
    strict = pct
    consv = conservative_pct(counts)
    lines = ["## Mutation score of the core (spec 7.6, idea 10)", ""]
    lines.append("| Mutants | Killed | Survived | Equivalent (suspected) | "
                 "Invalid (no build) | Score |")
    lines.append("|---|---|---|---|---|---|")
    lines.append("| %d | %d | %d | %d | %d | %s |" % (
        len(rows), counts["killed"], counts["survived"], counts["equivalent"],
        counts["invalid"], ("%.1f %%" % (strict * 100)) if strict is not None else "—"))
    lines.append("")
    lines.append("Score is `killed / (killed + survived)` (standard convention: "
                 "mutants with a byte-identical observable behaviour are "
                 "suspected equivalent and excluded). Counting every suspected "
                 "equivalent as a miss, the conservative score is %s. Treat the "
                 "%d suspected equivalents as the honest margin of doubt: a "
                 "pass/fail oracle cannot separate a survivor from an equivalent "
                 "automatically." % (
                     "%.1f %%" % (consv * 100) if consv is not None else "—",
                     counts["equivalent"]))
    lines.append("")
    lines.append("Command: `python3 host/mutants/mutation_score.py "
                 "--max-mutants %d --time-budget %d` (%s)." % (
                     opts.max_mutants, opts.time_budget, note))
    lines.append("")
    if survivors(rows):
        lines.append("Survivors — implementation faults the current oracle does "
                     "not catch:")
        lines.append("")
        for r in sorted(survivors(rows), key=lambda x: (x["file"], x["line"])):
            lines.append("- `%s:%d` — `%s` → `%s` (%s)" % (
                r["file"], r["line"], r["before"], r["after"], r["operator"]))
        lines.append("")
    if equivalents(rows):
        lines.append("Equivalent (suspected) — byte-identical on every oracle, "
                     "carry no information, excluded from the score:")
        lines.append("")
        for r in equivalents(rows):
            lines.append("- `%s:%d` — `%s` → `%s` (%s)" % (
                r["file"], r["line"], r["before"], r["after"], r["operator"]))
        lines.append("")
    return "\n".join(lines)


def read_sources(paths, root):
    out = {}
    for p in paths:
        with open(os.path.join(root, p), encoding="utf-8") as fh:
            out[p] = fh.read().splitlines()
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Mutation score of the Relais core.")
    ap.add_argument("--max-mutants", type=int, default=0)
    ap.add_argument("--time-budget", type=int, default=0,
                    help="wall-clock seconds; 0 = unlimited")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--json", help="write the full result to this file")
    ap.add_argument("--markdown", help="write a RESULTS-ready block to this file")
    ap.add_argument("--list", action="store_true", help="list mutants and exit")
    opts = ap.parse_args(argv)

    project = repo_project()
    lines_by_file = read_sources(project.sources, project.root)
    all_mutants = generate(lines_by_file)
    if opts.list:
        for m in select(all_mutants, 0):
            print("%s:%d:%d %8s  %-8r -> %s" % (
                m.file, m.line, m.col, m.operator, m.before, m.after))
        print("%d mutants" % len(all_mutants))
        return 0

    jobs = opts.jobs or min(8, multiprocessing.cpu_count() // 2)
    mutations = select(all_mutants, opts.max_mutants)
    print("== Relais mutation score ==")
    print("%d mutants generated, %d selected, %d jobs" % (
        len(all_mutants), len(mutations), jobs))

    start = time.monotonic()
    rows, partial = run_score(project, mutations, jobs, opts.time_budget)
    real_seconds = time.monotonic() - start

    counts, pct = score(rows)
    note = "partial run (time budget hit)" if partial else "full run, %.1f s" % real_seconds
    md = to_markdown(rows, opts, counts, pct, real_seconds, note)

    print("\n%-10s %6d" % ("killed", counts["killed"]))
    print("%-10s %6d" % ("survived", counts["survived"]))
    print("%-10s %6d" % ("equiv", counts["equivalent"]))
    print("%-10s %6d" % ("invalid", counts["invalid"]))
    if pct is not None:
        print("%-10s %6.1f %%" % ("score", pct * 100))
        consv = conservative_pct(counts)
        if consv is not None:
            print("%-10s %6.1f %%" % ("score-consv", consv * 100))
    for r in survivors(rows)[:20]:
        print("SURVIVOR %s:%d  %r -> %r (%s)" % (
            r["file"], r["line"], r["before"], r["after"], r["operator"]))

    payload = to_json(rows, opts, counts, pct, real_seconds)
    payload["partial"] = partial
    if opts.json:
        with open(opts.json, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2, sort_keys=True)
    if opts.markdown:
        with open(opts.markdown, "w", encoding="utf-8") as fh:
            fh.write(md + "\n")
    print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())