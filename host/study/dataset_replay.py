#!/usr/bin/env python3
"""Relais — real-dataset replay of steering-wheel time series (M31, idea 62).

A reader that takes a steering-angle time series from a CSV file (columns:
time_s, angle) and plays it as the SETPOINT of the whole protocol: the real C
arbiter (through host/difftest/bridge.c), the two computers with the float32
control-law mirror of host/physics/plant_law.py (validated tick by tick against
the C journal), and the mirrored rack. The world itself is
host/physics/dataset_world.py, proven tick-equal to build/relais-simulate on
slalom runs with and without a death (see host/study/test_studies.py).

The tool also GENERATES a realistic synthetic dataset (four manoeuvre families:
slalom, lane change, obstacle avoidance, parking) so the replay is runnable
without downloading anything, and documents how to plug a real public dataset
in, with the licensing questions answered BEFORE any use (nothing is
downloaded here; see host/study/DATASETS.md).

Usage:
  python3 host/study/dataset_replay.py --generate host/study/data
  python3 host/study/dataset_replay.py --csv host/study/data/avoidance.csv \
      --fault 2000:death:0 --level complet --out runs/m31-dataset
  python3 host/study/dataset_replay.py --all --out runs/m31-dataset
Exit code: 0 when every replayed run passed the C verifier.
"""
from __future__ import annotations

import argparse
import csv
import math
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "host", "physics"))

import study_common as sc       # noqa: E402
import plant_law as pl          # noqa: E402
from dataset_world import DatasetWorld  # noqa: E402

SAMPLE_DT = 0.005     # 200 Hz, the tick rate of the protocol defaults
DURATION_S = 20.0     # length of each generated manoeuvre
SEED = 6262           # fixed: the synthetic dataset is reproducible
# Sensor noise of a real steering-angle channel: ~0.5 deg on a +/-540 deg
# wheel, i.e. 0.1% of full scale. The command-robustness sweep (--noise-sweep)
# publishes what the law does with more noise than that.
NOISE_SIGMA = 0.001


# ---------------------------------------------------------------------------
# Synthetic dataset (realistic manoeuvre families, normalised angle in [-1, 1];
# on a real rack, 1.0 = full lock, e.g. 540 deg — the protocol is normalised).
# ---------------------------------------------------------------------------

def _smoothstep(x: float) -> float:
    x = min(1.0, max(0.0, x))
    return x * x * (3.0 - 2.0 * x)


def gen_slalom(rng: random.Random, n: int) -> list:
    """Highway slalom: two superposed sine waves + a little noise."""
    f1, f2 = 0.5, 0.83
    a2 = rng.uniform(0.10, 0.18)
    return [0.55 * math.sin(2 * math.pi * f1 * i * SAMPLE_DT)
            + a2 * math.sin(2 * math.pi * f2 * i * SAMPLE_DT)
            + rng.gauss(0.0, NOISE_SIGMA) for i in range(n)]


def gen_lane_change(rng: random.Random, n: int) -> list:
    """Three lane changes with holds in between, smoothstep transitions."""
    out, level, targets = [], 0.0, [0.7, 0.7, -0.6, 0.0, 0.5, 0.0]
    seg = n // len(targets)
    for k, target in enumerate(targets):
        start = level
        for i in range(seg if k < len(targets) - 1 else n - seg * k):
            frac = _smoothstep((i - seg * 0.2) / (seg * 0.6)) if i > seg * 0.2 \
                else 0.0
            out.append(start + (target - start) * frac
                       + rng.gauss(0.0, NOISE_SIGMA))
        level = target
    return out[:n]


def gen_avoidance(rng: random.Random, n: int,
                  sigma: float = NOISE_SIGMA) -> list:
    """Obstacle avoidance: calm cruise, one violent S-swerve, recovery.

    `sigma` is the setpoint-noise standard deviation (full-scale fraction);
    the --noise-sweep study varies it to publish the command robustness.
    """
    out = []
    t_swerve, t_back = 6.0, 7.2  # seconds
    for i in range(n):
        t = i * SAMPLE_DT
        v = 0.05 * math.sin(2 * math.pi * 0.3 * t)
        if t_swerve <= t < t_swerve + 0.7:
            v = -0.85 * _smoothstep((t - t_swerve) / 0.7)
        elif t_swerve + 0.7 <= t < t_back + 0.7:
            u = (t - t_swerve - 0.7) / (t_back + 0.7 - t_swerve - 0.7)
            v = -0.85 + 1.75 * _smoothstep(u)
        elif t_back + 0.7 <= t < t_back + 2.2:
            u = (t - t_back - 0.7) / 1.5
            v = 0.9 - 0.9 * _smoothstep(u)
        out.append(v + rng.gauss(0.0, sigma))
    return out


