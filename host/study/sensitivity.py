#!/usr/bin/env python3
"""Relais — parameter sensitivity study (M31, idea 68, spec 6.3).

Varies the five protocol parameters p, N, epsilon, delta and T, one at a time
and then on a reduced grid, and measures the effect on five published numbers:
  - the worst-case gap between fresh orders (REL-01/I4),
  - the worst deviation to the fault-free twin at the handover (I5),
  - the handover delay, from the last fresh order to the grant (MAN-04),
  - the certification rate: share of ticks with a certified second (REP-05),
  - the number of WRONGFUL handovers under frame losses, with no fault at all
    (ROB-03: losses shorter than p must never move the mandate).

Every point is a real run of build/relais-simulate judged by the C checker and
by build/relais-verify; the fixed seed makes the whole table reproducible.
The output is a sensitivity table (which parameter matters most), one tornado
chart, per-parameter charts, and a recommended parameter set per use case
(normal driving, emergency manoeuvre, degraded channel).

Usage:
  python3 host/study/sensitivity.py [--out runs/m31-sensitivity]
Exit code: 0 when no run violated an invariant, 1 otherwise.
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

import study_common as sc  # noqa: E402

SEED = 6868
TICKS = 2000
DEATH = "500:death:0"
LOSS = 0.10          # frame loss probability of the wrongful-handover runs
BOOT_GRACE = 10      # ticks: the initial grant is legitimate, not wrongful
TRAJ = {"kind": "slalom", "amp": 0.6, "freq": 0.5}
BASE = {"T": 5, "p": 1, "N": 8, "eps": 0.005, "delta": 0.02}

SWEEPS = {
    "p": [0, 1, 2, 3],
    "N": [6, 7, 8],
    "eps": [0.001, 0.002, 0.005, 0.01, 0.02, 0.05],
    "delta": [0.005, 0.01, 0.02, 0.05, 0.1, 0.2],
    "T": [1, 2, 5, 10, 20, 50],
}
GRID_PT = [(p, T) for p in (0, 1, 2, 3) for T in (2, 5, 20)]
GRID_ED = [(e, d) for e in (0.002, 0.005, 0.02) for d in (0.005, 0.02, 0.1)]


def wrongful_handovers(journal) -> int:
    """Mandate moves with no fault injected: each one is a wrongful handover."""
    return sum(1 for e in journal.events
               if e.event in ("mandate_granted", "mandate_auto_switch")
               and e.tick > BOOT_GRACE)


def measure(out_root: str, tag: str, params: dict, scenario: str) -> dict:
    """Run one configuration; return its published numbers."""
    out_dir = os.path.join(out_root, tag)
    faults = [DEATH] if scenario != "loss" else []
    loss = LOSS if scenario == "loss" else 0.0
    level = "socle" if scenario == "death_socle" else "complet"
    rc_sim = sc.sim_run(out_dir, TICKS, SEED, params=params, traj=TRAJ,
                        faults=faults, level=level, loss=loss,
                        scenario=tag, scenario_id=68)
    rc_verify, _ = sc.verify_run(out_dir, level)
    ana, journal, _twin = sc.load_and_analyze(out_dir, level)
    return {"tag": tag, "scenario": scenario, "params": dict(params),
            "max_gap": ana.max_gap, "max_dev": ana.max_deviation,
            "handover_delay": ana.max_handover_delay,
            "handover_delay_ms": ana.max_handover_delay * float(params["T"]),
            "cert_rate": round(sc.certification_rate(journal), 4),
            "wrongful": wrongful_handovers(journal),
            "violations": ana.violation_count,
            "rc": max(rc_sim, rc_verify)}


def measure_loss_world(tag: str, params: dict, seed: int) -> dict:
    """Wrongful handovers under the CORRECTED loss model (idea 68, ROB-03).

    The C simulator's burst guard (host/simulate/sim.c sim_lose) never resets
    lost_last, so --loss drops at most one frame per link per run and the
    claim "a burst shorter than p never moves the mandate" is not actually
    exercised by it. This measurement runs the same protocol through the REAL
    C arbiter (difftest bridge) inside host/physics/dataset_world.py, whose
    loss model drops each frame with probability LOSS and never more than
    max(p, 1) consecutive frames of one link. The defect is described in the
    M31 report (rule 10), not fixed here.
    """
    import dataset_world as dw
    import plant_law as pl
    dt = float(params["T"]) / 1000.0
    sp = lambda t: 0.6 * math.sin(2 * math.pi * 0.5 * (t - 1) * dt)
    dist = lambda t: pl.traj_disturbance(0.05, 2.0, 7, t * dt)
    world = dw.DatasetWorld(params=params, complete=True, loss=LOSS, seed=seed)
    rows = world.run(TICKS, sp, dist)
    world.close()
    wrongful = sum(1 for r in rows
                   if r.get("kind") == "event"
                   and r.get("event") in ("mandate_granted", "mandate_auto_switch")
                   and r.get("tick", 0) > BOOT_GRACE)
    violations = sum(1 for r in rows if r.get("kind") == "tick"
                     and r.get("source") == "none")
    return {"tag": tag, "params": dict(params), "seed": seed,
            "wrongful_py": wrongful, "dropped": world.dropped_frames,
            "violations_py": violations}


def sweep_all(out_root: str) -> list:
    """One-at-a-time sweeps + the two reduced grids; returns every row."""
    rows = []
    for scenario in ("death_socle", "death_complet", "loss"):
        for pname, values in SWEEPS.items():
            for v in values:
                params = dict(BASE)
                params[pname] = v
                if pname == "p" and v == 0:
                    params["N"] = 8  # N >= p+5 still holds
                rows.append(measure(out_root, "%s-%s-%s" % (scenario, pname, v),
                                    params, scenario))
    for p, T in GRID_PT:
        params = dict(BASE, p=p, T=T)
        rows.append(measure(out_root, "grid-loss-p%d-T%d" % (p, T), params, "loss"))
    for e, d in GRID_ED:
        params = dict(BASE, eps=e, delta=d)
        rows.append(measure(out_root, "grid-deathC-eps%g-del%g" % (e, d),
                            params, "death_complet"))
    return rows


def loss_world_sweep() -> list:
    """The wrongful-handover metric, measured through the real C arbiter with
    the corrected loss model, along every one-at-a-time sweep."""
    rows = []
    for pname, values in SWEEPS.items():
        for v in values:
            params = dict(BASE)
            params[pname] = v
            row = measure_loss_world("pyloss-%s-%s" % (pname, v), params, SEED)
            row["scenario"] = "pyloss"
            rows.append(row)
            print("  pyloss %-8s=%-6s wrongful=%d dropped=%d"
                  % (pname, v, row["wrongful_py"], row["dropped"]))
    extra = measure_loss_world("pyloss-p-1-seed2", dict(BASE), 7777)
    extra["scenario"] = "pyloss-seed-check"
    rows.append(extra)
    return rows


METRICS = ("max_gap", "max_dev", "handover_delay_ms", "cert_rate", "wrongful_py")
METRIC_LABELS = {"max_gap": "worst gap (ticks)", "max_dev": "twin deviation",
                 "handover_delay_ms": "handover delay (ms)",
                 "cert_rate": "certification rate",
                 "wrongful_py": "wrongful handovers (loss %.0f%%)" % (100 * LOSS)}
# Which scenario family feeds which metric of the tornado chart.
METRIC_SCENARIO = {"max_gap": "death_socle", "max_dev": "death_complet",
                   "handover_delay_ms": "death_socle",
                   "cert_rate": "death_complet", "wrongful_py": "pyloss"}


def sweep_series(rows: list, scenario: str, pname: str, metric: str):
    """(xs, ys) of one metric along one one-at-a-time sweep.

    The tag of a sweep row is exactly "<scenario>-<param>-<value>", so the
    selection is unambiguous; grid rows have their own tags and stay out.
    """
    seen = {}
    for r in rows:
        if r["scenario"] != scenario:
            continue
        prefix = "%s-%s-" % (scenario, pname)
        if r["tag"].startswith(prefix):
            try:
                seen[float(r["tag"][len(prefix):])] = r[metric]
            except ValueError:
                pass
    xs = sorted(seen)
    return xs, [seen[x] for x in xs]


def tornado(rows: list) -> list:
    """Normalized influence of each parameter on each metric (range / base)."""
    out = []
    for pname in SWEEPS:
        entry = {"param": pname}
        for metric in METRICS:
            scen = METRIC_SCENARIO[metric]
            xs, ys = sweep_series(rows, scen, pname, metric)
            base_val = BASE.get(pname)
            base = ys[xs.index(base_val)] if base_val in xs else (
                ys[0] if ys else 0.0)
            spread = (max(ys) - min(ys)) if ys else 0.0
            entry[metric] = {"range": spread, "base": base,
                             "norm": abs(spread / base) if base else spread}
        out.append(entry)
    return out


CHART_COLORS = ("#1565c0", "#c62828", "#2e7d32", "#f9a825", "#6a1b9a")

SWEEP_PLOTS = (("death_socle", "handover_delay_ms"), ("death_socle", "max_gap"),
               ("death_complet", "max_dev"), ("death_complet", "cert_rate"),
               ("pyloss", "wrongful_py"))


def write_charts(out_root: str, rows: list, torn: list) -> None:
    """One tornado chart + one chart per parameter (its five metric curves)."""
    labels = [e["param"] for e in torn]
    series = [(METRIC_LABELS[m], [e[m]["norm"] for e in torn], c)
              for m, c in zip(METRICS, CHART_COLORS)]
    sc.write_text(os.path.join(out_root, "tornado.svg"),
                  sc.bar_chart_svg(labels, series,
                                   "normalized influence (range / base value)",
                                   "Which parameter matters most", height=340))
    for pname, values in SWEEPS.items():
        series = []
        for si, (scen, metric) in enumerate(SWEEP_PLOTS):
            xs, ys = sweep_series(rows, scen, pname, metric)
            if len(xs) == len(values):
                series.append(("%s/%s" % (scen, metric), ys,
                               CHART_COLORS[si % len(CHART_COLORS)]))
        if not series:
            continue
        sc.write_text(os.path.join(out_root, "sweep-%s.svg" % pname),
                      sc.line_chart_svg([float(v) for v in values], series,
                                        pname, "metric value",
                                        "Sensitivity of the metrics to %s" % pname,
                                        height=320))


def recommend(rows: list) -> dict:
    """Data-driven parameter sets per use case (the mission's deliverable).

    - normal driving: the defaults, unless the sweep shows a strictly better
      handover delay at the same safety;
    - emergency manoeuvre: the fastest period T whose death-run stays green
      (smallest handover delay and gap), everything else default;
    - degraded channel: the smallest patience p with ZERO wrongful handover
      under the loss model, and the testament length the consistency rule
      allows (N >= p + 5).
    """
    green = [r for r in rows if r.get("rc", 0) == 0
             and r.get("violations", 0) == 0]

    def best(scenario, metric, param, pick=min):
        cand = [r for r in green if r["scenario"] == scenario
                and r["tag"].startswith("%s-%s-" % (scenario, param))]
        if not cand:
            return None
        return pick(cand, key=lambda r: (r[metric], r["params"][param]))

    fastest = best("death_socle", "handover_delay_ms", "T")
    pat = [r for r in rows if r.get("scenario") == "pyloss"
           and r["tag"].startswith("pyloss-p-")
           and r.get("violations_py", 0) == 0]
    patient = [r for r in pat if r["wrongful_py"] == 0]
    if patient:
        p_deg = min(r["params"]["p"] for r in patient)
    elif pat:
        # No patience makes the lossy channel handover-free: take the p with
        # the fewest wrongful handovers (ties -> the larger p, more margin).
        p_deg = max(pat, key=lambda r: (-r["wrongful_py"], r["params"]["p"]))["params"]["p"]
    else:
        p_deg = BASE["p"]
    return {
        "normal_driving": dict(BASE),
        "emergency_manoeuvre": dict(BASE, T=fastest["params"]["T"] if fastest
                                    else BASE["T"]),
        "degraded_channel": dict(BASE, p=int(p_deg),
                                 N=int(max(BASE["N"], int(p_deg) + 5))),
    }


def format_table(rows: list) -> str:
    out = ["| tag | scenario | params | gap | dev | delay ticks/ms | cert | "
           "wrongful C-sim | viol |\n",
           "|---|---|---|---|---|---|---|---|---|\n"]
    for r in rows:
        p = r["params"]
        out.append("| %s | %s | T=%s,p=%s,N=%s,eps=%s,delta=%s | %d | %.5f | "
                   "%d / %.0f | %.3f | %d | %d |\n"
                   % (r["tag"], r["scenario"], p["T"], p["p"], p["N"],
                      p["eps"], p["delta"], r["max_gap"], r["max_dev"],
                      r["handover_delay"], r["handover_delay_ms"],
                      r["cert_rate"], r["wrongful"], r["violations"]))
    return "".join(out)


def format_pyloss_table(rows: list) -> str:
    out = ["| params | seed | frames dropped | wrongful handovers | source-none |\n",
           "|---|---|---|---|---|\n"]
    for r in rows:
        if not str(r.get("scenario", "")).startswith("pyloss"):
            continue
        p = r["params"]
        out.append("| T=%s,p=%s,N=%s,eps=%s,delta=%s | %d | %d | %d | %d |\n"
                   % (p["T"], p["p"], p["N"], p["eps"], p["delta"], r["seed"],
                      r["dropped"], r["wrongful_py"], r["violations_py"]))
    return "".join(out)


SIM_LOSE_DEFECT = (
    "DEFECT FOUND (rule 10, NOT fixed here, out of M31 scope): "
    "`host/simulate/sim.c`, function `sim_lose` (lines 36-41). The burst guard "
    "sets `s->lost_last[link] = true` when a frame is dropped and NEVER resets "
    "it, so after the first drop a link can never lose another frame: with "
    "`--loss X` a whole run drops at most one frame per link, whatever X and "
    "the duration. Expected (comment + ROB-03): reset the flag when a frame "
    "passes, so losses keep happening with probability X and only CONSECUTIVE "
    "bursts longer than p are excluded. Consequence: the C simulator's loss "
    "runs cannot exercise 'a burst of p losses never moves the mandate'; the "
    "'wrongful C-sim' column above is therefore near zero by construction. "
    "The authoritative wrongful-handover numbers of this study come from the "
    "corrected loss model of host/physics/dataset_world.py, which drives the "
    "REAL C arbiter through host/difftest/bridge.c (table below).")


def _results_md(table: str, pytable: str, rec: dict) -> str:
    """The RESULTS.md body of the sensitivity study."""
    return ("# M31 sensitivity study (idea 68)\n\n"
            "Replay one point:\n\n"
            "    ./build/relais-simulate --ticks %d --seed %d "
            "--params '<T=..,p=..,N=..,eps=..,delta=..>' \\\n"
            "      --trajectory 'kind=slalom amp=0.6 freq=0.5' "
            "[--fault %s | --loss %.2f] [--level complet] \\\n"
            "      --journal <dir>/actuator.jsonl "
            "--twin-journal <dir>/twin.jsonl\n\n" % (TICKS, SEED, DEATH, LOSS)
            + table + "\nColumn note: in death runs the 'wrongful C-sim' "
            "cell counts the LEGITIMATE handover caused by the injected "
            "death (always 1); the wrongful-handover metric lives in the "
            "corrected-loss table below.\n\n" + SIM_LOSE_DEFECT
            + "\n\n" + pytable + "\n## Recommendations\n\n"
            + "".join("- **%s**: `%s`\n" % (k.replace("_", " "),
                                            sc.params_string(v))
                      for k, v in rec.items())
            + "\nRegenerate: `python3 host/study/sensitivity.py` "
              "(fixed seed %d).\n" % SEED)


def main() -> int:
    ap = argparse.ArgumentParser(description="M31 sensitivity study (idea 68)")
    ap.add_argument("--out", default=os.path.join(ROOT, "runs", "m31-sensitivity"))
    args = ap.parse_args()
    sc.require_binaries()
    os.makedirs(args.out, exist_ok=True)
    rows = sweep_all(args.out)
    pyrows = loss_world_sweep()
    all_rows = rows + pyrows
    torn = tornado(all_rows)
    write_charts(args.out, all_rows, torn)
    table = format_table(rows)
    pytable = format_pyloss_table(pyrows)
    print(table)
    print("Wrongful handovers under the CORRECTED loss model (real C arbiter):")
    print(pytable)
    print("Normalized influence (range/base) per parameter:")
    for e in torn:
        print("  %-6s %s" % (e["param"], " ".join(
            "%s=%.3g" % (m, e[m]["norm"]) for m in METRICS)))
    rec = recommend(all_rows)
    print("Recommended parameter sets:")
    for use, params in rec.items():
        print("  %-20s %s" % (use, sc.params_string(params)))
    red = [r for r in rows if r["rc"] != 0] + \
          [r for r in pyrows if r["violations_py"] > 0]
    with open(os.path.join(args.out, "results.json"), "w", encoding="utf-8") as fh:
        json.dump({"rows": rows, "pyloss": pyrows, "tornado": torn,
                   "recommend": rec}, fh, indent=1)
    sc.write_text(os.path.join(args.out, "RESULTS.md"),
                  _results_md(table, pytable, rec))
    print("VERDICT: %d C runs + %d loss-world runs, %d red"
          % (len(rows), len(pyrows), len(red)))
    return 1 if red else 0


if __name__ == "__main__":
    sys.exit(main())
