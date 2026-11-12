"""Relais — shared helpers of the M31 command-robustness studies.

One implementation of the gestures every study repeats (ROB-*, idea 61..71):
  - run the seeded simulator with arguments, into a run folder that holds
    actuator.jsonl + twin.jsonl + scenario.json (the campaign layout of
    host/optimize/worstcase.py, so runs stay replayable);
  - judge a journal with the C verifier (the authority) and with the Python
    mirror of host/report (metrics: worst gap, deviation to the twin,
    handover delay, certification rate);
  - draw dependency-free SVG charts for the study reports (the line chart and
    the bar chart the report module does not provide).

Standard library only.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from typing import Dict, List, Optional, Sequence, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for _inc in (os.path.join(ROOT, "host", "report"),
             os.path.join(ROOT, "host", "physics")):
    if _inc not in sys.path:
        sys.path.insert(0, _inc)

from journal_io import Run, read_journal          # noqa: E402  reuse, never rewrite
from analysis import analyze_run                  # noqa: E402

SIM = os.path.join(ROOT, "build", "relais-simulate")
VERIFY = os.path.join(ROOT, "build", "relais-verify")


def require_binaries() -> None:
    """Fail early with the exact command to run when the build is missing."""
    for path in (SIM, VERIFY):
        if not os.path.exists(path):
            raise SystemExit("missing %s: run `make -j4 all` first" % path)


def params_string(params: Dict[str, object]) -> str:
    """{"T":5,"p":1} -> "T=5,p=1" (the simulator's --params syntax)."""
    return ",".join("%s=%s" % (k, v) for k, v in params.items())


def trajectory_string(traj: Dict[str, object]) -> str:
    """{"kind":"brake","amp":1.0} -> "kind=brake amp=1.0" (--trajectory)."""
    return " ".join("%s=%s" % (k, v) for k, v in traj.items())


def sim_run(out_dir: str, ticks: int, seed: int,
            params: Optional[Dict[str, object]] = None,
            traj: Optional[Dict[str, object]] = None,
            faults: Optional[Sequence[str]] = None,
            level: str = "socle", loss: float = 0.0, delay: int = 0,
            scenario: str = "study", scenario_id: int = 0,
            twin: bool = True, timeout: int = 240) -> int:
    """One seeded simulator run into out_dir; returns its exit code.

    Exit code 0 = no invariant violated, 1 = violated (the C checker judged).
    The layout actuator.jsonl + twin.jsonl + scenario.json is the campaign
    layout, so host/report and host/replay can read the run unchanged.
    """
    os.makedirs(out_dir, exist_ok=True)
    cmd = [SIM, "--ticks", str(ticks), "--seed", str(seed),
           "--scenario", scenario, "--id", str(scenario_id),
           "--journal", os.path.join(out_dir, "actuator.jsonl"), "--quiet"]
    if twin:
        cmd += ["--twin-journal", os.path.join(out_dir, "twin.jsonl")]
    if params:
        cmd += ["--params", params_string(params)]
    if traj:
        cmd += ["--trajectory", trajectory_string(traj)]
    if level == "complet":
        cmd += ["--level", "complet"]
    if loss > 0.0:
        cmd += ["--loss", repr(loss)]
    if delay > 0:
        cmd += ["--delay", str(delay)]
    for f in faults or []:
        cmd += ["--fault", f]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          timeout=timeout)
    if proc.returncode not in (0, 1):
        raise SystemExit("simulator failed (%d): %s%s"
                         % (proc.returncode, proc.stdout, proc.stderr))
    with open(os.path.join(out_dir, "scenario.json"), "w", encoding="utf-8") as fh:
        json.dump({"name": scenario, "id": scenario_id, "ticks": ticks,
                   "seed": seed, "params": params or {}, "trajectory": traj or {},
                   "level": level, "loss": loss, "delay": delay,
                   "faults": [{"spec": s} for s in (faults or [])]}, fh, indent=1)
    return proc.returncode


def verify_run(out_dir: str, level: str = "socle",
               double_fault: bool = False) -> Tuple[int, str]:
    """Judge actuator.jsonl with the C verifier; returns (exit code, output)."""
    journal = os.path.join(out_dir, "actuator.jsonl")
    cmd = [VERIFY, journal, "--level", level]
    if double_fault:
        cmd.append("--double-fault")
    twin = os.path.join(out_dir, "twin.jsonl")
    if os.path.exists(twin):
        cmd += ["--twin", twin]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=120)
    return proc.returncode, proc.stdout.strip()


