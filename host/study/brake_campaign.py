#!/usr/bin/env python3
"""Relais — the braking campaign (M31, ideas 61 & 71, CMD-08).

Plays scenarios/brake-*.json with the seeded simulator at BOTH levels (socle
and complet), judges every run with the C verifier, and answers the mission
question with numbers: IS THE BRAKING INTERRUPTED BY THE HANDOVER?

Published per run:
  - the verdict of the C invariant checker (the authority);
  - the deviation to the DECELERATION SETPOINT: during the ramp the rack
    velocity must equal -amp (units/s); we publish max |vel + amp| over the
    ramp window, for the faulted run AND for the twin (the lag exists even
    without fault; what matters is the difference);
  - the position tracking error max |pos - setpoint| over the ramp window;
  - interruption markers: the worst rack-velocity RISE inside the ramp after
    the fault instant (a braking that stalls shows up as vel climbing back
    towards zero), and the worst deviation to the twin value;
  - the handover delay and the worst fresh-order gap (host/report mirror).

Usage:
  python3 host/study/brake_campaign.py [--out runs/m31-brake] [--ticks-override N]
Exit code: 0 when every verifier said no violation, 1 otherwise (published
either way — an honest red is a result).
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "host", "bench"))

import study_common as sc            # noqa: E402
from relais_bench import fault_arg   # noqa: E402  reuse the scenario translation

RAMP_SECONDS = 1.0  # the brake profile of plant.c ramps for exactly one second
SETTLE_TICKS = 40   # 0.2 s: the rack needs this to reach the ramp velocity
HANDOVER_SPAN = 40  # ticks of the handover window studied after the fault


def ramp_window(scenario: dict) -> tuple:
    """First and last tick of the braking ramp, from the scenario itself."""
    traj = scenario.get("trajectory", {})
    period_s = float(scenario.get("params", {}).get("T", 5)) / 1000.0
    t_start = float(traj.get("t_start", 2.0))
    first = int(round(t_start / period_s)) + 2   # setpoint_at uses (tick-1)*dt
    last = int(round((t_start + RAMP_SECONDS) / period_s)) + 1
    return first, last


def _window_stats(journal, lo: int, hi: int, amp: float) -> dict:
    """Deviation to the deceleration setpoint over [lo, hi]: during the ramp
    the rack velocity must equal -amp, so the error is |vel + amp|."""
    rows = [t for t in journal.ticks if lo <= t.tick <= hi]
    if not rows:
        return {"decel_err_max": 0.0, "decel_err_mean": 0.0, "pos_err_max": 0.0,
                "worst_vel_rise": 0.0}
    vel_err = [abs(t.vel + amp) for t in rows]
    pos_err = [abs(t.pos - t.setpoint) for t in rows]
    rises = [b.vel - a.vel for a, b in zip(rows, rows[1:]) if b.vel > a.vel]
    return {"decel_err_max": max(vel_err),
            "decel_err_mean": sum(vel_err) / len(vel_err),
            "pos_err_max": max(pos_err),
            "worst_vel_rise": max(rises) if rises else 0.0}


def decel_metrics(journal, window: tuple, amp: float) -> dict:
    """Steady part of the ramp: the startup transient is excluded because it
    is identical with or without fault (the twin lives it too)."""
    lo, hi = window
    return _window_stats(journal, lo + SETTLE_TICKS, hi, amp)


def handover_metrics(journal, twin, window: tuple, amp: float,
                     fault_at: int) -> dict:
    """The handover window itself: [fault, fault+span] inside the ramp."""
    lo, hi = window
    w_lo = max(lo, fault_at)
    w_hi = min(hi, fault_at + HANDOVER_SPAN)
    out = _window_stats(journal, w_lo, w_hi, amp)
    if twin is not None:
        ref = {t.tick: t for t in twin.ticks}
        rows = [t for t in journal.ticks if w_lo <= t.tick <= w_hi and t.tick in ref]
        out["twin_dev_value"] = max((abs(t.value - ref[t.tick].value) for t in rows),
                                    default=0.0)
        out["twin_dev_vel"] = max((abs(t.vel - ref[t.tick].vel) for t in rows),
                                  default=0.0)
        out["twin_decel_err"] = _window_stats(twin, w_lo, w_hi, amp)["decel_err_max"]
    return out


def play_one(scenario_path: str, out_root: str, level: str) -> dict:
    """Run one scenario at one level; return its measurement row."""
    with open(scenario_path, encoding="utf-8") as fh:
        scenario = json.load(fh)
    name = scenario["name"]
    out_dir = os.path.join(out_root, "%s-%s" % (name, level))
    faults = [fault_arg(f) for f in scenario.get("faults", [])]
    fault_at = min((int(f.get("at_tick", 0)) for f in scenario.get("faults", [])),
                   default=0)
    rc_sim = sc.sim_run(out_dir, int(scenario["ticks"]), int(scenario["seed"]),
                        params=scenario.get("params"), traj=scenario.get("trajectory"),
                        faults=faults, level=level, scenario=name,
                        scenario_id=int(scenario.get("id", 0)))
    rc_verify, verdict = sc.verify_run(out_dir, level)
    ana, journal, twin = sc.load_and_analyze(out_dir, level)
    window = ramp_window(scenario)
    amp = float(scenario.get("trajectory", {}).get("amp", 1.0))
    row = {"scenario": name, "level": level, "sim_rc": rc_sim,
           "verify_rc": rc_verify, "verdict": verdict.splitlines()[-1] if verdict else "",
           "violations": ana.violation_count, "max_gap": ana.max_gap,
           "handover_delay": ana.max_handover_delay,
           "certified_rate": round(sc.certification_rate(journal), 4)}
    row.update(decel_metrics(journal, window, amp))
    if twin is not None:
        row.update({"twin_" + k: v for k, v in
                    decel_metrics(twin, window, amp).items()})
    if fault_at:
        row.update(handover_metrics(journal, twin, window, amp, fault_at))
    else:
        row.update({"twin_dev_value": 0.0, "twin_dev_vel": 0.0,
                    "twin_decel_err": row["decel_err_max"]})
        row["worst_vel_rise"] = _window_stats(
            journal, window[0], window[1], amp)["worst_vel_rise"]
    return row


def format_table(rows: list) -> str:
    head = ("| scenario | level | verify | viol | gap | handover | "
            "decel_err steady (run/twin) | ho_decel_err (run/twin) | "
            "ho_vel_rise | ho_dev value/vel | pos_err ramp | cert.rate |\n"
            "|---|---|---|---|---|---|---|---|---|---|---|---|\n")
    lines = [head]
    for r in rows:
        lines.append("| %s | %s | %s | %d | %d | %d | %.4f / %.4f | %.4f / %.4f | "
                     "%+.4f | %.5f / %.5f | %.4f | %.2f |\n"
                     % (r["scenario"], r["level"],
                        "OK" if r["verify_rc"] == 0 else "VIOLATION",
                        r["violations"], r["max_gap"], r["handover_delay"],
                        r["decel_err_max"], r.get("twin_decel_err_max", 0.0),
                        r.get("decel_err_max", 0.0), r.get("twin_decel_err", 0.0),
                        r["worst_vel_rise"], r.get("twin_dev_value", 0.0),
                        r.get("twin_dev_vel", 0.0), r["pos_err_max"],
                        r["certified_rate"]))
    return "".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="M31 braking campaign (CMD-08)")
    ap.add_argument("--out", default=os.path.join(ROOT, "runs", "m31-brake"))
    ap.add_argument("--scenarios", default=os.path.join(ROOT, "scenarios", "brake-*.json"))
    args = ap.parse_args()
    sc.require_binaries()
    paths = sorted(glob.glob(args.scenarios))
    if not paths:
        raise SystemExit("no scenario matches %s" % args.scenarios)
    rows = []
    for path in paths:
        for level in ("socle", "complet"):
            rows.append(play_one(path, args.out, level))
            r = rows[-1]
            print("%-22s %-7s verify=%s violations=%d decel_err=%.4f (twin %.4f) "
                  "ho_decel=%.4f ho_vel_rise=%+.4f ho_dev=%.5f"
                  % (r["scenario"], r["level"],
                     "OK" if r["verify_rc"] == 0 else "VIOLATION", r["violations"],
                     r["decel_err_max"], r.get("twin_decel_err_max", 0.0),
                     r.get("decel_err_max", 0.0), r["worst_vel_rise"],
                     r.get("twin_dev_value", 0.0)))
    table = format_table(rows)
    print("\n" + table)
    interrupted = [r for r in rows
                   if r["worst_vel_rise"] > 0.05
                   or r["decel_err_max"] > r.get("twin_decel_err", 0.0) + 0.25
                   or r.get("twin_dev_value", 0.0) > 0.02]
    verdict = ("braking NOT interrupted by the handover in any run"
               if not interrupted else
               "braking degraded in: %s" % ", ".join(
                   "%s/%s" % (r["scenario"], r["level"]) for r in interrupted))
    print("VERDICT: " + verdict)
    sc.write_text(os.path.join(args.out, "RESULTS.md"),
                  "# M31 braking campaign (ideas 61, 71 — CMD-08)\n\n"
                  "Replay one row with:\n\n"
                  "    ./build/relais-simulate --ticks 2000 --seed <seed> "
                  "--params T=5,p=1,N=8 \\\n"
                  "      --trajectory 'kind=brake amp=1.0 offset=0.4 t_start=2.0' "
                  "[--level complet] [--fault <spec>] \\\n"
                  "      --journal runs/m31-brake/<run>/actuator.jsonl "
                  "--twin-journal runs/m31-brake/<run>/twin.jsonl\n\n"
                  "or `python3 host/bench/relais_bench.py --sim --level <level> "
                  "--scenario scenarios/<name>.json`.\n\n" + table + "\n"
                  + verdict + "\n")
    bad = [r for r in rows if r["verify_rc"] != 0 or r["sim_rc"] != 0]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
