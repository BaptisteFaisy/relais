#!/usr/bin/env python3
"""Relais — campaign HTML report generator (OBS-05, spec 7.7/7.8/13).

Reads a campaign tree of run folders (actuator.jsonl + twin.jsonl + optional
scenario.json) and produces ONE self-contained HTML file: inline CSS, inline
SVG, no script, no external resource. English throughout, standard
library only, no external resource in the output.

Usage: python3 host/report/report.py --campaign runs/ --out report.html
           [--level socle|complet] [--title "..."] [--mutants m.json]
Exit code 0 when the report was written (whatever the verdict).
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import sys
import time
from typing import Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import charts  # noqa: E402
import stats  # noqa: E402
from analysis import RunAnalysis, analyze_campaign  # noqa: E402
from journal_io import Run, discover_runs  # noqa: E402

esc = charts.esc  # single escape implementation, shared with the charts

CSS = """
body{font-family:'Segoe UI',Arial,sans-serif;margin:0;background:#f5f6f8;color:#212121}
main{max-width:980px;margin:0 auto;padding:16px 22px 40px}
h1{font-size:26px;margin:12px 0 2px}h2{font-size:19px;margin:30px 0 8px;
border-bottom:2px solid #cfd8dc;padding-bottom:4px}
h3{font-size:15px;margin:18px 0 6px}
table{border-collapse:collapse;width:100%;background:#fff;margin:8px 0}
th,td{border:1px solid #cfd8dc;padding:5px 8px;text-align:left;font-size:13px}
th{background:#eceff1}td.num{text-align:right;font-variant-numeric:tabular-nums}
.ok{color:#1b5e20;font-weight:bold}.ko{color:#b71c1c;font-weight:bold}
.na{color:#616161}
.banner{border-radius:8px;padding:14px 18px;margin:14px 0;font-size:17px;font-weight:bold}
.banner.ok{background:#e8f5e9;border:2px solid #2e7d32;color:#1b5e20}
.banner.ko{background:#ffebee;border:2px solid #c62828;color:#b71c1c}
.meta{color:#455a64;font-size:13px}
.card{background:#fff;border:1px solid #cfd8dc;border-radius:8px;padding:10px 14px;margin:12px 0}
.comment{font-size:13px;line-height:1.45;background:#fafafa;border-left:4px solid #90a4ae;
padding:8px 10px;margin:8px 0}
svg{max-width:100%;height:auto}
ul{font-size:13px;line-height:1.5}
footer{margin-top:34px;font-size:12px;color:#607d8b}
"""

FAULT_LABELS: Dict[str, str] = {
    "death": "Mort", "freeze": "Gel", "cut": "Coupure de lien",
    "slow": "Machine lente", "double": "Double panne", "nominal": "Nominal",
    "other": "Autre panne",
}
FAULT_ORDER = ("death", "freeze", "cut", "slow", "double", "nominal", "other")
MUTANT_IDS = tuple(range(1, 9))


def _inline(svg: str) -> str:
    """Embed a standalone SVG into HTML5: the xmlns URL is dropped so that the
    produced file contains no http reference at all (offline, self-contained)."""
    return svg.replace(' xmlns="%s"' % charts.XMLNS, "", 1)


def _status(ok: Optional[bool]) -> str:
    """One status cell: OK / FAIL / not measured."""
    if ok is None:
        return '<td class="na">not measured</td>'
    return ('<td class="ok">OK</td>' if ok else '<td class="ko">FAIL</td>')


def _fmt_ms(ms: float) -> str:
    """Human milliseconds."""
    return "%.1f ms" % ms


def load_mutants(path: Optional[str]) -> Optional[List[Dict]]:
    """Load a mutant bench result file; None when absent or unreadable.
    Accepts {"mutants":[...]} or a bare list of {id,verdict,defect,expected,
    detail} records."""
    if not path:
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return None
    rows = data.get("mutants") if isinstance(data, dict) else data
    if not isinstance(rows, list):
        return None
    out: List[Dict] = []
    for r in rows:
        if isinstance(r, dict):
            out.append(r)
    return out


def _totals(analyses: Sequence[RunAnalysis]) -> Dict[str, float]:
    """Campaign-wide totals for the header and the verdict."""
    ticks = sum(a.ticks_total for a in analyses)
    period = analyses[0].period_ms if analyses else 5.0
    return {
        "runs": len(analyses), "ticks": ticks,
        "seconds": ticks * period / 1000.0,
        "violations": sum(a.violation_count for a in analyses),
        "bad_runs": sum(1 for a in analyses if not a.ok),
        "malformed": sum(a.malformed for a in analyses),
    }


def verdict_section(analyses: Sequence[RunAnalysis], tot: Dict[str, float],
                    level: str) -> str:
    """The global verdict banner plus the acceptance criteria table."""
    ok = tot["violations"] == 0 and tot["bad_runs"] == 0
    banner = ('<div class="banner ok">GLOBAL VERDICT: ZERO VIOLATION — %d runs, the '
              'handover bound holds everywhere (level %s).</div>'
              % (tot["runs"], level)) if ok else (
              '<div class="banner ko">GLOBAL VERDICT: FAIL — %d invariant violation(s) '
              'over %d faulty run(s) (level %s).</div>'
              % (tot["violations"], tot["bad_runs"], level))
    rows = "".join(
        "<tr><td>%s</td><td>%s</td>%s</tr>" % (esc(c), esc(m), _status(s))
        for c, m, s in acceptance_rows(analyses, tot, level))
    table = ("<table><tr><th>Acceptance criterion (spec §13)</th><th>Measurement</th>"
             "<th>Statut</th></tr>%s</table>" % rows)
    return banner + table


def _scale_row(analyses: Sequence[RunAnalysis], level: str) -> Tuple[str, str, Optional[bool]]:
    """One acceptance row: campaign scale against spec 7.8 thresholds."""
    need = {"death": (200, 2000), "freeze": (2000, 10000), "cut": (500, 5000)}
    counts = {k: sum(1 for a in analyses if a.run.fault_class == k) for k in need}
    idx = 1 if level == "complet" else 0
    parts = []
    ok = True
    for key, label in (("death", "deaths"), ("freeze", "freezes"), ("cut", "cuts")):
        req = need[key][idx]
        parts.append("%s %d/%d" % (label, counts[key], req))
        ok = ok and counts[key] >= req
    return ("Campaign scale (spec 7.8): real %s" % label,
            ", ".join(parts), ok)


def acceptance_rows(analyses: Sequence[RunAnalysis], tot: Dict[str, float],
                    level: str, mutants: Optional[List[Dict]] = None
                    ) -> List[Tuple[str, str, Optional[bool]]]:
    """Acceptance criteria of spec §13 that this campaign can measure."""
    rows: List[Tuple[str, str, Optional[bool]]] = []
    rows.append(("Zero invariant violation tolerated",
                 "%d violation(s), %d malformed line(s)"
                 % (tot["violations"], tot["malformed"]),
                 tot["violations"] == 0))
    bound = ("0 ticks without a fresh order (certified second)" if level == "complet"
             else "p+2 ticks between two fresh orders")
    worst = max((a.max_gap for a in analyses), default=0)
    rows.append(("Handover after a death: %s" % bound,
                 "worst measured gap: %d tick(s)" % worst,
                 tot["bad_runs"] == 0))
    delta = analyses[0].run.journal.header.params.get("delta", 0.02) if (
        analyses and analyses[0].run.journal.header) else 0.02
    dev = max((a.max_deviation for a in analyses), default=0.0)
    rows.append(("Deviation from the twin without fault ≤ delta (%.3g)" % delta,
                 "worst measured deviation: %.4g" % dev,
                 dev <= delta if level == "complet" else None))
    rows.append(_scale_row(analyses, level))
    need_ids = MUTANT_IDS if level == "complet" else MUTANT_IDS[:4]
    if mutants is None:
        rows.append(("Mutants M1–M%d flagged" % need_ids[-1],
                     "no mutant bench result supplied (--mutants)", None))
    else:
        flagged = {int(m.get("id", 0)) for m in mutants
                   if str(m.get("verdict", "")).upper() == "FLAGGED"}
        missing = [i for i in need_ids if i not in flagged]
        rows.append(("Mutants M1–M%d flagged" % need_ids[-1],
                     "%d/%d flagged%s" % (len(need_ids) - len(missing),
                                           len(need_ids),
                                           "" if not missing else " (missing M%d)" % missing[0]),
                     not missing))
    return rows


def _worst_cases(analyses: Sequence[RunAnalysis], count: int = 3) -> List[RunAnalysis]:
    """The runs that came closest to the bounds: the ones worth a chart."""
    ranked = sorted(analyses, key=lambda a: (a.max_gap, a.max_deviation), reverse=True)
    return [a for a in ranked if a.ticks_total > 0][:count]


def _gap_chart(a: RunAnalysis) -> str:
    """Histogram of the gaps between two fresh orders, when there is any."""
    if not a.gaps:
        return ""
    bins = stats.histogram_from_values([float(g) for g in a.gaps], 6)
    if not bins:
        return ""
    return _inline(charts.histogram_svg(
        bins, title="Ticks between two fresh orders", x_label="ticks"))


def _case_section(a: RunAnalysis) -> str:
    """One worst case: its curves, its sources, its mandates, and what happened."""
    ticks = [t.tick for t in a.run.journal.ticks]
    values = [t.value for t in a.run.journal.ticks]
    twin = None
    if a.run.twin is not None:
        by_tick = {t.tick: t.value for t in a.run.twin.ticks}
        twin = [by_tick.get(k) for k in ticks]
    sources = [t.source for t in a.run.journal.ticks]
    holders = [t.holder or "-" for t in a.run.journal.ticks]
    mandates = [t.mandate for t in a.run.journal.ticks]
    events = [(e.tick, e.event) for e in a.run.journal.events
              if "mandate" in e.event or e.event in ("cession", "fallback_enter")]
    handovers = ", ".join("tick %d → %s (mandate %d, %d tick(s))"
                          % (h.tick, h.node, h.mandate, h.delay_ticks)
                          for h in a.handovers[:4]) or "none"
    comment = ("Worst gap %d tick(s) at tick %d; worst deviation from the twin "
               "%.6g; command served by: %s. Handovers: %s."
               % (a.max_gap, a.max_gap_tick, a.max_deviation,
                  ", ".join("%s=%d" % kv for kv in sorted(a.source_counts.items())),
                  handovers))
    return ("<section><h3>%s <span class='tag'>%s</span></h3>"
            "<p class='comment'>%s</p>%s%s%s%s</section>"
            % (esc(a.run.name), esc(a.run.fault_class), esc(comment),
               _inline(charts.series_svg(ticks, values, twin,
                                  title="Applied value vs the twin without fault")),
               _inline(charts.source_strip_svg(sources)),
               _inline(charts.mandate_strip_svg(holders, mandates, events)),
               _gap_chart(a)))


def _fault_table(analyses: Sequence[RunAnalysis], level: str) -> str:
    """One row per fault class: runs, violations, worst gap, bound, verdict."""
    bound = 0 if level == "complet" else 2
    classes: Dict[str, List[RunAnalysis]] = {}
    for a in analyses:
        classes.setdefault(a.run.fault_class, []).append(a)
    rows = []
    for name in sorted(classes):
        group = classes[name]
        worst = max(a.max_gap for a in group)
        bad = sum(len(a.violations) for a in group)
        rows.append("<tr><td>%s</td><td>%d</td><td>%d</td><td>%d</td><td>%s</td>%s</tr>"
                    % (esc(name), len(group), bad, worst,
                       ("0 (certified second)" if level == "complet" else "p+1 = %d" % bound),
                       _status(bad == 0 and worst <= bound)))
    return ("<table><tr><th>Fault class</th><th>Runs</th><th>Violations</th>"
            "<th>Worst gap (ticks)</th><th>Bound</th><th>Status</th></tr>%s</table>"
            % "".join(rows))


def render_html(analyses: Sequence[RunAnalysis], level: str, title: str,
                mutants: Optional[List[Dict]] = None) -> str:
    """One standalone HTML file: inline CSS, inline SVG, no external resource."""
    tot = _totals(analyses)
    parts = ["<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>",
             "<title>%s</title><style>%s</style></head><body>" % (esc(title), CSS),
             "<h1>%s</h1>" % esc(title),
             "<p class='meta'>Level %s &middot; %d run(s) &middot; %d ticks "
             "(%.1f s of emulated time) &middot; generated %s</p>"
             % (esc(level), tot["runs"], tot["ticks"], tot["seconds"],
                time.strftime("%Y-%m-%d %H:%M:%S")),
             verdict_section(analyses, tot, level),
             "<h2>Per fault class</h2>", _fault_table(analyses, level)]
    if mutants:
        rows = "".join("<tr><td>M%s</td><td>%s</td><td>%s</td>%s</tr>"
                       % (esc(str(m.get("id", "?"))), esc(str(m.get("defect", ""))),
                          esc(str(m.get("evidence", ""))),
                          _status(str(m.get("verdict", "")).upper() == "FLAGGED"))
                       for m in mutants)
        parts.append("<h2>Mutants (spec 7.6)</h2><table><tr><th>Mutant</th>"
                     "<th>Defect</th><th>Evidence</th><th>Flagged</th></tr>%s</table>"
                     % rows)
    parts.append("<h2>Worst cases</h2>")
    for a in _worst_cases(analyses):
        parts.append(_case_section(a))
    parts.append("<h2>Runs</h2><table><tr><th>Run</th><th>Class</th><th>Ticks</th>"
                 "<th>Violations</th><th>Max gap</th><th>Max deviation</th>"
                 "<th>Status</th></tr>")
    for a in analyses:
        parts.append("<tr><td>%s</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td>"
                     "<td>%.6g</td>%s</tr>"
                     % (esc(a.run.name), esc(a.run.fault_class), a.ticks_total,
                        len(a.violations), a.max_gap, a.max_deviation, _status(a.ok)))
    parts.append("</table>")
    parts.append("<p class='limits'>Emulated timings, measured worst cases — never "
                 "proven worst cases. Single-fault hypothesis: one fault at a time "
                 "until redundancy is back. The actuator is a declared single point "
                 "of failure; its arbitration core is tiny and exhaustively "
                 "explored. See docs/SAFETY.md §7 and docs/RESULTS.md.</p>")
    parts.append("</body></html>")
    return "".join(parts)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Relais campaign report (standalone HTML)")
    ap.add_argument("--campaign", required=True, help="directory holding the run folders")
    ap.add_argument("--out", default="report.html")
    ap.add_argument("--level", default="socle", choices=["socle", "complet"])
    ap.add_argument("--title", default="Relais — fault campaign report")
    ap.add_argument("--mutants", default="", help="JSON list from run_mutants.py")
    args = ap.parse_args(list(argv) if argv is not None else None)

    runs = discover_runs(args.campaign)
    if not runs:
        print("no run found under %s (a run is a folder holding actuator.jsonl)"
              % args.campaign)
        return 2
    analyses = analyze_campaign(runs, args.level)
    mutants = load_mutants(args.mutants) if args.mutants else None
    html = render_html(analyses, args.level, args.title, mutants)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(html)
    tot = _totals(analyses)
    print("report written: %s (%d runs, %d violations, %d bytes)"
          % (args.out, tot["runs"], tot["violations"], len(html)))
    return 0 if tot["violations"] == 0 and tot["bad_runs"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