def load_and_analyze(out_dir: str, level: str = "socle"):
    """Load one run folder and analyse it with the host/report mirror."""
    journal = read_journal(os.path.join(out_dir, "actuator.jsonl"))
    twin_path = os.path.join(out_dir, "twin.jsonl")
    twin = read_journal(twin_path) if os.path.exists(twin_path) else None
    with open(os.path.join(out_dir, "scenario.json"), encoding="utf-8") as fh:
        scenario = json.load(fh)
    run = Run(name=scenario.get("name", os.path.basename(out_dir)),
              path=out_dir, journal=journal, twin=twin, scenario=scenario)
    return analyze_run(run, level), journal, twin


def certification_rate(journal) -> float:
    """Share of ticks where a second was certified (REP-05 visibility)."""
    ticks = journal.ticks
    if not ticks:
        return 0.0
    return sum(1 for t in ticks if t.certified) / float(len(ticks))


def handover_ticks(journal) -> List[int]:
    """Ticks of mandate grants/auto-switches (the handover instants)."""
    return [e.tick for e in journal.events
            if e.event in ("mandate_granted", "mandate_auto_switch")]


def value_bounds_ok(journal, amp: float, slew: float,
                    tol: float = 1e-5) -> Tuple[bool, float, float]:
    """ACT-05 check on the APPLIED values: amplitude and per-tick slew bounds.

    Returns (all inside, worst amplitude, worst |delta| between two ticks).
    """
    worst_amp = 0.0
    worst_slew = 0.0
    ok = True
    prev: Optional[float] = None
    for t in journal.ticks:
        worst_amp = max(worst_amp, abs(t.value))
        if abs(t.value) > amp + tol:
            ok = False
        if prev is not None:
            worst_slew = max(worst_slew, abs(t.value - prev))
            if abs(t.value - prev) > slew + tol:
                ok = False
        prev = t.value
    return ok, worst_amp, worst_slew


def saturation_share(journal, amp: float, slew: float,
                     tol: float = 1e-4) -> Tuple[float, float]:
    """Share of ticks pinned on the amplitude bound / on the slew bound."""
    ticks = journal.ticks
    if not ticks:
        return 0.0, 0.0
    amp_hits = sum(1 for t in ticks if abs(abs(t.value) - amp) < tol)
    slew_hits = 0
    for a, b in zip(ticks, ticks[1:]):
        if abs(abs(b.value - a.value) - slew) < tol:
            slew_hits += 1
    return amp_hits / float(len(ticks)), slew_hits / float(len(ticks) - 1)


# ---------------------------------------------------------------------------
# Dependency-free SVG charts (the report module has run/twin curves and
# histograms of bins; the studies need parameter sweeps and strategy bars).
# ---------------------------------------------------------------------------

def _svg_wrap(width: int, height: int, title: str, body: str) -> str:
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
            'viewBox="0 0 %d %d" role="img" font-family="sans-serif" '
            'font-size="11"><title>%s</title>%s</svg>'
            % (width, height, width, height, title, body))


def _scale(lo: float, hi: float, pad_frac: float = 0.08) -> Tuple[float, float]:
    if hi - lo < 1e-12:
        return lo - 0.5, hi + 0.5
    pad = (hi - lo) * pad_frac
    return lo - pad, hi + pad