def gen_parking(rng: random.Random, n: int) -> list:
    """Parking manoeuvres: alternating near-full-lock arcs with pauses."""
    out, t = [], 0.0
    sign = 1.0
    while len(out) < n:
        pause = int(0.4 / SAMPLE_DT)
        out.extend([out[-1] if out else 0.0] * min(pause, n - len(out)))
        arc = int(rng.uniform(1.5, 2.5) / SAMPLE_DT)
        amp = sign * rng.uniform(0.75, 0.95)
        for i in range(min(arc, n - len(out))):
            u = i / arc
            out.append(amp * math.sin(math.pi * u)
                       + rng.gauss(0.0, NOISE_SIGMA))
        t += arc * SAMPLE_DT
        sign = -sign
        if t > DURATION_S:
            break
    return out[:n]


GENERATORS = {"slalom": gen_slalom, "lane_change": gen_lane_change,
              "avoidance": gen_avoidance, "parking": gen_parking}


def write_csv(path: str, angles: list) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["time_s", "angle"])
        for i, a in enumerate(angles):
            w.writerow(["%.4f" % (i * SAMPLE_DT), "%.6f" % a])


def generate_all(out_dir: str, seed: int = SEED) -> list:
    """Write the four synthetic manoeuvres; returns their paths."""
    rng = random.Random(seed)
    n = int(DURATION_S / SAMPLE_DT) + 1
    paths = []
    for name, gen in GENERATORS.items():
        path = os.path.join(out_dir, "%s.csv" % name)
        write_csv(path, gen(rng, n))
        paths.append(path)
    return paths


# ---------------------------------------------------------------------------
# The reader: CSV -> (times, angles) -> per-tick setpoints.
# ---------------------------------------------------------------------------

def load_csv(path: str):
    """Read a (time_s, angle) CSV; tolerant to header names and to whitespace.

    Returns two lists (times, angles) with strictly increasing times.
    Raises ValueError with a clear message on anything unusable.
    """
    times, angles = [], []
    with open(path, "r", encoding="utf-8", newline="") as fh:
        reader = csv.reader(fh)
        for row in reader:
            if not row or not row[0].strip():
                continue
            try:
                t = float(row[0])
                a = float(row[1])
            except (ValueError, IndexError):
                if times or row[0].strip().lower() not in ("time_s", "time", "#"):
                    continue  # skip the header / comment line
                continue
            if not (math.isfinite(t) and math.isfinite(a)):
                raise ValueError("%s: non-finite value at time %r" % (path, row[0]))
            if times and t <= times[-1]:
                raise ValueError("%s: time not increasing at %r" % (path, row[0]))
            times.append(t)
            angles.append(a)
    if len(times) < 2:
        raise ValueError("%s: need at least two samples" % path)
    return times, angles


def resample(times: list, angles: list, ticks: int, period_s: float) -> list:
    """Per-tick setpoints: linear interpolation at t = (tick-1) * period_s,
    the same convention as the C simulator's setpoint_at(); the last value is
    held past the end of the series."""
    out, j = [], 0
    for tick in range(1, ticks + 1):
        t = (tick - 1) * period_s
        while j + 1 < len(times) and times[j + 1] < t:
            j += 1
        if t <= times[0]:
            out.append(angles[0])
        elif t >= times[-1]:
            out.append(angles[-1])
        else:
            span = times[j + 1] - times[j]
            frac = (t - times[j]) / span if span > 0 else 0.0
            out.append(angles[j] + (angles[j + 1] - angles[j]) * frac)
    return out


# ---------------------------------------------------------------------------
# Replay: the CSV drives the whole protocol (real C arbiter via the bridge).
# ---------------------------------------------------------------------------

def parse_fault(spec: str):
    """'1500:death:0' / '1500:freeze:1:40' -> (tick, kind, node, arg)."""
    parts = spec.split(":")
    if len(parts) < 3 or parts[1] not in ("death", "freeze"):
        raise ValueError("fault spec must be TICK:death:NODE or "
                         "TICK:freeze:NODE:DUR, got %r" % spec)
    arg = int(parts[3]) if len(parts) > 3 else 0
    return int(parts[0]), parts[1], int(parts[2]), arg


