"""Inline SVG charts for the Relais campaign report (OBS-05).

No dependency, no JavaScript: every function returns a self-contained SVG
string (XML well-formed, English texts, <title> and <desc> for accessibility).
Sizes and scales are parameters. Colours are shared through SOURCE_COLORS so
the report never duplicates a style decision.
"""
from __future__ import annotations

from typing import Dict, List, Optional, Sequence, Tuple

XMLNS = "http://www.w3.org/2000/svg"
SOURCE_COLORS: Dict[str, str] = {
    "holder": "#2e7d32",     # green: a fresh order from the mandate holder
    "shadow": "#1565c0",     # blue: a fresh order from the certified second
    "testament": "#f9a825",  # amber: the will of the dead machine is playing
    "fallback": "#c62828",   # red: safe-state fallback law
    "none": "#9e9e9e",       # grey: nothing applied (must never happen)
}
HOLDER_COLORS: Dict[str, str] = {"A": "#6a1b9a", "B": "#00838f", "": "#9e9e9e"}
RUN_COLOR = "#1565c0"
TWIN_COLOR = "#ef6c00"
AXIS = "#424242"
GRID = "#e0e0e0"


def esc(text: object) -> str:
    """Escape the five XML/HTML special characters (single implementation)."""
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;").replace("'", "&#39;"))


def _wrap(width: int, height: int, title: str, desc: str, body: str) -> str:
    """Wrap chart body into a standalone accessible <svg> element."""
    return ('<svg xmlns="%s" width="%d" height="%d" viewBox="0 0 %d %d" '
            'role="img" font-family="sans-serif" font-size="11">'
            "<title>%s</title><desc>%s</desc>%s</svg>"
            % (XMLNS, width, height, width, height, esc(title), esc(desc), body))


def _legend(items: Sequence[Tuple[str, str]], x: int, y: int) -> str:
    """Horizontal colour legend: list of (color, english label)."""
    out: List[str] = []
    cx = x
    for color, label in items:
        out.append('<rect x="%d" y="%d" width="10" height="10" fill="%s"/>'
                   '<text x="%d" y="%d">%s</text>' % (cx, y - 9, color, cx + 14, y, esc(label)))
        cx += 22 + 7 * len(label)
    return "".join(out)


def _fmt(v: float) -> str:
    """Short numeric label for axes."""
    return ("%g" % v)


def _gridlines_y(y0: float, y1: float, x0: float, x1: float,
                 lo: float, hi: float, steps: int = 3) -> str:
    """Horizontal gridlines with value labels between lo and hi."""
    out: List[str] = []
    for i in range(steps + 1):
        v = lo + (hi - lo) * i / steps
        y = y1 - (y1 - y0) * i / steps
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s"/>'
                   '<text x="%.1f" y="%.1f" text-anchor="end" fill="%s">%s</text>'
                   % (x0, y, x1, y, GRID, x0 - 4, y + 3.5, AXIS, _fmt(v)))
    return "".join(out)


