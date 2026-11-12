"""Timeline of the three processes (idea 43, OBS-03 / OBS-06).

Draws an SVG frise aligned on ticks showing, for each actor -- the actuator,
machine A, machine B and the three links -- what happened on every tick:
STATUT reception, the source of the applied value (ORDRE / OMBRE / testament /
fallback), the lamp, delivery and rejection. Source of truth is the actuator
journal; the per-node status logs (host/node --verbose, one line per tick) are
merged in when supplied, to mark order/shadow emission and the role each
machine believes it holds.

Standalone SVG, no external dependency (stdlib only), readable when projected.

Usage:
    python3 host/observability-timeline/timeline.py --journal /tmp/x.jsonl \
        [--twin /tmp/x.twin.jsonl] [--node A=/path/a.log --node B=/path/b.log] \
        [--from 480 --to 540] [--out /tmp/timeline.svg]
"""
from __future__ import annotations

import argparse
import os
import sys
from typing import Dict, List, Optional, Sequence, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
for _inc in (os.path.join(_HERE, "..", "report"), os.path.join(_HERE, "..", "edr")):
    if _inc not in sys.path:
        sys.path.insert(0, _inc)

from journal_io import Journal, read_journal  # noqa: E402

SRC_COLOR = {"holder": "#2e7d32", "shadow": "#1565c0", "testament": "#ef6c00",
             "fallback": "#c62828", "none": "#9e9e9e"}
LAMP_COLOR = {"off": "#f5f5f5", "yellow": "#fff59d", "red": "#ef9a9a"}
COLW = 9
MARGIN_L = 132
MARGIN_T = 54
ROW_H = 30
DEFAULT_WINDOW = 60


def _handover_ticks(ticks) -> Dict[int, Tuple[str, str]]:
    """Map detection tick -> (old holder, new holder) for vertical markers."""
    out: Dict[int, Tuple[str, str]] = {}
    prev = None
    for t in ticks:
        if prev is not None and t.holder != prev.holder and t.holder:
            out[t.tick] = (prev.holder, t.holder)
        prev = t
    return out


def _parse_node_log(path: str) -> List[Dict[str, str]]:
    """Parse 'node X [tag] k=v ...' lines into a list of dicts (one per tick)."""
    rows: List[Dict[str, str]] = []
    try:
        f = open(path, "r", encoding="utf-8", errors="replace")
    except OSError:
        return rows
    with f:
        for line in f:
            if not line.startswith("node "):
                continue
            d: Dict[str, str] = {}
            for tok in line.split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    d[k] = v
            rows.append(d)
    return rows


def _emission_marks(rows: List[Dict[str, str]], counter: str) -> List[bool]:
    """Per node-log line: did `counter` increase since the previous line?"""
    marks: List[bool] = []
    prev = 0
    for d in rows:
        cur = int(d.get(counter, "0") or 0)
        marks.append(cur > prev)
        prev = cur
    return marks


def _window(ticks, args) -> Tuple[int, int]:
    lo, hi = ticks[0].tick, ticks[-1].tick
    if args.tick_from is not None:
        lo = args.tick_from
    if args.tick_to is not None:
        hi = args.tick_to
    span = hi - lo + 1
    if span > DEFAULT_WINDOW and args.tick_from is None:
        hi = lo + DEFAULT_WINDOW - 1
    return lo, hi


def _svg_header(width: int, height: int) -> List[str]:
    return [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d" font-family="monospace" font-size="11">' % (width, height, width, height),
        '<rect width="%d" height="%d" fill="white"/>' % (width, height),
    ]


def _legend(x: int, y: int) -> List[str]:
    items = [("ORDRE", SRC_COLOR["holder"]), ("OMBRE", SRC_COLOR["shadow"]),
             ("testament", SRC_COLOR["testament"]), ("secours", SRC_COLOR["fallback"])]
    out = ['<text x="%d" y="%d" font-weight="bold">source appliquee :</text>' % (x, y)]
    cx = x + 150
    for label, color in items:
        out.append('<rect x="%d" y="%d" width="12" height="12" fill="%s"/>' % (cx, y - 10, color))
        out.append('<text x="%d" y="%d">%s</text>' % (cx + 16, y, label))
        cx += 16 + 9 * len(label)
    return out


def _row_label(y: int, text: str) -> str:
    return '<text x="6" y="%d" font-weight="bold">%s</text>' % (y + 4, text)


