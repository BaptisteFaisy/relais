#!/usr/bin/env python3
"""Relais — testament strategy comparison (M31, idea 69, ACT-02).

The core fills the testament (the will of the last order) by predicting with
the model: it runs the same control law on the same rack model, extrapolating
the manoeuvre with the filtered slope of the setpoint (control.c,
relais_testament_fill). Four ALTERNATIVE strategies are compared against it on
the SAME campaign, offline, without touching the core:

  hold_last       freeze the command at its last value;
  linear          linear extrapolation of the command slope;
  setpoint_slope  extrapolate the command with the filtered setpoint slope;
  moving_average  hold the mean of the last four commands.

Method: for every holder death of the campaign, anchor t0 = last tick served by
a fresh holder order. The law state at t0 is rebuilt by replaying the journal
through the float32 mirror of host/physics/plant_law.py (validated tick by tick
against the C journal). Each strategy predicts the N will-values for
t0+1..t0+N; the reference is the fault-free TWIN on the same ticks (what the
holder would have sent). Published per strategy: mean/max of the worst
deviation and the RMS deviation over all windows, plus the fidelity check
"the mirror of the current strategy reproduces the will the C arbiter actually
played" — if the mirror drifted, the comparison would be meaningless.

If another strategy beats the current one, this script SAYS SO with the
numbers; changing core/src/control.c is out of M31 scope (rule 10), the
proposal lives in the report.

Usage:
  python3 host/study/testament_opt.py [--out runs/m31-testament] [--seeds 1,2,3]
Exit code: 0 when every run stayed green and the mirror fidelity held.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "host", "physics"))

import study_common as sc       # noqa: E402
import plant_law as pl          # noqa: E402

SEED_BASE = 6900
TICKS = 1400
DEATHS = (300, 500, 700, 900, 1100)
PARAMS = pl.default_params()  # T=5, p=1, N=8 + the ACT-05 bounds (amp, slew)
TRAJS = {
    "slalom": {"kind": "slalom", "amp": 0.6, "freq": 0.5},
    "brake": {"kind": "brake", "amp": 1.0, "offset": 0.4, "t_start": 2.0},
}
FIDELITY_TOL = 2e-3  # float32 mirror vs the C will actually played


def anchor_tick(journal, death: int) -> int:
    """t0: the last tick served by a fresh order of the dying holder.

    The fault lands at the START of tick `death`, so the order the holder
    queued at death-1 is still applied at death; the successor can only be
    granted at death+2 (demande queued at death+1), which stays out of the
    window `tick <= death + 1`.
    """
    t0 = 0
    for t in journal.ticks:
        if t.tick > death + 1:
            break
        if t.source == "holder":
            t0 = t.tick
    return t0


def replay_state(journal, t0: int, params: dict):
    """Law state after consuming rows 1..t0-1, and the row t0-1 itself.

    That is exactly the holder state when it filled the will of the order
    applied at t0 (node.c computes on statut(t0-1), ACT-02).
    """
    state = pl.CtlState()
    rows = journal.ticks
    for i in range(t0 - 1):  # rows[0..t0-2] = ticks 1..t0-1
        r = rows[i]
        state.step({"pos": r.pos, "vel": r.vel, "setpoint": r.setpoint,
                    "applied": r.value}, params)
    last = rows[t0 - 2] if t0 >= 2 else None
    meas = {"pos": last.pos, "vel": last.vel, "setpoint": last.setpoint,
            "applied": last.value} if last else \
        {"pos": 0.0, "vel": 0.0, "setpoint": 0.0, "applied": 0.0}
    history = [r.value for r in rows[:t0]]
    return state, meas, history


def window_truth(journal, twin, t0: int, n: int):
    """Twin values for t0+1..t0+n (what the holder would have sent)."""
    ref = {t.tick: t.value for t in twin.ticks}
    return [ref.get(t0 + 1 + i) for i in range(n)]


def played_will(journal, t0: int, n: int):
    """The will values the C arbiter ACTUALLY played inside t0+1..t0+n, as
    {offset: value}; ticks already served by the successor are left out (the
    double-death fidelity run plays the full horizon, see fidelity_run)."""
    out = {}
    for r in journal.ticks:
        if t0 < r.tick <= t0 + n and r.source == "testament":
            out[r.tick - t0 - 1] = r.value
    return out or None


def deviations(pred, truth) -> dict:
    """Max and RMS deviation of one strategy over one window."""
    pairs = [(p, w) for p, w in zip(pred, truth) if w is not None]
    if not pairs:
        return {"dev_max": None, "dev_rms": None, "n": 0}
    errs = [abs(p - w) for p, w in pairs]
    return {"dev_max": max(errs),
            "dev_rms": math.sqrt(sum(e * e for e in errs) / len(errs)),
            "n": len(errs)}


def study_run(out_root: str, traj_name: str, death: int, seed: int,
              level: str) -> dict:
    """One run of the campaign: play it, anchor it, score every strategy."""
    tag = "%s-death%d-%s-s%d" % (traj_name, death, level, seed)
    out_dir = os.path.join(out_root, tag)
    rc_sim = sc.sim_run(out_dir, TICKS, seed, params=PARAMS,
                        traj=TRAJS[traj_name], faults=["%d:death:0" % death],
                        level=level, scenario=tag, scenario_id=69)
    rc_verify, _ = sc.verify_run(out_dir, level)
    ana, journal, twin = sc.load_and_analyze(out_dir, level)
    t0 = anchor_tick(journal, death)
    result = {"tag": tag, "traj": traj_name, "death": death, "seed": seed,
              "level": level, "rc": max(rc_sim, rc_verify),
              "violations": ana.violation_count, "t0": t0}
    if t0 < 2 or twin is None:
        result["error"] = "no anchor or no twin"
        return result
    state, meas, history = replay_state(journal, t0, PARAMS)
    truth = window_truth(journal, twin, t0, PARAMS["N"])
    played = played_will(journal, t0, PARAMS["N"])
    for name, fn in pl.STRATEGIES.items():
        pred = fn(state, meas, PARAMS, history)
        result[name] = deviations(pred, truth)
        if name == "model" and played:
            fid = [abs(pred[i] - v) for i, v in played.items()]
            result["fidelity_max"] = max(fid)
            result["fidelity_ticks"] = len(fid)
    return result


def fidelity_run(out_root: str, seed: int, death: int = 300) -> dict:
    """The will played over its FULL horizon: both machines die at the same
    tick (the pattern of scenarios/slalom-double-death.json), so no successor
    is ever granted and the arbiter plays the testament of the last order for
    its whole N ticks before the fallback law. The mirror of the current
    strategy must reproduce those N values (float32 tolerance) — that is the
    proof the offline comparison below is faithful."""
    tag = "fidelity-double-death-s%d" % seed
    out_dir = os.path.join(out_root, tag)
    rc_sim = sc.sim_run(out_dir, TICKS, seed, params=PARAMS,
                        traj=TRAJS["slalom"],
                        faults=["%d:death:0" % death, "%d:death:1" % death],
                        level="socle", scenario=tag, scenario_id=69)
    rc_verify, _ = sc.verify_run(out_dir, "socle", double_fault=True)
    _ana, journal, twin = sc.load_and_analyze(out_dir, "socle")
    t0 = anchor_tick(journal, death)
    state, meas, history = replay_state(journal, t0, PARAMS)
    played = played_will(journal, t0, PARAMS["N"]) or {}
    pred = pl.STRATEGIES["model"](state, meas, PARAMS, history)
    fid = [abs(pred[i] - v) for i, v in played.items()]
    truth = window_truth(journal, twin, t0, PARAMS["N"])
    return {"tag": tag, "rc": max(rc_sim, rc_verify), "t0": t0,
            "horizon_played": len(played),
            "fidelity_max": max(fid) if fid else float("nan"),
            "model_dev_max": deviations(pred, truth)["dev_max"],
            "model_dev_rms": deviations(pred, truth)["dev_rms"]}


def campaign(out_root: str, seeds: list) -> list:
    """The same campaign for every strategy: deaths x trajectories x levels."""
    results = []
    for traj_name in TRAJS:
        deaths = DEATHS if traj_name == "slalom" else (500, 550)
        for death in deaths:
            for seed in seeds:
                for level in ("socle", "complet"):
                    r = study_run(out_root, traj_name, death,
                                  SEED_BASE + seed * 100 + death, level)
                    results.append(r)
                    print("  %-28s t0=%-5d rc=%d viol=%d fid=%s model_dev=%s"
                          % (r["tag"], r["t0"], r["rc"], r["violations"],
                             "%.2e" % r["fidelity_max"] if "fidelity_max" in r
                             else "-",
                             "%.4f" % r["model"]["dev_max"]
                             if r.get("model", {}).get("dev_max") is not None
                             else "-"))
    return results


def ranking(results: list) -> list:
    """Aggregate the windows per strategy; sort by mean of the worst deviation."""
    rows = []
    for name in pl.STRATEGIES:
        dev_max = [r[name]["dev_max"] for r in results
                   if r.get(name, {}).get("dev_max") is not None]
        dev_rms = [r[name]["dev_rms"] for r in results
                   if r.get(name, {}).get("dev_rms") is not None]
        if not dev_max:
            continue
        rows.append({"strategy": name, "windows": len(dev_max),
                     "mean_dev_max": sum(dev_max) / len(dev_max),
                     "worst_dev_max": max(dev_max),
                     "mean_dev_rms": sum(dev_rms) / len(dev_rms)})
    rows.sort(key=lambda r: r["mean_dev_max"])
    return rows


def format_ranking(rows: list) -> str:
    out = ["| rank | strategy | windows | mean worst dev | worst dev | mean RMS |\n",
           "|---|---|---|---|---|---|\n"]
    for i, r in enumerate(rows, 1):
        out.append("| %d | %s | %d | %.5f | %.5f | %.5f |\n"
                   % (i, r["strategy"], r["windows"], r["mean_dev_max"],
                      r["worst_dev_max"], r["mean_dev_rms"]))
    return "".join(out)


def write_chart(out_root: str, rows: list) -> None:
    labels = [r["strategy"] for r in rows]
    sc.write_text(os.path.join(out_root, "ranking.svg"),
                  sc.bar_chart_svg(labels, [
                      ("mean worst dev", [r["mean_dev_max"] for r in rows],
                       "#1565c0"),
                      ("worst dev", [r["worst_dev_max"] for r in rows],
                       "#c62828")],
                      "deviation to the fault-free twin (full scale = 1)",
                      "Testament strategies, same campaign"))


def verdict_of(rows: list) -> str:
    """The published conclusion of the comparison (idea 69)."""
    best = rows[0] if rows else None
    current = next((r for r in rows if r["strategy"] == "model"), None)
    if best and current and best["strategy"] != "model":
        return ("ANOTHER STRATEGY BEATS THE CURRENT ONE: %s "
                "(mean worst dev %.5f vs %.5f for model, %.1f%% better); "
                "proposed core change is described in the report, NOT "
                "applied (rule 10)."
                % (best["strategy"], best["mean_dev_max"],
                   current["mean_dev_max"],
                   100.0 * (1 - best["mean_dev_max"] /
                            max(current["mean_dev_max"], 1e-12))))
    return ("the current model-prediction strategy stays the best of "
            "the five on this campaign")


def _results_md(table: str, seeds: list, fid_max: float, fid_count: int,
                verdict: str) -> str:
    return ("# M31 testament strategies (idea 69)\n\n"
            "Same campaign for every strategy: deaths at ticks %s on the "
            "slalom (and 500/550 on the brake profile), seeds %s, both "
            "levels, %d ticks, params T=5,p=1,N=8.\n\n"
            "Replay one window: `./build/relais-simulate --ticks %d "
            "--seed <seed> --params T=5,p=1,N=8 --trajectory '<traj>' "
            "--fault <D>:death:0 [--level complet] --journal <dir>/"
            "actuator.jsonl --twin-journal <dir>/twin.jsonl`, then "
            "`python3 host/study/testament_opt.py`.\n\n"
            % (", ".join(map(str, DEATHS)), seeds, TICKS, TICKS)
            + table + "\nFidelity of the Python mirror vs the will the C "
            "arbiter actually played: worst %.2e (tolerance %.0e) over "
            "%d windows.\n\n%s\n" % (fid_max, FIDELITY_TOL, fid_count,
                                     verdict))


def main() -> int:
    ap = argparse.ArgumentParser(description="M31 testament strategies (idea 69)")
    ap.add_argument("--out", default=os.path.join(ROOT, "runs", "m31-testament"))
    ap.add_argument("--seeds", default="1,2,3")
    args = ap.parse_args()
    sc.require_binaries()
    seeds = [int(s) for s in args.seeds.split(",") if s.strip()]
    results = campaign(args.out, seeds)
    fid_runs = []
    for seed in seeds:
        fr = fidelity_run(args.out, SEED_BASE + seed * 100)
        fid_runs.append(fr)
        print("  %-28s rc=%d horizon=%d/%d fidelity=%.2e model_dev=%.5f"
              % (fr["tag"], fr["rc"], fr["horizon_played"], PARAMS["N"],
                 fr["fidelity_max"], fr["model_dev_max"]))
    rows = ranking(results)
    table = format_ranking(rows)
    print("\n" + table)
    fid = [r["fidelity_max"] for r in results if "fidelity_max" in r] + \
          [r["fidelity_max"] for r in fid_runs]
    fid_max = max(fid) if fid else float("nan")
    full_horizon = all(r["horizon_played"] == PARAMS["N"] for r in fid_runs)
    fid_ok = bool(fid) and fid_max <= FIDELITY_TOL and full_horizon
    print("mirror fidelity (model strategy vs the will the C arbiter played): "
          "worst %.2e over %d windows -> %s (tol %.0e)"
          % (fid_max, len(fid), "OK" if fid_ok else "DRIFT", FIDELITY_TOL))
    verdict = verdict_of(rows)
    print("VERDICT: " + verdict)
    red = [r for r in results if r["rc"] != 0 or r.get("violations", 1) != 0] + \
          [r for r in fid_runs if r["rc"] != 0]
    with open(os.path.join(args.out, "results.json"), "w", encoding="utf-8") as fh:
        json.dump({"results": results, "fidelity_runs": fid_runs,
                   "ranking": rows,
                   "fidelity_max": fid_max, "verdict": verdict}, fh, indent=1)
    write_chart(args.out, rows)
    sc.write_text(os.path.join(args.out, "RESULTS.md"),
                  _results_md(table, seeds, fid_max, len(fid), verdict))
    return 1 if (red or not fid_ok) else 0


if __name__ == "__main__":
    sys.exit(main())
