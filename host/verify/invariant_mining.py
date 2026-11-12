#!/usr/bin/env python3
"""Relais — automatic invariant discovery over a corpus of journals (M12).

The actuator writes one JSON object per line (header / tick / event, see
CONTRACT.md and core/src/journal.c). This miner reads a corpus of such journals
and evaluates a library of candidate relations — implications between events,
numeric bounds, exclusions and orderings — reporting, for each, how many
journals confirm it and with what confidence.

A candidate is *confirmed* when it holds in every journal that exercised it
(support > 0); it is a *candidate to confirm* when at least one journal
contradicts it; it is *vacuous* when no journal ever exercised it. Results are
sorted by confidence then by total support, so the strongest, best-supported
relations come first.

Standard library only. Usage:
    python3 invariant_mining.py [--min-confidence F] [--json] PATH [PATH ...]
PATH is a journal file or a directory of *.json journals. Exit code is always 0
(the miner reports, it does not gate); use --json for machine consumption.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

# The journal header carries p / eps / delta but not the command bounds, which
# the actuator clamps to the compiled defaults (RELAIS_FULL_SCALE, max_slew).
DEFAULT_MAX_AMPLITUDE = 1.0
DEFAULT_MAX_SLEW = 0.25
EPS = 1e-6  # float slack when comparing journal values written with %.6g


class Journal:
    """One parsed journal: its params, its tick records and its events."""

    def __init__(self, path: str):
        self.path = path
        self.params: dict = {}
        self.ticks: list[dict] = []
        self.events: list[dict] = []
        self.tick_by_num: dict[int, dict] = {}
        self.malformed = 0

    def patience(self) -> int:
        return int(self.params.get("p", 1))

    def max_amplitude(self) -> float:
        return float(self.params.get("max_amplitude", DEFAULT_MAX_AMPLITUDE))

    def max_slew(self) -> float:
        return float(self.params.get("max_slew", DEFAULT_MAX_SLEW))

    def tick_before(self, tick: int) -> dict | None:
        return self.tick_by_num.get(tick - 1)

    def tick_after(self, tick: int) -> dict | None:
        return self.tick_by_num.get(tick + 1)

    def events_of(self, kind: str) -> list[dict]:
        return [e for e in self.events if e.get("event") == kind]


def load_journal(path: str) -> Journal:
    """Parse one journal file, skipping malformed lines without crashing."""
    j = Journal(path)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except (ValueError, TypeError):
                    j.malformed += 1
                    continue
                kind = obj.get("kind")
                if kind == "header":
                    j.params = dict(obj.get("params", {}))
                elif kind == "tick":
                    j.ticks.append(obj)
                    j.tick_by_num[int(obj.get("tick", 0))] = obj
                elif kind == "event":
                    j.events.append(obj)
    except OSError:
        pass
    return j


def load_corpus(paths: list[str]) -> list[Journal]:
    """Expand directories into their *.json journals and load them all."""
    files: list[str] = []
    for p in paths:
        if os.path.isdir(p):
            for name in sorted(os.listdir(p)):
                if name.endswith(".json"):
                    files.append(os.path.join(p, name))
        elif os.path.isfile(p):
            files.append(p)
    return [load_journal(f) for f in files]


# --- candidate checks -------------------------------------------------------
# Each returns (holds, support): support is the number of observations where
# the relation was applicable; holds is True when none of them contradicted it.
# A check with support 0 is vacuous (never exercised by this journal).


def _all(values: list[bool]) -> tuple[bool, int]:
    return (all(values), len(values))


def chk_amplitude(j: Journal) -> tuple[bool, int]:
    amp = j.max_amplitude()
    return _all([abs(float(t.get("value", 0.0))) <= amp + EPS for t in j.ticks])


def chk_slew(j: Journal) -> tuple[bool, int]:
    slew = j.max_slew()
    ok = []
    for a, b in zip(j.ticks, j.ticks[1:]):
        ok.append(abs(float(b.get("value", 0)) - float(a.get("value", 0))) <= slew + EPS)
    return _all(ok)


def chk_holder_fresh(j: Journal) -> tuple[bool, int]:
    return _all([int(t.get("misses", 0)) == 0 for t in j.ticks
                 if t.get("source") == "holder"])


def chk_fallback_red(j: Journal) -> tuple[bool, int]:
    return _all([t.get("lamp") == "red" for t in j.ticks
                 if t.get("source") == "fallback"])


def chk_nonholder_lamp_lit(j: Journal) -> tuple[bool, int]:
    return _all([t.get("lamp") != "off" for t in j.ticks
                 if t.get("source") in ("shadow", "testament", "fallback")])


def chk_lamp_off_holder(j: Journal) -> tuple[bool, int]:
    return _all([t.get("source") == "holder" for t in j.ticks
                 if t.get("lamp") == "off"])


def chk_certified_node_set(j: Journal) -> tuple[bool, int]:
    return _all([t.get("certified_node", "") != "" for t in j.ticks
                 if t.get("certified") is True])


def chk_mandate_monotone(j: Journal) -> tuple[bool, int]:
    ok = []
    for a, b in zip(j.ticks, j.ticks[1:]):
        d = int(b.get("mandate", 0)) - int(a.get("mandate", 0))
        ok.append(0 <= d <= 1)
    return _all(ok)


def chk_misses_bound(j: Journal) -> tuple[bool, int]:
    p = j.patience()
    return _all([int(t.get("misses", 0)) <= p + 1 for t in j.ticks
                 if t.get("holder", "") != ""])


def chk_grant_after_vacant(j: Journal) -> tuple[bool, int]:
    # MAN-05: a grant is legal only when the mandate is vacant at that instant.
    # The vacancy is visible either on the previous tick, or as a revoke /
    # cession that freed the mandate earlier in the SAME tick (the arbiter
    # revokes on its own evidence, then grants, within one step).
    ok = []
    for e in j.events_of("mandate_granted"):
        t = int(e.get("tick", 0))
        prev = j.tick_before(t)
        freed_same_tick = any(x.get("event") == "mandate_revoked" and
                              int(x.get("tick", 0)) == t for x in j.events)
        ok.append(prev is None or prev.get("holder", "") == "" or freed_same_tick)
    return _all(ok)


def chk_autoswitch_after_certified(j: Journal) -> tuple[bool, int]:
    ok = []
    for e in j.events_of("mandate_auto_switch"):
        prev = j.tick_before(int(e.get("tick", 0)))
        ok.append(prev is not None and prev.get("certified") is True)
    return _all(ok)


def chk_certified_next_tick(j: Journal) -> tuple[bool, int]:
    ok = []
    for e in j.events_of("certified"):
        nxt = j.tick_after(int(e.get("tick", 0)))
        ok.append(nxt is not None and nxt.get("certified") is True)
    return _all(ok)


# id, statement, kind, check
INVARIANTS = [
    ("AMP-BOUND", "|value| <= max_amplitude at every tick", "bound", chk_amplitude),
    ("SLEW-BOUND", "|value[t]-value[t-1]| <= max_slew", "bound", chk_slew),
    ("HOLDER-FRESH", "source=holder => misses=0", "exclusion", chk_holder_fresh),
    ("FALLBACK-RED", "source=fallback => lamp=red", "exclusion", chk_fallback_red),
    ("NONHOLDER-LIT", "source in {shadow,testament,fallback} => lamp!=off",
     "exclusion", chk_nonholder_lamp_lit),
    ("LAMPOFF-HOLDER", "lamp=off => source=holder", "exclusion", chk_lamp_off_holder),
    ("CERT-NODE", "certified=true => certified_node set", "exclusion",
     chk_certified_node_set),
    ("MANDATE-MONO", "mandate non-decreasing and rises by at most 1", "ordering",
     chk_mandate_monotone),
    ("MISSES-BOUND", "holder set => misses <= p+1", "bound", chk_misses_bound),
    ("GRANT-VACANT", "mandate_granted => vacant before, or freed same tick",
     "implication", chk_grant_after_vacant),
    ("AUTOSW-CERT", "mandate_auto_switch => previous tick certified", "implication",
     chk_autoswitch_after_certified),
    ("CERT-NEXT", "certified event => next tick reports certified", "implication",
     chk_certified_next_tick),
]


class Result:
    """The aggregated verdict of one candidate over the whole corpus."""

    def __init__(self, inv_id, statement, kind):
        self.id = inv_id
        self.statement = statement
        self.kind = kind
        self.confirming = 0
        self.contradicting = 0
        self.vacuous = 0
        self.support = 0
        self.first_counterexample = None

    @property
    def total(self) -> int:
        return self.confirming + self.contradicting + self.vacuous

    @property
    def confidence(self) -> float:
        exercised = self.confirming + self.contradicting
        return self.confirming / exercised if exercised else 0.0

    @property
    def status(self) -> str:
        exercised = self.confirming + self.contradicting
        if exercised == 0:
            return "vacuous"
        return "confirmed" if self.contradicting == 0 else "candidate"


def mine(journals: list[Journal]) -> list[Result]:
    """Evaluate every candidate over every journal and aggregate."""
    results = [Result(i, s, k) for (i, s, k, _) in INVARIANTS]
    for j in journals:
        for res, (_, _, _, check) in zip(results, INVARIANTS):
            holds, support = check(j)
            res.support += support
            if support == 0:
                res.vacuous += 1
            elif holds:
                res.confirming += 1
            else:
                res.contradicting += 1
                if res.first_counterexample is None:
                    res.first_counterexample = j.path
    results.sort(key=lambda r: (r.confidence, r.support, r.id), reverse=True)
    return results


def format_report(results: list[Result], n_journals: int, min_conf: float) -> str:
    lines = [f"invariant mining over {n_journals} journal(s)",
             f"{'id':<16}{'kind':<12}{'conf':>6}{'conf-j':>8}{'support':>9}  status"]
    for r in results:
        if r.confidence < min_conf and r.status != "confirmed":
            continue
        lines.append(
            f"{r.id:<16}{r.kind:<12}{r.confidence:>6.2f}"
            f"{str(r.confirming) + '/' + str(r.total):>8}{r.support:>9}  {r.status}")
        lines.append(f"    {r.statement}")
        if r.first_counterexample:
            lines.append(f"    first counterexample: {r.first_counterexample}")
    return "\n".join(lines)


def to_json(results: list[Result], n_journals: int) -> str:
    out = {"journals": n_journals, "invariants": [
        {"id": r.id, "kind": r.kind, "statement": r.statement,
         "confidence": round(r.confidence, 4), "confirming": r.confirming,
         "contradicting": r.contradicting, "vacuous": r.vacuous,
         "support": r.support, "status": r.status,
         "first_counterexample": r.first_counterexample} for r in results]}
    return json.dumps(out, indent=2)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="mine invariants from Relais journals")
    ap.add_argument("paths", nargs="*", help="journal files or directories")
    ap.add_argument("--min-confidence", type=float, default=0.0)
    ap.add_argument("--json", action="store_true", help="emit JSON instead of text")
    args = ap.parse_args(argv)
    if not args.paths:
        ap.error("at least one journal path or directory is required")
    journals = load_corpus(args.paths)
    if not journals:
        print("no journals found", file=sys.stderr)
        return 0
    results = mine(journals)
    if args.json:
        print(to_json(results, len(journals)))
    else:
        print(format_report(results, len(journals), args.min_confidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
