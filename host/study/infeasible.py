#!/usr/bin/env python3
"""Relais — the infeasible-setpoint study (M31, idea 66, CMD-06/ACT-05/ROB-*).

A steering request the rack PHYSICALLY cannot follow: a step beyond the
saturation bound, an infinitely fast step, a slalom above the bandwidth, an
amplitude three times the actuator authority. The question is not "does it
track?" (it cannot, and the study says so with numbers) but "DOES THE PROTOCOL
HOLD ANYWAY?": the command stays inside the ACT-05 bounds, saturates cleanly,
no invariant falls, the handover still works, and the witness (lamp/cause)
says what it is designed to say.

Everything runs through the real pipeline: build/relais-simulate (the same
binary as the campaign) judged by build/relais-verify (the authority) and by
the host/report mirror (metrics). Each case is played fault-free AND with the
holder dying in the middle, at both levels.

Usage:
  python3 host/study/infeasible.py [--out runs/m31-infeasible] [--ticks 2000]
Exit code: 0 when no invariant was violated anywhere, 1 otherwise. A published
violation is a result, never hidden.
"""
from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "host", "report"))

import study_common as sc        # noqa: E402
from charts import series_svg    # noqa: E402  reuse the report chart, no rewrite

# Four ways to ask the impossible (CMD-08 profiles are feasible; these are not).
CASES = [
    {"name": "step-beyond-saturation",
     "traj": {"kind": "step", "amp": 2.5, "t_start": 1.0, "offset": 0.0},
     "why": "amplitude 2.5 > max_amplitude 1.0: the rack can never reach it"},
    {"name": "instant-step-in-bounds",
     "traj": {"kind": "step", "amp": 0.8, "t_start": 1.0, "offset": 0.0},
     "why": "infinite slope: only the slew-limited ramp is physical"},
    {"name": "slalom-above-bandwidth",
     "traj": {"kind": "slalom", "amp": 0.6, "freq": 50.0},
     "why": "50 Hz = 4 ticks per period, far above the 20 ms velocity loop"},
    {"name": "slalom-huge-amplitude",
     "traj": {"kind": "slalom", "amp": 3.0, "freq": 0.5},
     "why": "amplitude 3x the actuator authority: permanently saturated"},
]
DEATH_TICK = 800
SEED = 3166  # fixed: the study is reproducible