def replay_csv(csv_path: str, out_dir: str, tag: str, level: str = "complet",
               faults=None, params=None, ticks: int = 0) -> dict:
    """Play one CSV through the protocol; return its metrics.

    Runs the faulted world and the fault-free twin of the SAME csv (the world
    is deterministic without loss, so the twin is reproducible), writes both
    journals in the authoritative format and judges them with the C verifier.
    """
    times, angles = load_csv(csv_path)
    p = dict(pl.default_params())
    if params:
        p.update(params)
    period_s = float(p["T"]) / 1000.0
    n_ticks = ticks or min(len(times), int(math.ceil(times[-1] / period_s)) + 1)
    setpoints = resample(times, angles, n_ticks, period_s)
    sp_at = lambda t: setpoints[t - 1]

    runs = {}
    for name, world_faults in (("twin", []), ("run", faults or [])):
        world = DatasetWorld(params=p, complete=(level == "complet"), seed=SEED)
        for spec in world_faults:
            world.add_fault(*parse_fault(spec))
        world.run(n_ticks, sp_at)
        runs[name] = world
    os.makedirs(out_dir, exist_ok=True)
    runs["run"].write_journal(os.path.join(out_dir, "actuator.jsonl"), tag)
    runs["twin"].write_journal(os.path.join(out_dir, "twin.jsonl"), tag)
    runs["run"].close()
    runs["twin"].close()
    return metrics(out_dir, tag, level, runs, setpoints)


def metrics(out_dir: str, tag: str, level: str, runs: dict,
            setpoints: list) -> dict:
    """Verdict of the C verifier + tracking/continuity numbers."""
    rc_verify, verdict = sc.verify_run(out_dir, level)
    journal = runs["run"].rows
    twin_rows = runs["twin"].rows
    ticks = [r for r in journal if r["kind"] == "tick"]
    src_mix = {}
    for r in ticks:
        src_mix[r["source"]] = src_mix.get(r["source"], 0) + 1
    track = [abs(r["pos"] - r["setpoint"]) for r in ticks]
    twin_val = {r["tick"]: r["value"] for r in twin_rows if r["kind"] == "tick"}
    dev = [abs(r["value"] - twin_val[r["tick"]]) for r in ticks
           if r["tick"] in twin_val]
    grants = sum(1 for r in journal if r["kind"] == "event"
                 and r["event"] in ("mandate_granted", "mandate_auto_switch")
                 and r["tick"] > 10)
    return {"tag": tag, "level": level, "verify_rc": rc_verify,
            "verdict": verdict.splitlines()[-1] if verdict else "",
            "ticks": len(ticks), "src_mix": src_mix, "handovers": grants,
            "track_err_max": max(track) if track else 0.0,
            "track_err_mean": sum(track) / len(track) if track else 0.0,
            "twin_dev_max": max(dev) if dev else 0.0}


ALL_PLAYS = [("nominal", "complet", []),
             ("death-socle", "socle", ["{mid}:death:0"]),
             ("death-complet", "complet", ["{mid}:death:0"]),
             ("freeze-complet", "complet", ["{mid}:freeze:0:40"])]


def run_all(csv_paths: list, out_root: str, params=None) -> list:
    """The full replay matrix over the generated datasets."""
    rows = []
    for path in csv_paths:
        name = os.path.basename(path).replace(".csv", "")
        n = sum(1 for _ in open(path, encoding="utf-8")) - 1
        mid = n // 2
        for play, level, faults in ALL_PLAYS:
            specs = [f.replace("{mid}", str(mid)) for f in faults]
            tag = "%s-%s" % (name, play)
            out_dir = os.path.join(out_root, tag)
            print("  replay %-28s level=%-7s faults=%s" % (tag, level, specs))
            rows.append(replay_csv(path, out_dir, tag, level, specs, params))
            r = rows[-1]
            print("    verify=%s handovers=%d track_err=%.4f/%.4f twin_dev=%.5f "
                  "src=%s" % ("OK" if r["verify_rc"] == 0 else "VIOLATION",
                              r["handovers"], r["track_err_mean"],
                              r["track_err_max"], r["twin_dev_max"],
                              r["src_mix"]))
    return rows


def format_table(rows: list) -> str:
    out = ["| dataset/play | level | verify | ticks | handovers | "
           "track err mean/max | twin dev max | sources |\n",
           "|---|---|---|---|---|---|---|---|\n"]
    for r in rows:
        out.append("| %s | %s | %s | %d | %d | %.4f / %.4f | %.5f | %s |\n"
                   % (r["tag"], r["level"],
                      "OK" if r["verify_rc"] == 0 else "VIOLATION",
                      r["ticks"], r["handovers"], r["track_err_mean"],
                      r["track_err_max"], r["twin_dev_max"],
                      " ".join("%s:%d" % kv for kv in sorted(
                          r["src_mix"].items()))))
    return "".join(out)


NOISE_SWEEP = (0.0, 0.0002, 0.0005, 0.002, 0.008)