def _actuator_rows(ticks, lo, hi, handovers) -> Tuple[List[str], int]:
    out: List[str] = []
    y_lamp = MARGIN_T
    y_src = MARGIN_T + ROW_H
    out.append(_row_label(y_lamp, "temoin"))
    out.append(_row_label(y_src, "actionneur"))
    for t in ticks:
        if t.tick < lo or t.tick > hi:
            continue
        x = MARGIN_L + (t.tick - lo) * COLW
        out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="%s" stroke="#ddd"/>'
                   % (x, y_lamp, COLW, ROW_H, LAMP_COLOR.get(t.lamp, "#fff")))
        out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="%s" stroke="#ccc"/>'
                   % (x, y_src, COLW, ROW_H, SRC_COLOR.get(t.source, "#fff")))
    for tick, (old, new) in handovers.items():
        if tick < lo or tick > hi:
            continue
        x = MARGIN_L + (tick - lo) * COLW + COLW / 2
        out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#000" '
                   'stroke-dasharray="3,2" stroke-width="1"/>'
                   % (x, MARGIN_T - 18, x, y_src + ROW_H + 4))
        out.append('<text x="%d" y="%d" font-size="9">%s&gt;%s</text>'
                   % (x + 2, MARGIN_T - 20, old, new))
    return out, y_src + ROW_H


def _machine_row(ticks, lo, hi, name: str, y: int, node_rows: Optional[List[Dict[str, str]]]) -> List[str]:
    out = [_row_label(y, "machine " + name)]
    order_marks = _emission_marks(node_rows, "orders") if node_rows else []
    first_tick = ticks[0].tick if ticks else 1
    for t in ticks:
        if t.tick < lo or t.tick > hi:
            continue
        x = MARGIN_L + (t.tick - lo) * COLW
        cx = x + COLW / 2
        cy = y + ROW_H / 2
        if t.holder == name:
            out.append('<circle cx="%d" cy="%d" r="4" fill="#2e7d32"/>' % (cx, cy))
        if t.certified_node == name:
            out.append('<circle cx="%d" cy="%d" r="6" fill="none" stroke="#1565c0"/>' % (cx, cy))
        idx = t.tick - first_tick
        if 0 <= idx < len(order_marks) and order_marks[idx]:
            out.append('<rect x="%d" y="%d" width="%d" height="4" fill="#000"/>' % (x, y + 2, COLW))
    return out


def _link_rows(ticks, events, lo, hi, y0: int) -> Tuple[List[str], int]:
    links = [("lien A-actionneur", ("A",)), ("lien B-actionneur", ("B",)),
             ("lien A-B", ("A", "B"))]
    out: List[str] = []
    y = y0
    ev_by_tick: Dict[int, List] = {}
    for e in events:
        ev_by_tick.setdefault(e.tick, []).append(e)
    for label, nodes in links:
        out.append(_row_label(y, label))
        for t in ticks:
            if t.tick < lo or t.tick > hi:
                continue
            x = MARGIN_L + (t.tick - lo) * COLW
            for e in ev_by_tick.get(t.tick, []):
                if e.node in nodes or (label.startswith("lien A-B") and e.event in ("disagreement", "order_rejected")):
                    color = "#c62828" if e.event in ("order_rejected", "disagreement", "request_denied") else "#1565c0"
                    out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="%s" opacity="0.7"/>'
                               % (x, y + 4, COLW, ROW_H - 8, color))
        y += ROW_H
    return out, y


def build_svg(act: Journal, nodes: Dict[str, List[Dict[str, str]]], lo: int, hi: int) -> str:
    ticks = act.ticks
    events = act.events
    handovers = _handover_ticks(ticks)
    ncols = hi - lo + 1
    width = MARGIN_L + ncols * COLW + 20
    n_rows = 2 + 2 + 3  # lamp, actuator, A, B, three links
    height = MARGIN_T + n_rows * ROW_H + 40
    parts = _svg_header(width, height)
    parts.append('<text x="6" y="20" font-size="14" font-weight="bold">'
                 'Relais - timeline des trois processus (ticks %d..%d)</text>' % (lo, hi))
    parts += _legend(6, 38)
    y = MARGIN_T
    rows, y = _actuator_rows(ticks, lo, hi, handovers)
    parts += rows
    parts += _machine_row(ticks, lo, hi, "A", y, nodes.get("A"))
    y += ROW_H
    parts += _machine_row(ticks, lo, hi, "B", y, nodes.get("B"))
    y += ROW_H
    lrows, y = _link_rows(ticks, events, lo, hi, y)
    parts += lrows
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Relais timeline (SVG, idea 43)")
    ap.add_argument("--journal", required=True)
    ap.add_argument("--node", action="append", default=[], help="NAME=path, repeatable")
    ap.add_argument("--from", dest="tick_from", type=int, default=None)
    ap.add_argument("--to", dest="tick_to", type=int, default=None)
    ap.add_argument("--out", default=None, help="output .svg path")
    args = ap.parse_args(argv)

    act = read_journal(args.journal)
    if not act.ticks:
        print("timeline: journal sans tick")
        return 1
    nodes: Dict[str, List[Dict[str, str]]] = {}
    for spec in args.node:
        if "=" in spec:
            name, _, path = spec.partition("=")
            nodes[name.strip().upper()] = _parse_node_log(path)
    lo, hi = _window(act.ticks, args)
    svg = build_svg(act, nodes, lo, hi)
    out = args.out or (os.path.splitext(args.journal)[0] + ".timeline.svg")
    with open(out, "w", encoding="utf-8") as f:
        f.write(svg)
    print("timeline: ticks %d..%d -> %s" % (lo, hi, out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())