#!/usr/bin/env python3
"""Relais — the worst-case account, in text and in markdown (INJ-12, OBS-05).

Formats the result of host/optimize/worstcase.py: a readable account of what the
adversary spent and found (`text_report`, printed by the search itself), and a
markdown block ready to paste into docs/RESULTS.md (`markdown_report`, the table
of the worst case per fault kind with its deviation, its hole and the exact
command that replays it).

Usage:
  python3 host/optimize/report_worst.py                 # build/worstcase/worstcase.json
  python3 host/optimize/report_worst.py --text FILE.json
  python3 host/optimize/report_worst.py --out docs/worst.md FILE.json ...
Exit code 0, or 2 when no result file can be read.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any, Dict, List, Optional, Sequence

DEFAULT_RESULT = os.path.join("build", "worstcase", "worstcase.json")
COVERAGE = (
    "The adversary searches the **modelled** fault space only: six fault kinds, "
    "two machines, three link directions, one or two nested faults, one simulator "
    "seed per run. It says nothing about fine hardware faults (bit flips, partial "
    "frames, clock drift, actuator mechanics) nor about QNX timing: the published "
    "worst case is the worst case *of the model*, with the seed and the command "
    "that replay it.")


def load(path: str) -> Dict[str, Any]:
    """One result file written by worstcase.py."""
    with open(path, "r", encoding="utf-8") as handle:
        doc = json.load(handle)
    if not isinstance(doc, dict):
        raise ValueError("%s is not a worstcase result" % path)
    return doc


def period_ms(doc: Dict[str, Any]) -> float:
    """The period T of the run, in milliseconds, from its parameter spec."""
    params = str((doc.get("config") or {}).get("params", ""))
    for chunk in params.replace(" ", ",").split(","):
        if chunk.startswith("T="):
            try:
                return float(chunk[2:])
            except ValueError:
                return 5.0
    return 5.0


def numbers(entry: Dict[str, Any], period: float) -> Dict[str, Any]:
    """The four figures of one entry, already formatted for a table."""
    m = entry.get("measurement", {})
    score = entry.get("score", {})
    return {"label": entry.get("label", "?"),
            "deviation": m.get("max_deviation", 0.0),
            "dev_pct": 100.0 * float(score.get("dev_norm", 0.0)),
            "bound": m.get("deviation_bound", 0.02),
            "hole": m.get("max_gap", 0),
            "hole_ms": m.get("max_gap", 0) * period,
            "hole_bound": m.get("gap_bound", "?"),
            "violations": score.get("violations", 0),
            "counter_example": bool(entry.get("counter_example")),
            "first": m.get("first", ""),
            "fallback": m.get("fallback_run", 0),
            "replay": entry.get("replay_command", ""),
            "bench": entry.get("bench_command", ""),
            "verify": entry.get("verify_command", "")}


def headline(doc: Dict[str, Any]) -> List[str]:
    """What was spent, in one paragraph."""
    cfg, budget = doc.get("config", {}), doc.get("budget", {})
    period = period_ms(doc)
    return ["budget: %s evaluations x %s seed(s) x %d level(s) = **%s simulator "
            "runs** in %s s (algorithm `%s`, population %s)"
            % (budget.get("evaluations_per_search"), budget.get("seeds"),
               len(cfg.get("levels", [])), budget.get("evaluations_total"),
               budget.get("wall_s"), budget.get("algorithm"),
               budget.get("population")),
            "run: %s ticks (%.0f ms emulated), simulator seed %s, search seed %s"
            % (cfg.get("ticks"), int(cfg.get("ticks", 0)) * period,
               cfg.get("sim_seed"), budget.get("search_seed"))]


def _entry_lines(fig: Dict[str, Any]) -> List[str]:
    """The figures of one entry, as indented text lines."""
    flag = "  <- COUNTER-EXAMPLE: " + fig["first"] if fig["counter_example"] else ""
    return ["  scenario      : %s" % fig["label"],
            "  max deviation : %.6g of full scale (%.1f %%), bound %.3g"
            % (fig["deviation"], fig["dev_pct"], fig["bound"]),
            "  max hole      : %d ticks without a fresh order (%.0f ms), bound %s"
            % (fig["hole"], fig["hole_ms"], fig["hole_bound"]),
            "  violations    : %d%s" % (fig["violations"], flag),
            "  fallback run  : %d ticks" % fig["fallback"],
            "  replay        : %s" % fig["replay"],
            "  bench         : %s" % fig["bench"],
            "  offline verify: %s" % fig["verify"]]


def text_report(doc: Dict[str, Any]) -> str:
    """The readable account printed at the end of a search."""
    period = period_ms(doc)
    lines = ["Relais - adversarial worst-case fault search (INJ-12, spec 12 #1)"]
    lines += headline(doc)
    best = doc.get("best")
    if best:
        lines.append("worst case found (counter-example, reported first):")
        lines += _entry_lines(numbers(best, period))
    clean = doc.get("clean_best")
    if clean:
        lines.append("worst case WITHOUT an invariant breach:")
        lines += _entry_lines(numbers(clean, period))
    examples = doc.get("counter_examples", [])
    lines.append("counter-examples (invariant violations): %d distinct" % len(examples))
    for entry in examples[:3]:
        fig = numbers(entry, period)
        lines.append("  ! %s -> %s" % (fig["label"], fig["first"] or "?"))
    lines.append("worst case per fault kind:")
    lines.append("  %-26s %10s %6s %5s" % ("kinds", "dev/full", "hole", "viol"))
    for key, entry in (doc.get("worst_by_kind") or {}).items():
        fig = numbers(entry, period)
        lines.append("  %-26s %10.4f %6d %5d"
                     % (key, fig["dev_pct"] / 100.0, fig["hole"], fig["violations"]))
    lines.append("coverage: " + COVERAGE)
    return "\n".join(lines)


def _table(doc: Dict[str, Any]) -> List[str]:
    """The markdown table of the worst case per fault kind."""
    period = period_ms(doc)
    rows = ["| Fault kind | Worst scenario | Deviation from the twin | Hole "
            "between fresh orders | Violations |",
            "|---|---|---|---|---|"]
    for key, entry in (doc.get("worst_by_kind") or {}).items():
        fig = numbers(entry, period)
        rows.append("| `%s` | %s | **%.6g** (%.1f %% of full scale) | %d ticks "
                    "(%.0f ms) | %d |"
                    % (key, fig["label"], fig["deviation"], fig["dev_pct"],
                       fig["hole"], fig["hole_ms"], fig["violations"]))
    return rows


def markdown_report(doc: Dict[str, Any]) -> str:
    """The markdown block to paste into docs/RESULTS.md."""
    period = period_ms(doc)
    out = ["### Worst case found by the adversary (INJ-12)", ""]
    out += ["`python3 host/optimize/worstcase.py` replaced the regular fault sweep",
            "by an optimizer that chooses the instant, the kind, the duration, the",
            "target machine, the link direction and the nesting of two faults, to",
            "maximise the deviation from the fault-free twin.", ""]
    out += headline(doc)
    out.append("")
    best = doc.get("best")
    if best:
        fig = numbers(best, period)
        out += ["**Worst case published** (a counter-example: an invariant was"
                " breached, so it is a proof, not a score). %s" % fig["label"], "",
                "- deviation from the twin: **%.6g** of full scale (%.1f %%), "
                "bound delta = %.3g" % (fig["deviation"], fig["dev_pct"], fig["bound"]),
                "- hole between two fresh orders: **%d ticks** (%.0f ms), bound %s"
                % (fig["hole"], fig["hole_ms"], fig["hole_bound"]),
                "- invariant violations: **%d**%s" % (
                    fig["violations"],
                    "" if not fig["counter_example"] else " (%s)" % fig["first"]),
                "- fallback run: %d ticks" % fig["fallback"], "",
                "```", fig["replay"], "```", "",
                "Same scenario on the bench: `%s`; offline check of its journal: "
                "`%s`." % (fig["bench"], fig["verify"]), ""]
    clean = doc.get("clean_best")
    if clean:
        fig = numbers(clean, period)
        out += ["**Worst case without an invariant breach.** %s" % fig["label"], "",
                "- deviation from the twin: **%.6g** of full scale (%.1f %%), "
                "bound delta = %.3g" % (fig["deviation"], fig["dev_pct"], fig["bound"]),
                "- hole between two fresh orders: **%d ticks** (%.0f ms), bound %s"
                % (fig["hole"], fig["hole_ms"], fig["hole_bound"]),
                "- fallback run: %d ticks" % fig["fallback"], "",
                "```", fig["replay"], "```", ""]
    examples = doc.get("counter_examples", [])
    if examples:
        out += ["**Counter-examples.** %d distinct scenarios breach an invariant; "
                "they are published first, as proofs, not as scores:" % len(examples),
                ""]
        for entry in examples[:5]:
            fig = numbers(entry, period)
            out.append("- `%s` -> %s" % (fig["label"], fig["first"] or "?"))
        out.append("")
    out += _table(doc)
    out += ["", COVERAGE, ""]
    return "\n".join(out)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("results", nargs="*", default=[DEFAULT_RESULT],
                    help="worstcase.json files (default %s)" % DEFAULT_RESULT)
    ap.add_argument("--text", action="store_true", help="print the text account")
    ap.add_argument("--out", help="write the markdown to this file")
    args = ap.parse_args(argv)
    paths = args.results or [DEFAULT_RESULT]
    blocks: List[str] = []
    for path in paths:
        try:
            doc = load(path)
        except (OSError, ValueError) as exc:
            sys.stderr.write("cannot read %s: %s\n" % (path, exc))
            return 2
        blocks.append(text_report(doc) if args.text else markdown_report(doc))
    text = ("\n\n---\n\n".join(blocks)).rstrip() + "\n"
    if args.out:
        folder = os.path.dirname(args.out)
        if folder:
            os.makedirs(folder, exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text)
        sys.stderr.write("wrote %s\n" % args.out)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
