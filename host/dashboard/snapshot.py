#!/usr/bin/env python3
"""Static capture of the Relais dashboard (OBS-08).

Two artefacts, both standard library only:

* an SVG snapshot built from a sample journal — the command curve, the source
  strip and the mandate strip are reused verbatim from host/report/charts.py
  (single chart implementation), re-laid out on a dark, projector-friendly
  canvas;
* dashboard.html — the offline version of host/dashboard/index.html with
  style.css and app.js inlined and the journal embedded as
  window.RELAIS_REPLAY, so the page replays a recorded run tick by tick with a
  play button: usable on the video and by a jury without the repository.

Run it with:
    python3 host/dashboard/snapshot.py --journal runs/r/actuator.jsonl \
        --twin runs/r/twin.jsonl --svg _site/dashboard.svg --html _site/dashboard.html
"""
from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "report"))
from charts import (  # noqa: E402
    esc, mandate_strip_svg, series_svg, source_strip_svg,
)
from journal_io import read_journal, twin_values  # noqa: E402

XMLNS = "http://www.w3.org/2000/svg"


def _chart_body(svg):
    """Return the inner markup of a standalone chart <svg> element."""
    start = svg.find(">") + 1
    end = svg.rfind("</svg>")
    return svg[start:end]


def _nest(svg, x, y, width, height):
    """Re-lay a standalone chart into a nested <svg> frame."""
    return ('<svg x="%d" y="%d" width="%d" height="%d" viewBox="0 0 880 %d">%s</svg>'
            % (x, y, width, height, height, _chart_body(svg)))


def build_snapshot(journal, twin=None):
    """Render the dark SVG snapshot from a parsed Journal (and optional twin)."""
    ticks = [t.tick for t in journal.ticks]
    values = [t.value for t in journal.ticks]
    sources = [t.source for t in journal.ticks]
    holders = [t.holder for t in journal.ticks]
    mandates = [t.mandate for t in journal.ticks]
    events = [(e.tick, e.event) for e in journal.events]
    twin_by_tick = twin_values(twin) if twin is not None else {}
    twin_line = [twin_by_tick.get(t) for t in ticks] or None

    series = series_svg(ticks, values, twin=twin_line, width=880, height=250,
                        title="Applied command value", run_name="run",
                        twin_name="fault-free twin")
    strip = source_strip_svg(sources, first_tick=ticks[0] if ticks else 1,
                             width=880, height=74)
    band = mandate_strip_svg(holders, mandates, tuple(events),
                             first_tick=ticks[0] if ticks else 1,
                             width=880, height=78)

    last = journal.ticks[-1] if journal.ticks else None
    if last is None:
        head = "no ticks recorded"
    else:
        head = ("mandate %s · holder %s · source %s"
                % (last.mandate, last.holder or "-", last.source))
    counts = {s: sources.count(s) for s in set(sources)}
    mix = ", ".join("%s ×%d" % (k, counts[k]) for k in sorted(counts))

    total_h = 130 + 250 + 74 + 78 + 24
    shell = ['<svg xmlns="%s" width="920" height="%d" viewBox="0 0 920 %d" '
             'role="img" font-family="sans-serif">' % (XMLNS, total_h, total_h),
             '<rect width="920" height="%d" fill="#0b0f14"/>' % total_h,
             '<text x="24" y="46" font-size="30" font-weight="800" fill="#e7eef6">'
             'Relais — dashboard</text>',
             '<rect x="24" y="66" width="872" height="2" fill="#23303f"/>',
             '<text x="24" y="96" font-size="20" fill="#e7eef6">%s</text>'
             % esc(head),
             '<text x="24" y="120" font-size="15" fill="#8ba1b8">%d ticks · %s — '
             'replayed from a recorded journal</text>'
             % (len(ticks), esc(mix)),
             _nest(series, 20, 134, 880, 250),
             _nest(strip, 20, 384, 880, 74),
             _nest(band, 20, 458, 880, 78),
             '</svg>']
    return "".join(shell)


def build_html(journal, static_dir, max_records=4000):
    """Assemble the offline dashboard.html (inlined page + embedded journal)."""
    with open(os.path.join(static_dir, "index.html"), "r",
              encoding="utf-8") as fh:
        html = fh.read()
    with open(os.path.join(static_dir, "style.css"), "r",
              encoding="utf-8") as fh:
        css = fh.read()
    with open(os.path.join(static_dir, "app.js"), "r",
              encoding="utf-8") as fh:
        js = fh.read()
    html = html.replace('<link rel="stylesheet" href="style.css">',
                        "<style>%s</style>" % css)
    html = html.replace('<script src="app.js"></script>',
                        "<script>%s</script>" % js)
    replay = ["<script>window.RELAIS_REPLAY=%s;</script>"
              % json.dumps(journal.records[:max_records], separators=(",", ":"))]
    html = html.replace("</body>", "".join(replay) + "</body>")
    return html


def load_records(journal_path, max_records):
    """Read the raw JSON-lines journal as a tolerant list of dicts."""
    records = []
    with open(journal_path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict):
                records.append(obj)
                if len(records) >= max_records:
                    break
    return records


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Static SVG capture + offline replay page of the dashboard.")
    parser.add_argument("--journal", required=True)
    parser.add_argument("--twin", default=None)
    parser.add_argument("--svg", default=None)
    parser.add_argument("--html", default=None)
    parser.add_argument("--max-records", type=int, default=4000)
    args = parser.parse_args(argv)
    if not args.svg and not args.html:
        parser.error("one of --svg or --html is required")

    static_dir = os.path.dirname(os.path.abspath(__file__))
    journal = read_journal(args.journal)
    twin = read_journal(args.twin) if args.twin else None
    if args.svg:
        svg = build_snapshot(journal, twin)
        with open(args.svg, "w", encoding="utf-8") as fh:
            fh.write(svg + "\n")
        print("snapshot: %s (%d bytes)" % (args.svg, len(svg)))
    if args.html:
        journal.records = load_records(args.journal, args.max_records)
        html = build_html(journal, static_dir, args.max_records)
        with open(args.html, "w", encoding="utf-8") as fh:
            fh.write(html)
        print("offline page: %s (%d bytes)" % (args.html, len(html)))
    return 0


if __name__ == "__main__":
    sys.exit(main())