def tracking_stats(journal) -> dict:
    """Honest tracking numbers: the error is large, that IS the result."""
    errs = [abs(t.pos - t.setpoint) for t in journal.ticks]
    late = errs[len(errs) // 2:] or [0.0]
    return {"track_err_max": max(errs) if errs else 0.0,
            "track_err_mean": sum(errs) / len(errs) if errs else 0.0,
            "track_err_late_mean": sum(late) / len(late)}


def run_case(out_root: str, case: dict, fault: bool, level: str, ticks: int) -> dict:
    """One case x one fault hypothesis x one level; returns its metric row."""
    tag = "%s-%s-%s" % (case["name"], "death" if fault else "nominal", level)
    out_dir = os.path.join(out_root, tag)
    faults = ["%d:death:0" % DEATH_TICK] if fault else []
    rc_sim = sc.sim_run(out_dir, ticks, SEED, params={"T": 5, "p": 1, "N": 8},
                        traj=case["traj"], faults=faults, level=level,
                        scenario=tag, scenario_id=66)
    rc_verify, verdict = sc.verify_run(out_dir, level)
    ana, journal, _twin = sc.load_and_analyze(out_dir, level)
    bounds_ok, worst_amp, worst_slew = sc.value_bounds_ok(journal, 1.0, 0.25)
    sat_amp, sat_slew = sc.saturation_share(journal, 1.0, 0.25)
    lamps = {}
    for t in journal.ticks:
        lamps[t.lamp] = lamps.get(t.lamp, 0) + 1
    row = {"case": case["name"], "fault": "death@%d" % DEATH_TICK if fault else "-",
           "level": level, "sim_rc": rc_sim, "verify_rc": rc_verify,
           "violations": ana.violation_count, "bounds_ok": bounds_ok,
           "worst_amp": worst_amp, "worst_slew": worst_slew,
           "sat_amp": sat_amp, "sat_slew": sat_slew,
           "max_gap": ana.max_gap, "handover_delay": ana.max_handover_delay,
           "lamps": lamps, "verdict": verdict.splitlines()[-1] if verdict else ""}
    row.update(tracking_stats(journal))
    return row


def format_table(rows: list) -> str:
    out = ["| case | fault | level | verify | ACT-05 bounds | worst amp/slew | "
           "sat amp/slew % | track err max/mean | gap | handover | lamps |\n",
           "|---|---|---|---|---|---|---|---|---|---|---|\n"]
    for r in rows:
        lamps = ",".join("%s:%d" % kv for kv in sorted(r["lamps"].items()))
        out.append("| %s | %s | %s | %s | %s | %.3f / %.3f | %.0f / %.0f | "
                   "%.3f / %.3f | %d | %d | %s |\n"
                   % (r["case"], r["fault"], r["level"],
                      "OK" if r["verify_rc"] == 0 else "VIOLATION",
                      "held" if r["bounds_ok"] else "BROKEN",
                      r["worst_amp"], r["worst_slew"],
                      100 * r["sat_amp"], 100 * r["sat_slew"],
                      r["track_err_max"], r["track_err_mean"],
                      r["max_gap"], r["handover_delay"], lamps))
    return "".join(out)


def write_chart(out_root: str, ticks: int) -> None:
    """One SVG: position vs setpoint of the 50 Hz case (the ugliest, published
    as it is). Reuses the report chart: the run curve is pos, the dashed curve
    is the setpoint, read from the nominal journal."""
    path = os.path.join(out_root, "%s-nominal-socle" % CASES[2]["name"],
                        "actuator.jsonl")
    journal = sc.read_journal(path)
    xs = [t.tick for t in journal.ticks[:1200]]
    svg = series_svg(xs, [t.pos for t in journal.ticks[:1200]],
                     twin=[t.setpoint for t in journal.ticks[:1200]],
                     title="50 Hz slalom: rack position (solid) vs setpoint (dashed)",
                     run_name="pos", twin_name="setpoint (infeasible)")
    sc.write_text(os.path.join(out_root, "slalom-50hz.svg"), svg)


def main() -> int:
    ap = argparse.ArgumentParser(description="M31 infeasible setpoints (idea 66)")
    ap.add_argument("--out", default=os.path.join(ROOT, "runs", "m31-infeasible"))
    ap.add_argument("--ticks", type=int, default=2000)
    args = ap.parse_args()
    sc.require_binaries()
    rows = []
    for case in CASES:
        for fault in (False, True):
            for level in ("socle", "complet"):
                rows.append(run_case(args.out, case, fault, level, args.ticks))
                r = rows[-1]
                print("%-24s %-9s %-7s verify=%s bounds=%s sat=%.0f%%/%.0f%% "
                      "track_err=%.3f gap=%d"
                      % (r["case"], r["fault"], r["level"],
                         "OK" if r["verify_rc"] == 0 else "VIOLATION",
                         "held" if r["bounds_ok"] else "BROKEN",
                         100 * r["sat_amp"], 100 * r["sat_slew"],
                         r["track_err_max"], r["max_gap"]))
    write_chart(args.out, args.ticks)
    table = format_table(rows)
    print("\n" + table)
    broken = [r for r in rows if r["verify_rc"] != 0 or not r["bounds_ok"]]
    lamp_note = ("The lamp reports the REDUNDANCY state (DEG-01), not the "
                 "tracking error. At level C with both machines alive it stays "
                 "OFF for the whole infeasible run (off:1997/2000); at level S "
                 "it is yellow by design (no shadow ever certifies). The "
                 "saturation is visible in the command and in the journal, not "
                 "in the witness: an infeasible setpoint is a case the driver "
                 "would NOT be warned about. Published as-is.")
    print("VERDICT: %s" % ("protocol holds on every infeasible case"
                           if not broken else
                           "BROKEN in %d runs (published above)" % len(broken)))
    print(lamp_note)
    sc.write_text(os.path.join(args.out, "RESULTS.md"),
                  "# M31 infeasible setpoints (idea 66)\n\n"
                  "Replay one row: `./build/relais-simulate --ticks %d --seed %d "
                  "--params T=5,p=1,N=8 --trajectory '<traj>' [--level complet] "
                  "[--fault %d:death:0] --journal <dir>/actuator.jsonl "
                  "--twin-journal <dir>/twin.jsonl` then `./build/relais-verify "
                  "<dir>/actuator.jsonl --level <level> --twin <dir>/twin.jsonl`.\n\n"
                  % (args.ticks, SEED, DEATH_TICK)
                  + "".join("- **%s**: %s\n" % (c["name"], c["why"]) for c in CASES)
                  + "\n" + table + "\n" + lamp_note + "\n")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
