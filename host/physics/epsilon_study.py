#!/usr/bin/env python3
"""Relais — sensor noise and the choice of epsilon (mission M30, idea 67).

The actuator certifies a second by comparing its shadow with the holder's
order inside a band of `epsilon` (REP-05), and it declares a disagreement when
the gap stays above that band for three ticks (DET-09). Epsilon is therefore
both a tolerance and a detection threshold:

  * too small, and the noise of the measurements alone breaks the certification;
  * too large, and a value fault (INJ-10) hides inside the band.

The published value is epsilon = 0.005 (0.5 % of full scale). This study
replaces that round number with a measurement. It sweeps the sensor noise and
the band over the REAL core -- build/relais-physics drives the arbiter, two
node state machines, the rack and the published control law -- and answers:

  (a) which epsilon still certifies the second at a given noise level;
  (b) which epsilon still detects a value fault, and how fast;
  (c) what the architecture has to do to keep both.

Two sensor architectures are measured, because they do not behave alike:
  independent  each computer reads its own position sensor, so the two noise
               draws are different and they show up in the comparison;
  shared       the actuator publishes one measurement that both computers use,
               which is what the published software does, so the noise moves
               the command but never the comparison.

Outputs (all written under host/physics/):
  epsilon_study.csv         every measured point, one CSV row each;
  EPSILON-STUDY.md          the tables, the curve and the recommendation;
  epsilon-certification.svg certification availability against the noise.

Usage: python3 host/physics/epsilon_study.py [--quick] [--seeds N] [--ticks N]
                                           [--workers N] [--mode all|epsilon|phys]
Standard library only. Exit code 0 when the study completed.
"""
import argparse
import concurrent.futures
import csv
import io
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from study_report import (  # noqa: E402  (same directory, standard library only)
    CERTIFIED_TARGET, build_report, build_report_tail, epsilon_floor, fmt_sigma,
    svg_curve, table_certification, table_detection, table_floor)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BENCH = os.path.join(ROOT, "build", "relais-physics")
OUT_DIR = os.path.join(ROOT, "host", "physics")

SIGMAS = (0.0, 1e-5, 3e-5, 1e-4, 3e-4, 1e-3, 3e-3)
EPSILONS = (0.001, 0.002, 0.005, 0.01, 0.02, 0.05)
BIASES = (0.001, 0.002, 0.01, 0.02, 0.05)  # the two smallest are below the
                                             # published band: they must stay
                                             # invisible, and that is measured
NOISES = ("independent", "shared")
TRAJECTORIES = ("", "kind=step amp=0.6 t_start=1.0")


def build_bench():
    """Build the bench if needed; the Makefile owns the compile line."""
    if os.path.exists(BENCH):
        return True
    proc = subprocess.run(["make", BENCH], cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write((proc.stdout + proc.stderr)[-2000:])
        return False
    return True


def bench_rows(args):
    """Run the bench once and parse its CSV output."""
    proc = subprocess.run([BENCH] + args + ["--header"], cwd=ROOT,
                          capture_output=True, text=True, timeout=600)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit("relais-physics failed: %s" % " ".join(args))
    return list(csv.DictReader(io.StringIO(proc.stdout)))


def epsilon_point(noise, sigma, epsilon, bias, seeds, ticks, bias_at):
    args = ["--mode", "epsilon", "--noise", noise, "--sigma", repr(sigma),
            "--epsilon", repr(epsilon), "--bias", repr(bias), "--bias-at", str(bias_at),
            "--seeds", str(seeds), "--ticks", str(ticks)]
    row = bench_rows(args)[0]
    for key in list(row):
        if key not in ("noise",):
            row[key] = float(row[key])
    return row


def sweep(points, workers):
    """Run every grid point, in parallel, keeping the order of the inputs."""
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(lambda a: epsilon_point(*a), points))


def pass_a(sigmas, epsilons, noises, seeds, ticks, workers):
    """No fault injected: the noise floor, the certification, the false alarms."""
    points = [(n, s, e, 0.0, seeds, ticks, 10 ** 9)
              for n in noises for s in sigmas for e in epsilons]
    return sweep(points, workers)


