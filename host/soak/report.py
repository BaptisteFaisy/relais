#!/usr/bin/env python3
"""Relais -- the reliability curve of the long soak (mission M42).

Standalone SVG, written by hand: no plotting library, no JavaScript, no
external reference. The panels reuse the chart helpers of host/report/charts.py
so that the soak report and the campaign report share one style and one
implementation (OBS-05).

The report publishes what the mission asks for: the cumulative time without an
anomaly, the number of ticks, the number of handovers, and the distribution of
the run latencies, slice by slice.
"""
from __future__ import annotations

import json
import os
import sys
from typing import Any, Dict, List, Optional, Sequence, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
_SHARED = os.path.join(_ROOT, "host", "report")
for _p in (_HERE, _SHARED):
    if _p not in sys.path:
        sys.path.append(_p)

import charts  # noqa: E402  (repo-local modules, path set above)
import stats   # noqa: E402

SLICES = 6
WIDTH = 920
INK = "#212121"
MUTED = "#616161"
ALARM = "#c62828"


def _place(svg: str, x: int, y: int) -> str:
    """Move a standalone chart into a panel of the composed figure."""
    return svg.replace("<svg ", '<svg x="%d" y="%d" ' % (x, y), 1)


def _text(x: int, y: int, body: str, size: int = 12,
          color: str = INK, weight: str = "normal") -> str:
    """One line of text of the composed figure."""
    return ('<text x="%d" y="%d" font-size="%d" fill="%s" font-weight="%s">%s</text>'
            % (x, y, size, color, weight, charts.esc(body)))


def _header(summary: Dict[str, Any]) -> Tuple[str, int]:
    """The counters of the campaign; returns the markup and its height."""
    latency = summary.get("latency", {})
    rss = summary.get("rss", {})
    lines = [
        "Relais long soak -- mission M42, rulebook %s" % summary.get("rules_version", "?"),
        "status: %s   duration: %.1f min (requested %.0f, effective %.1f)"
        % (summary.get("status", "?"), summary.get("duration_s", 0.0) / 60.0,
           summary.get("requested_minutes", 0.0), summary.get("effective_minutes", 0.0)),
        "runs: %d   ticks: %s   handovers: %d   mandates: %d   anomalies: %d"
        % (summary.get("runs", 0), "{:,}".format(summary.get("ticks_total", 0)),
           summary.get("handovers_total", 0), summary.get("mandates_total", 0),
           summary.get("anomaly_count", 0)),
        "latency (%s): reference p99.9 %.0f, threshold %.0f, samples %d"
        % (latency.get("metric", "cpu us per 1000 ticks"),
           latency.get("reference_p999", 0.0), latency.get("threshold", 0.0),
           latency.get("samples", 0)),
        "peak RSS: %d..%d kB over %d runs (window %d)"
        % (rss.get("min_kb", 0), rss.get("max_kb", 0), summary.get("runs", 0),
           rss.get("window", 0)),
        "machine guard: %s" % summary.get("machine_guard", ""),
    ]
    out = [_text(14, 22 + 18 * i, line, 12 if i else 15,
                 INK if i else INK, "normal" if i else "bold")
           for i, line in enumerate(lines)]
    height = 26 + 18 * len(lines)
    for i, item in enumerate(summary.get("anomalies", [])[:3]):
        out.append(_text(14, height + 4 + 16 * i,
                         "ANOMALY %s: %s" % (item.get("rule", "?"), item.get("title", "")),
                         12, ALARM, "bold"))
        height += 16
    return "".join(out), height + 10


def _elapsed_minutes(records: Sequence[Dict[str, Any]]) -> List[int]:
    """Wall-clock minutes of each run, from the first finished run."""
    if not records:
        return []
    origin = float(records[0].get("ts", 0.0))
    return [int(round((float(r.get("ts", origin)) - origin) / 60.0)) for r in records]


def reliability_panel(records: Sequence[Dict[str, Any]], width: int,
                      height: int) -> str:
    """Cumulative ticks played without an anomaly, against elapsed minutes."""
    minutes = _elapsed_minutes(records)
    if not minutes:
        return charts.series_svg([0], [0.0], width=width, height=height,
                                 title="Reliability: no run yet")
    # One point per minute: the curve then follows the wall clock and not the
    # rank of the runs, which are not evenly spread in time.
    last_minute = max(minutes)
    totals: List[float] = []
    running = 0
    cursor = 0
    for minute in range(last_minute + 1):
        while cursor < len(records) and minutes[cursor] <= minute:
            running += int(records[cursor].get("ticks", 0))
            cursor += 1
        totals.append(running / 1e6)
    marks = list(range(last_minute + 1))
    return charts.series_svg(marks, totals, width=width, height=height,
                             title="Reliability curve: cumulative ticks without anomaly",
                             run_name="cumulative ticks (millions)")


