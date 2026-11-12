"""Margin alerts (idea 44) — turn the binary bench into an instrument.

Instead of waiting for a bound to be crossed, compute the REMAINING MARGIN on
each observable and alert as soon as the margin drops under a threshold
(default 50 %). Three margins, mirroring the documented bounds:

  * fresh-order gap  : observed longest run without a fresh holder order,
                       over the allowed gap (p+2 ticks at level S);
  * deviation        : observed max |value - twin value|, over delta;
  * reaction latency : observed max FRTI (detection -> first order of the new
                       holder), over the allowed reaction (p+2 ticks).

ratio = observed / bound ; margin = 1 - ratio. A ratio above 1 is a violation
(the bound is crossed), a margin under the threshold is an early alert.

Reuses host/report/journal_io.py and host/edr/edr.py (one implementation of the
handover timing, zero duplication).

Usage:
    python3 host/observability-timeline/margins.py --journal /tmp/x.jsonl \
        --twin /tmp/x.twin.jsonl [--threshold 0.5] [--out /tmp/margins.csv]
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
for _inc in (os.path.join(_HERE, "..", "report"), os.path.join(_HERE, "..", "edr")):
    if _inc not in sys.path:
        sys.path.insert(0, _inc)

from journal_io import Journal, read_journal, FRESH_SOURCES  # noqa: E402
from edr import build_records  # noqa: E402


@dataclass
class Margin:
    """One observable against its documented bound."""
    name: str
    observed: float
    bound: float
    unit: str = "ticks"

    @property
    def ratio(self) -> float:
        if self.bound > 0:
            return self.observed / self.bound
        return float("inf") if self.observed > 0 else 0.0

    @property
    def margin(self) -> float:
        return 1.0 - self.ratio

    def status(self, threshold: float) -> str:
        # "DEPASSE" = the bound is crossed; relais-verify stays the authority on
        # safety violations (I1..I11). These margins are an early-warning tool.
        if self.ratio > 1.0:
            return "DEPASSE"
        if self.margin < threshold:
            return "ALERTE"
        return "OK"


def _is_complete(ticks) -> bool:
    """Infer the level from the journal: a certified second or a shadow tick."""
    return any(t.certified or t.source == "shadow" for t in ticks)


def max_fresh_gap(ticks) -> int:
    """Longest run of ticks with no fresh value (fresh = holder or shadow, I4).

    Mirrors check_gap in core/src/invariants.c: ticks before the first fresh
    order (start-up, while the mandate is still being armed) are not counted.
    """
    best = run = 0
    started = False
    for t in ticks:
        if t.source in FRESH_SOURCES:
            started = True
            run = 0
        elif started:
            run += 1
            best = max(best, run)
    return best


def max_deviation(act: Journal, twin: Optional[Journal]) -> float:
    """Max |value - twin value|, measured like I5 (fallback ticks excluded)."""
    if twin is None:
        return 0.0
    twin_val = {t.tick: t.value for t in twin.ticks}
    best = 0.0
    for t in act.ticks:
        if t.source == "fallback" or t.tick not in twin_val:
            continue
        best = max(best, abs(t.value - twin_val[t.tick]))
    return best


def max_frti(act: Journal) -> int:
    """Max fault-reaction interval over every handover (from the EDR)."""
    records = build_records(act, None, 5.0, None)
    return max((r.frti_ticks for r in records), default=0)


def compute_margins(act: Journal, twin: Optional[Journal]) -> List[Margin]:
    """Build the margin set for one run from the journal header parameters."""
    params = act.header.params if act.header else {}
    p = int(params.get("p", 1))
    delta = float(params.get("delta", 0.02))
    complete = _is_complete(act.ticks)
    gap_bound = 0.0 if complete else float(p + 1)  # I4: 0 at level C, p+1 at S
    frti_bound = float(p + 2)  # SAFETY.md: reaction allowed p+2 ticks at level S
    return [
        Margin("fresh-order gap", float(max_fresh_gap(act.ticks)), gap_bound, "ticks"),
        Margin("deviation to twin", max_deviation(act, twin), delta, "value"),
        Margin("reaction latency (FRTI)", float(max_frti(act)), frti_bound, "ticks"),
    ]


def format_table(margins: List[Margin], threshold: float) -> str:
    lines = ["%-26s %10s %10s %8s %8s  %s" %
             ("observable", "observed", "bound", "ratio", "margin", "status")]
    lines.append("-" * 78)
    for m in margins:
        lines.append("%-26s %10.4g %10.4g %7.0f%% %7.0f%%  %s" %
                     (m.name, m.observed, m.bound, m.ratio * 100, m.margin * 100,
                      m.status(threshold)))
    return "\n".join(lines) + "\n"


def write_csv(path: str, margins: List[Margin], threshold: float) -> int:
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["observable", "observed", "bound", "unit", "ratio", "margin", "status"])
        for m in margins:
            w.writerow([m.name, "%.6g" % m.observed, "%.6g" % m.bound, m.unit,
                        "%.4f" % m.ratio, "%.4f" % m.margin, m.status(threshold)])
    return len(margins)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Relais margin alerts (idea 44)")
    ap.add_argument("--journal", required=True)
    ap.add_argument("--twin", default=None)
    ap.add_argument("--threshold", type=float, default=0.5, help="alert under this margin")
    ap.add_argument("--out", default=None, help="optional CSV output path")
    args = ap.parse_args(argv)

    act = read_journal(args.journal)
    twin = read_journal(args.twin) if args.twin and os.path.isfile(args.twin) else None
    margins = compute_margins(act, twin)
    sys.stdout.write(format_table(margins, args.threshold))
    alerts = [m for m in margins if m.status(args.threshold) != "OK"]
    if alerts:
        print("alertes (%d) :" % len(alerts))
        for m in alerts:
            print("  - %s : marge %.0f%% (observed %.4g / bound %.4g) -> %s" %
                  (m.name, m.margin * 100, m.observed, m.bound, m.status(args.threshold)))
    else:
        print("alertes : aucune (toutes les marges >= %.0f%%)" % (args.threshold * 100))
    print("note: relais-verify reste l'autorite sur les violations de surete (I1..I11);"
          " ces marges sont un instrument d'alerte precoce.")
    if args.out:
        write_csv(args.out, margins, args.threshold)
        print("margins -> %s" % args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())