def pass_b(sigmas, epsilons, noises, biases, seeds, ticks, bias_at, workers):
    """A value fault is injected: how fast the actuator sees it."""
    points = [(n, s, e, b, seeds, ticks, bias_at)
              for n in noises for s in sigmas for e in epsilons for b in biases]
    return sweep(points, workers)


def phys_rows(ticks):
    rows = []
    for traj in TRAJECTORIES:
        args = ["--mode", "phys", "--ticks", str(ticks)]
        if traj:
            args += ["--trajectory", traj]
        rows += bench_rows(args)
    return rows


def index(rows, keys):
    out = {}
    for row in rows:
        out[tuple(row[k] for k in keys)] = row
    return out


def write_csv(path, rows):
    """One file per row shape: the epsilon points and the phys points do not
    carry the same columns, and a merged file would be full of holes."""
    if not rows:
        return
    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def main():
    ap = argparse.ArgumentParser(description="epsilon against sensor noise (mission M30)")
    ap.add_argument("--quick", action="store_true", help="a reduced grid")
    ap.add_argument("--seeds", type=int, default=8, help="runs per grid point")
    ap.add_argument("--ticks", type=int, default=3000, help="ticks per run")
    ap.add_argument("--bias-at", type=int, default=1000, help="tick of the value fault")
    ap.add_argument("--workers", type=int, default=8, help="parallel bench processes")
    ap.add_argument("--mode", default="all", choices=("all", "epsilon", "phys"))
    args = ap.parse_args()
    if not build_bench():
        return 1
    sigmas = tuple(round(s, 9) for s in ((0.0, 1e-4, 1e-3) if args.quick else SIGMAS))
    epsilons = tuple(round(e, 9) for e in
                     ((0.002, 0.005, 0.02) if args.quick else EPSILONS))
    biases = tuple(round(b, 9) for b in ((0.02,) if args.quick else BIASES))
    rows = []
    if args.mode in ("all", "phys"):
        rows += phys_rows(args.ticks)
        print("== faithful rack against the published one ==")
        for row in rows:
            print("  %(trajectory)-28s %(config)-10s max_err=%(max_err)s rms=%(rms_err)s "
                  "max_pos=%(max_pos)s" % row)
    a_rows, b_rows = [], []
    if args.mode in ("all", "epsilon"):
        a_rows = pass_a(sigmas, epsilons, NOISES, args.seeds, args.ticks, args.workers)
        b_rows = pass_b(sigmas, epsilons, NOISES, biases, args.seeds, args.ticks,
                        args.bias_at, args.workers)
        a_table = index(a_rows, ("noise", "sigma", "epsilon"))
        b_table = index(b_rows, ("noise", "sigma", "epsilon", "bias"))
        report = build_report(args, rows, a_table, b_table, sigmas, epsilons, biases)
        report += build_report_tail(args, a_table, b_table, rows, sigmas, epsilons, biases)
        text = "\n".join(report)
        with open(os.path.join(OUT_DIR, "EPSILON-STUDY.md"), "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
        svg_curve(a_table, sigmas, epsilons, "independent",
                  os.path.join(OUT_DIR, "epsilon-certification.svg"))
        print("== certification against noise, independent channels ==")
        print(table_certification(a_table, sigmas, epsilons, "independent",
                                  "certified_fraction"))
        print("== false alarms, independent channels ==")
        print(table_certification(a_table, sigmas, epsilons, "independent",
                                  "false_alarm_rate"))
        print("== smallest usable band ==")
        print(table_floor(a_table, "independent", sigmas, epsilons))
        print("== detection delay, bias 2 % of full scale, independent channels ==")
        print(table_detection(a_table, b_table, sigmas, epsilons, 0.02, "independent"))
        print("\nreport written to host/physics/EPSILON-STUDY.md")
    write_csv(os.path.join(OUT_DIR, "epsilon_study.csv"), a_rows + b_rows)
    write_csv(os.path.join(OUT_DIR, "phys_study.csv"), rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