def line_chart_svg(xs: Sequence[float], series: Sequence[Tuple[str, Sequence[float], str]],
                   x_label: str, y_label: str, title: str,
                   width: int = 640, height: int = 300, log_x: bool = False) -> str:
    """One polyline per (name, ys, color) over a shared numeric x axis."""
    import math as _m
    ml, mr, mt, mb = 64, 14, 30, 52
    x0, x1, y0, y1 = float(ml), float(width - mr), float(mt), float(height - mb)
    tx = [_m.log10(v) if log_x else float(v) for v in xs]
    all_y = [v for _, ys, _ in series for v in ys]
    lo, hi = _scale(min(all_y), max(all_y))
    tlo, thi = _scale(min(tx), max(tx), 0.02)

    def sx(v: float) -> float:
        t = _m.log10(v) if log_x else v
        return x0 + (x1 - x0) * ((t - tlo) / (thi - tlo) if thi > tlo else 0.0)

    def sy(v: float) -> float:
        return y1 - (y1 - y0) * ((v - lo) / (hi - lo))

    body: List[str] = []
    for i in range(5):  # gridlines + y labels
        v = lo + (hi - lo) * i / 4.0
        y = sy(v)
        body.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#e0e0e0"/>'
                    '<text x="%.1f" y="%.1f" text-anchor="end" fill="#424242">%g</text>'
                    % (x0, y, x1, y, x0 - 5, y + 3.5, v))
    for xv in xs:  # x labels
        body.append('<text x="%.1f" y="%.1f" text-anchor="middle" fill="#424242">%g</text>'
                    % (sx(xv), y1 + 15, xv))
    for name, ys, color in series:
        pts = " ".join("%.2f,%.2f" % (sx(x), sy(y)) for x, y in zip(xs, ys))
        body.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="1.8"/>'
                    % (pts, color))
        for x, y in zip(xs, ys):
            body.append('<circle cx="%.2f" cy="%.2f" r="2.6" fill="%s"><title>%s: x=%g '
                        'y=%.6g</title></circle>' % (sx(x), sy(y), color, name, x, y))
    lx = x0
    for name, _, color in series:  # legend
        body.append('<rect x="%d" y="%d" width="10" height="10" fill="%s"/>'
                    '<text x="%d" y="%d">%s</text>' % (lx, height - 16, color,
                                                       lx + 13, height - 7, name))
        lx += 30 + 7 * len(name)
    body.append('<text x="%d" y="16" font-size="13" font-weight="bold">%s</text>'
                % (8, title))
    body.append('<text x="%.1f" y="%.1f" text-anchor="middle" fill="#424242">%s</text>'
                % ((x0 + x1) / 2, height - 26, x_label))
    body.append('<text x="14" y="%.1f" text-anchor="middle" fill="#424242" '
                'transform="rotate(-90 14 %.1f)">%s</text>' % ((y0 + y1) / 2,
                                                               (y0 + y1) / 2, y_label))
    return _svg_wrap(width, height, title, "".join(body))


def bar_chart_svg(labels: Sequence[str], series: Sequence[Tuple[str, Sequence[float], str]],
                  y_label: str, title: str, width: int = 640, height: int = 300) -> str:
    """Grouped vertical bars: one group per label, one bar per series."""
    ml, mr, mt, mb = 64, 14, 30, 64
    x0, x1, y0, y1 = float(ml), float(width - mr), float(mt), float(height - mb)
    all_v = [v for _, ys, _ in series for v in ys]
    lo, hi = 0.0, _scale(0.0, max(all_v))[1]
    n_groups = max(len(labels), 1)
    gw = (x1 - x0) / n_groups
    bw = gw / (len(series) + 1)

    def sy(v: float) -> float:
        return y1 - (y1 - y0) * ((v - lo) / (hi - lo) if hi > lo else 0.0)

    body: List[str] = []
    for i in range(5):
        v = lo + (hi - lo) * i / 4.0
        body.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#e0e0e0"/>'
                    '<text x="%.1f" y="%.1f" text-anchor="end" fill="#424242">%g</text>'
                    % (x0, sy(v), x1, sy(v), x0 - 5, sy(v) + 3.5, v))
    for gi, label in enumerate(labels):
        gx = x0 + gi * gw
        body.append('<text x="%.1f" y="%.1f" text-anchor="middle" fill="#424242">%s</text>'
                    % (gx + gw / 2, y1 + 15, label))
        for si, (name, ys, color) in enumerate(series):
            v = ys[gi] if gi < len(ys) else 0.0
            bx = gx + (si + 0.5) * bw
            body.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s">'
                        '<title>%s / %s: %.6g</title></rect>'
                        % (bx, sy(v), bw * 0.9, y1 - sy(v), color, label, name, v))
    lx = x0
    for name, _, color in series:
        body.append('<rect x="%d" y="%d" width="10" height="10" fill="%s"/>'
                    '<text x="%d" y="%d">%s</text>' % (lx, height - 16, color,
                                                       lx + 13, height - 7, name))
        lx += 30 + 7 * len(name)
    body.append('<text x="%d" y="16" font-size="13" font-weight="bold">%s</text>'
                % (8, title))
    body.append('<text x="14" y="%.1f" text-anchor="middle" fill="#424242" '
                'transform="rotate(-90 14 %.1f)">%s</text>'
                % ((y0 + y1) / 2, (y0 + y1) / 2, y_label))
    return _svg_wrap(width, height, title, "".join(body))


def write_text(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