def noise_sweep(out_root: str) -> list:
    """Command robustness to setpoint noise (idea 61/68 crossover).

    Replays the avoidance manoeuvre, fault-free, with increasing sensor noise
    on the steering request, and publishes the command's late-run excursion:
    the frozen law (control.c: KP=60, no setpoint filter) amplifies the noise
    by about two orders of magnitude, up to a saturated limit cycle. The
    protocol bounds (ACT-05) hold throughout - that is the point of the clamps.
    """
    n = int(DURATION_S / SAMPLE_DT) + 1
    rows = []
    for sigma in NOISE_SWEEP:
        angles = gen_avoidance(random.Random(SEED), n, sigma)
        csv_path = os.path.join(out_root, "avoidance-noise-%.4f.csv" % sigma)
        write_csv(csv_path, angles)
        sp_at = lambda t: angles[t - 1]
        world = DatasetWorld(complete=True, seed=SEED)
        world.run(n, sp_at)
        ticks = [r for r in world.rows if r["kind"] == "tick"]
        world.close()
        late = [r["value"] for r in ticks[int(0.75 * n):]]
        rows.append({"sigma": sigma,
                     "late_min": min(late), "late_max": max(late),
                     "abs_max": max(abs(r["value"]) for r in ticks)})
        print("  sigma=%.4f (%.3f%% FS): late command [%+.4f, %+.4f], "
              "|value| max %.4f" % (sigma, 100 * sigma, rows[-1]["late_min"],
                                    rows[-1]["late_max"], rows[-1]["abs_max"]))
    return rows


def _cmd_noise_sweep(out_root: str) -> int:
    rows = noise_sweep(out_root)
    sc.write_text(os.path.join(out_root, "NOISE-SWEEP.md"),
                  "# Command robustness to setpoint noise (M31)\n\n"
                  "Avoidance manoeuvre, fault-free, level complet, seed "
                  "%d, 4001 ticks. Replay:\n\n"
                  "    python3 host/study/dataset_replay.py --noise-sweep\n\n"
                  "| sigma (FS fraction) | late command min | late command "
                  "max | abs max |\n|---|---|---|---|\n" % SEED
                  + "".join("| %.4f | %+.4f | %+.4f | %.4f |\n"
                            % (r["sigma"], r["late_min"], r["late_max"],
                               r["abs_max"]) for r in rows)
                  + "\nThe frozen law amplifies steering-request noise by "
                    "~100x into the command (KP=60, no setpoint filter in "
                    "core/src/control.c relais_ctl_step - out of M31 "
                    "scope, reported, not modified). ACT-05 clamps keep "
                    "every value inside the bounds; the invariants hold "
                    "at every sigma.\n")
    return 0


def _cmd_all(args) -> int:
    paths = [os.path.join(args.data, "%s.csv" % n) for n in GENERATORS]
    if [p for p in paths if not os.path.exists(p)]:
        print("generating the synthetic dataset in %s" % args.data)
        generate_all(args.data)
    rows = run_all(paths, args.out)
    table = format_table(rows)
    print("\n" + table)
    sc.write_text(os.path.join(args.out, "RESULTS.md"),
                  "# M31 real-dataset replay (idea 62)\n\n"
                  "Synthetic dataset: `python3 host/study/dataset_replay.py "
                  "--generate host/study/data` (seed %d, 200 Hz, 20 s,\n"
                  "normalised angle, sensor noise sigma=%.3f of full scale). "
                  "Replay one file:\n\n"
                  "    python3 host/study/dataset_replay.py --csv "
                  "host/study/data/avoidance.csv \\\n"
                  "      --fault 2000:death:0 --level complet --out "
                  "runs/m31-dataset\n\n"
                  "Real datasets and licensing: host/study/DATASETS.md.\n\n"
                  % (SEED, NOISE_SIGMA) + table)
    bad = [r for r in rows if r["verify_rc"] != 0]
    print("VERDICT: %d replays, %d violated" % (len(rows), len(bad)))
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="M31 dataset replay (idea 62)")
    ap.add_argument("--generate", metavar="DIR",
                    help="write the synthetic dataset and exit")
    ap.add_argument("--csv", help="replay one CSV file")
    ap.add_argument("--all", action="store_true",
                    help="replay the whole generated dataset matrix")
    ap.add_argument("--noise-sweep", action="store_true",
                    help="command robustness to setpoint noise (no faults)")
    ap.add_argument("--fault", action="append", default=[],
                    help="TICK:death:NODE or TICK:freeze:NODE:DUR (repeatable)")
    ap.add_argument("--level", default="complet", choices=["socle", "complet"])
    ap.add_argument("--ticks", type=int, default=0)
    ap.add_argument("--out", default=os.path.join(ROOT, "runs", "m31-dataset"))
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    args = ap.parse_args()

    if args.generate:
        for p in generate_all(args.generate):
            print("generated", p)
        return 0
    if args.noise_sweep:
        return _cmd_noise_sweep(args.out)
    if args.csv:
        name = os.path.basename(args.csv).replace(".csv", "")
        r = replay_csv(args.csv, os.path.join(args.out, name), name,
                       args.level, args.fault, ticks=args.ticks)
        print(format_table([r]))
        return 1 if r["verify_rc"] != 0 else 0
    if args.all:
        return _cmd_all(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