def _slices(records: Sequence[Dict[str, Any]],
            minutes: Sequence[int]) -> List[List[float]]:
    """Group the latency samples of the runs into SLICES time buckets."""
    buckets: List[List[float]] = [[] for _ in range(SLICES)]
    if not records:
        return buckets
    span = max(1, minutes[-1])
    for record, minute in zip(records, minutes):
        idx = min(SLICES - 1, int(minute * SLICES / (span + 1)))
        buckets[idx].append(float(record.get("cpu_us_per_ktick", 0.0)))
    return buckets


def latency_panel(records: Sequence[Dict[str, Any]], summary: Dict[str, Any],
                  width: int, height: int) -> str:
    """Latency per time slice: the p99.9 against the drift threshold."""
    minutes = _elapsed_minutes(records)
    buckets = _slices(records, minutes)
    worst = [stats.percentile(b, 99.9) for b in buckets]
    typical = [stats.percentile(b, 50.0) for b in buckets]
    threshold = float(summary.get("latency", {}).get("threshold", 0.0))
    span = max(1, minutes[-1]) if minutes else 1
    marks = [int(round(span * (i + 0.5) / SLICES)) for i in range(SLICES)]
    line = [threshold] * SLICES if threshold > 0 else None
    title = ("Latency per slice: p99.9 %s, p50 %s cpu us per 1000 ticks"
             % ("%.0f" % max(worst, default=0.0), "%.0f" % max(typical, default=0.0)))
    return charts.series_svg(marks, worst, twin=line, width=width, height=height,
                             title=title, run_name="p99.9 per slice",
                             twin_name="A09 threshold")


def histogram_panel(records: Sequence[Dict[str, Any]], width: int,
                    height: int) -> str:
    """Distribution of the run latencies over the whole campaign."""
    values = [float(r.get("cpu_us_per_ktick", 0.0)) for r in records]
    if not values:
        return charts.histogram_svg([], width=width, height=height,
                                    title="Latency distribution: no run yet")
    span = max(values) - min(values)
    step = span / 12.0 if span > 0 else 1.0
    bins = stats.histogram_from_values(values, step, min(values))
    return charts.histogram_svg(bins, width=width, height=height,
                                title="Latency distribution over %d runs" % len(values),
                                x_label="cpu microseconds per 1000 ticks")


def build_svg(summary: Dict[str, Any],
              records: Sequence[Dict[str, Any]]) -> str:
    """Compose the whole figure: counters, reliability curve, latency panels."""
    header, head_h = _header(summary)
    panel_h = 250
    hist_h = 220
    height = head_h + panel_h * 2 + hist_h + 20
    body = [header]
    body.append(_place(reliability_panel(records, WIDTH - 20, panel_h),
                       10, head_h))
    body.append(_place(latency_panel(records, summary, WIDTH - 20, panel_h),
                       10, head_h + panel_h))
    body.append(_place(histogram_panel(records, WIDTH - 20, hist_h),
                       10, head_h + panel_h * 2))
    desc = ("Reliability of the Relais protocol over %d runs, %d ticks, %d "
            "handovers, %d anomalies."
            % (summary.get("runs", 0), summary.get("ticks_total", 0),
               summary.get("handovers_total", 0), summary.get("anomaly_count", 0)))
    return ('<svg xmlns="%s" width="%d" height="%d" viewBox="0 0 %d %d" role="img" '
            'font-family="sans-serif" font-size="12"><title>Relais long soak '
            'reliability</title><desc>%s</desc><rect width="%d" height="%d" '
            'fill="#ffffff"/>%s</svg>'
            % (charts.XMLNS, WIDTH, height, WIDTH, height, charts.esc(desc),
               WIDTH, height, "".join(body)))


def load_folder(folder: str) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    """Read the summary and the digest-chained log of one campaign."""
    with open(os.path.join(folder, "summary.json"), "r", encoding="utf-8") as fh:
        summary = json.load(fh)
    records: List[Dict[str, Any]] = []
    log = os.path.join(folder, summary.get("log", "soak.jsonl"))
    if os.path.isfile(log):
        with open(log, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line:
                    records.append(json.loads(line))
    return summary, records


def main(argv: Optional[Sequence[str]] = None) -> int:
    """Rebuild the reliability curve of a finished (or frozen) campaign."""
    folder = argv[1] if argv and len(argv) > 1 else "results/soak"
    summary, records = load_folder(folder)
    path = os.path.join(folder, "reliability.svg")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(build_svg(summary, records))
    print("report: %s (%d runs, %d anomalies)"
          % (path, len(records), summary.get("anomaly_count", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