def histogram_svg(bins: Sequence[Dict[str, float]], width: int = 620,
                  height: int = 240, title: str = "Histogram",
                  x_label: str = "value", color: str = RUN_COLOR) -> str:
    """Bar chart of pre-binned data [{"lo","hi","count"}]; y starts at zero."""
    m_left, m_right, m_top, m_bottom = 52, 12, 30, 44
    x0, x1 = m_left, width - m_right
    y0, y1 = m_top, height - m_bottom
    hi_count = max((b["count"] for b in bins), default=0.0) or 1.0
    n = max(len(bins), 1)
    bw = (x1 - x0) / n
    body: List[str] = [_gridlines_y(y0, y1, x0, x1, 0.0, hi_count)]
    for i, b in enumerate(bins):
        h = (y1 - y0) * b["count"] / hi_count
        bx = x0 + i * bw
        body.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s">'
                    "<title>[%s, %s[: %s</title></rect>"
                    % (bx + 0.5, y1 - h, bw - 1, h, color, _fmt(b["lo"]),
                       _fmt(b["hi"]), _fmt(b["count"])))
    step_lbl = max(1, n // 8)
    for i in range(0, n, step_lbl):
        body.append('<text x="%.2f" y="%d" text-anchor="middle" fill="%s">%s</text>'
                    % (x0 + (i + 0.5) * bw, y1 + 14, AXIS, _fmt(bins[i]["lo"])))
    body.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"/>'
                '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%d" stroke="%s"/>'
                % (x0, y1, x1, y1, AXIS, x0, y0, x0, y1, AXIS))
    body.append('<text x="%d" y="%d" text-anchor="middle" fill="%s">%s</text>'
                % ((x0 + x1) // 2, height - 6, AXIS, esc(x_label)))
    body.append('<text x="%d" y="16" font-size="13" font-weight="bold">%s</text>'
                % (m_left - 40, esc(title)))
    desc = ("Bar chart of %d bins. Highest bin: %s. X axis: %s."
            % (len(bins), _fmt(hi_count), x_label))
    return _wrap(width, height, title, desc, "".join(body))


def series_svg(ticks: Sequence[int], values: Sequence[float],
               twin: Optional[Sequence[Optional[float]]] = None,
               width: int = 880, height: int = 250,
               title: str = "Applied value over time",
               run_name: str = "run", twin_name: str = "fault-free twin",
               max_points: int = 1800) -> str:
    """Time curve of the applied value, overlaid with the twin when given."""
    m_left, m_right, m_top, m_bottom = 52, 12, 30, 40
    x0, x1 = float(m_left), float(width - m_right)
    y0, y1 = float(m_top), float(height - m_bottom)
    n = len(ticks)
    body: List[str] = []
    if n == 0:
        return _wrap(width, height, title, "Empty series.",
                     '<text x="20" y="40">no data</text>')
    all_v = [v for v in values] + [t for t in (twin or []) if t is not None]
    lo, hi = min(all_v), max(all_v)
    if hi - lo < 1e-9:
        lo, hi = lo - 0.5, hi + 0.5
    pad = (hi - lo) * 0.06
    lo, hi = lo - pad, hi + pad
    stride = max(1, n // max_points)

    def sx(i: int) -> float:
        return x0 + (x1 - x0) * (i / (n - 1) if n > 1 else 0.0)

    def sy(v: float) -> float:
        return y1 - (y1 - y0) * (v - lo) / (hi - lo)

    body.append(_gridlines_y(y0, y1, x0, x1, lo, hi, 4))
    if twin is not None:
        pts = " ".join("%.2f,%.2f" % (sx(i), sy(twin[i]))
                       for i in range(0, n, stride)
                       if i < len(twin) and twin[i] is not None)
        if pts:
            body.append('<polyline class="twin" points="%s" fill="none" '
                        'stroke="%s" stroke-width="1" stroke-dasharray="4,3"/>'
                        % (pts, TWIN_COLOR))
    pts = " ".join("%.2f,%.2f" % (sx(i), sy(values[i])) for i in range(0, n, stride))
    body.append('<polyline class="run" points="%s" fill="none" stroke="%s" '
                'stroke-width="1.6"/>' % (pts, RUN_COLOR))
    for frac in (0.0, 0.5, 1.0):
        i = int(round(frac * (n - 1)))
        body.append('<text x="%.1f" y="%d" text-anchor="middle" fill="%s">%d</text>'
                    % (sx(i), height - m_bottom + 16, AXIS, ticks[i]))
    body.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"/>'
                % (x0, y1, x1, y1, AXIS))
    legend = [(RUN_COLOR, run_name)]
    if twin is not None:
        legend.append((TWIN_COLOR, twin_name))
    body.append(_legend(legend, m_left, height - 6))
    body.append('<text x="%d" y="16" font-size="13" font-weight="bold">%s</text>'
                % (8, esc(title)))
    desc = ("Applied steering value for %d ticks, overlaid with the fault-free "
            "twin. X axis: tick number. Y axis: applied value [%s, %s]."
            % (n, _fmt(lo), _fmt(hi)))
    return _wrap(width, height, title, desc, "".join(body))


def source_strip_svg(sources: Sequence[str], first_tick: int = 1,
                     width: int = 880, height: int = 74,
                     title: str = "Command source per tick") -> str:
    """Colour band of the value source per tick, with legend (English)."""
    m_left, band_y, band_h = 8, 22, 16
    plot_w = width - 2 * m_left
    n = len(sources)
    body: List[str] = ['<text x="%d" y="14" font-size="13" font-weight="bold">%s</text>'
                       % (m_left, esc(title))]
    if n == 0:
        return _wrap(width, height, title, "Empty run.", "".join(body))
    per_px = max(1, -(-n // max(plot_w, 1)))
    for col in range(0, n, per_px):
        chunk = sources[col:col + per_px]
        dom = max(set(chunk), key=chunk.count)
        w = min(per_px, n - col) * plot_w / n
        x = m_left + col * plot_w / n
        body.append('<rect x="%.2f" y="%d" width="%.2f" height="%d" fill="%s"/>'
                    % (x, band_y, max(w, 0.4), band_h, SOURCE_COLORS.get(dom, "#000")))
    for frac in (0.0, 0.5, 1.0):
        body.append('<text x="%.1f" y="%d" text-anchor="middle" fill="%s">%d</text>'
                    % (m_left + frac * plot_w, band_y + band_h + 13, AXIS,
                       first_tick + int(frac * (n - 1))))
    legend = [(SOURCE_COLORS[s], s) for s in ("holder", "shadow", "testament", "fallback")
              if s in set(sources)]
    body.append(_legend(legend, m_left, height - 4))
    counts = {s: sources.count(s) for s in set(sources)}
    desc = "Source of the applied value for each tick. " + ", ".join(
        "%s: %d ticks" % (k, v) for k, v in sorted(counts.items()))
    return _wrap(width, height, title, desc, "".join(body))


def mandate_strip_svg(holders: Sequence[str], mandates: Sequence[int],
                      events: Sequence[Tuple[int, str]] = (),
                      first_tick: int = 1, width: int = 880, height: int = 78,
                      title: str = "Mandates and handovers") -> str:
    """Band of mandate holders with handover event markers, with legend."""
    m_left, band_y, band_h = 8, 22, 16
    plot_w = width - 2 * m_left
    n = len(holders)
    body: List[str] = ['<text x="%d" y="14" font-size="13" font-weight="bold">%s</text>'
                       % (m_left, esc(title))]
    if n == 0:
        return _wrap(width, height, title, "Empty run.", "".join(body))

    def sx(i: int) -> float:
        return m_left + plot_w * (i + 0.5) / n

    start = 0
    for i in range(1, n + 1):
        if i < n and holders[i] == holders[start] and mandates[i] == mandates[start]:
            continue
        x0 = m_left + plot_w * start / n
        w = plot_w * (i - start) / n
        color = HOLDER_COLORS.get(holders[start], "#5d4037")
        label = holders[start] or "vacant"
        body.append('<rect x="%.2f" y="%d" width="%.2f" height="%d" fill="%s">'
                    "<title>%s, mandate %d, ticks %d-%d</title></rect>"
                    % (x0, band_y, max(w, 0.4), band_h, color, label,
                       mandates[start], first_tick + start, first_tick + i - 1))
        if w > 34:
            body.append('<text x="%.2f" y="%d" text-anchor="middle" fill="#fff">%s·%d</text>'
                        % (x0 + w / 2, band_y + 12, esc(label), mandates[start]))
        start = i
    for tick, kind in events:
        x = sx(min(max(tick - first_tick, 0), n - 1))
        body.append('<line x1="%.2f" y1="%d" x2="%.2f" y2="%d" stroke="#000" '
                    'stroke-dasharray="2,2"/><circle cx="%.2f" cy="%d" r="3" fill="#000">'
                    "<title>%s at tick %d</title></circle>"
                    % (x, band_y - 5, x, band_y + band_h + 5, x, band_y - 5,
                       esc(kind), tick))
    body.append(_legend([(HOLDER_COLORS["A"], "holder A"),
                         (HOLDER_COLORS["B"], "holder B"),
                         (HOLDER_COLORS[""], "vacant"),
                         ("#000000", "handover event")], m_left, height - 4))
    desc = ("Mandate holder over time with handover events. Holders: %s."
            % ", ".join(sorted(set(h or "vacant" for h in holders))))
    return _wrap(width, height, title, desc, "".join(body))
