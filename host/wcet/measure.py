#!/usr/bin/env python3
"""measure.py — reproducible, portable measurement of the critical path (M40).

WHAT IS MEASURED. The in-process critical path of one machine, per tick:
"STATUT bytes received -> ORDRE bytes emitted" (decode + integrity check, the
protocol state machine, the control law, the encode + MAC of every frame out).
The timestamps come from clock_gettime(CLOCK_MONOTONIC) inside
host/wcet/probe.c, so the SAME harness runs unchanged on the PC, on a Raspberry
Pi and on the QNX 8.0 target: this file only drives the probe, aggregates and
publishes. The socket syscalls and the kernel stack are outside the window and
are covered by M02's socket-level campaign (host/measure/) and by
host/timing/procedures-qnx.md P1 on the target.

WHAT IS PUBLISHED. For every run and every segment: n, p50, p90, p99, p99.9,
max and the standard deviation, the dispersion between successive runs
(reproducibility), an extrapolated Gumbel-POT bound, and a cross-check of the C
summary against a recomputation by host/timing/wcet.py (one definition of the
percentiles, two implementations, they must agree).

This tool never invents a number: when the probe binary is absent it prints
BLOCKED(<reason>) with the exact build command for the platform and exits.

Usage (PC / Pi / QNX, standard library only):
  python3 host/wcet/measure.py --samples 20000 --runs 2 --cpu 3 --lock
  python3 host/wcet/measure.py --interference none,cpu,journal,net

Requirements: PERF-01, PERF-06, PERF-07, PERF-08, QNX-04, OBS-03.
"""
import argparse
import glob
import json
import os
import shlex
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import interference as INTERFERENCE  # noqa: E402
import wcet_lib as W  # noqa: E402

BUILD_HINT = """the WCET probe binary is missing. Build it, then re-run:
  PC / Raspberry Pi (Linux):  make wcet-probe
  QNX 8.0 target (SDP):
    source ~/qnx800/qnxsdp-env.sh
    cd target && qcc -V gcc_ntox86_64 -std=c11 -O2 -Wall -Wextra -Werror \\
      -D_POSIX_C_SOURCE=200809L -I../core/include -I../host/wcet \\
      ../host/wcet/probe.c ../host/wcet/wcet_stats.c ../core/src/*.c -lm \\
      -o relais-wcet-probe
  then:  python3 host/wcet/measure.py --probe target/relais-wcet-probe"""


def default_probe():
    return os.path.join(W.ROOT, "build", "relais-wcet-probe")


def prepare_env(outdir, cpu, enabled):
    """Run isolate.sh; return (env record, its printed table, status)."""
    script = os.path.join(HERE, "isolate.sh")
    shell = shutil.which("sh")
    if not enabled:
        return {"status": "SKIP(--no-isolate)"}, "", "skipped"
    if shell is None or not os.path.exists(script):
        return {"status": "BLOCKED(no sh or isolate.sh)"}, "", "blocked"
    os.makedirs(outdir, exist_ok=True)
    env_path = os.path.join(outdir, "env.json")
    cmd = [shell, script, "--json", env_path]
    if cpu is not None:
        cmd += ["--cpu", str(cpu)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    with open(os.path.join(outdir, "isolate.log"), "w", encoding="utf-8") as fh:
        fh.write(proc.stdout + proc.stderr)
    env = W.read_json(env_path) if os.path.exists(env_path) else {}
    env["isolate_rc"] = proc.returncode
    return env, proc.stdout, "ok"


def pinned_command(cmd, cpu):
    """Wrap cmd with isolate.sh: the pinning logic lives in exactly one file."""
    script = os.path.join(HERE, "isolate.sh")
    shell = shutil.which("sh")
    if cpu is None or shell is None or not os.path.exists(script):
        return list(cmd)
    return [shell, script, "--cpu", str(cpu), "--quiet", "--", shlex.join(cmd)]


def probe_command(args, probe, host, run_dir, tag):
    cmd = [probe, "--samples", str(args.samples), "--warmup", str(args.warmup),
           "--out", os.path.join(run_dir, "samples-%s.jsonl" % tag),
           "--summary", os.path.join(run_dir, "summary-%s.json" % tag),
           "--host", host, "--quiet"]
    if args.lock:
        cmd.append("--lock")
    if args.prio:
        cmd += ["--prio", str(args.prio)]
    if args.params:
        cmd += ["--params", args.params]
    if not args.mac:
        cmd.append("--no-mac")
    return pinned_command(cmd, args.cpu)


def loadavg():
    """Ambient load, recorded with every run: on a shared machine the tail of
    the distribution is mostly the neighbours, and that must be visible."""
    try:
        return [round(v, 2) for v in os.getloadavg()]
    except (OSError, AttributeError):
        return None


def run_once(args, probe, host, run_dir, tag, workers):
    """One measurement: interference up, probe run, interference down."""
    ctx = {"probe": probe, "run_dir": run_dir, "workers": workers,
           "seconds": args.burn}
    before = loadavg()
    pressure = INTERFERENCE.build(args_mode(tag), ctx).start()
    cmd = probe_command(args, probe, host, run_dir, tag)
    started = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=args.timeout)
    except subprocess.TimeoutExpired:
        pressure.stop()
        return {"tag": tag, "status": "TIMEOUT", "command": shlex.join(cmd),
                "loadavg_before": before, "interference": pressure.describe()}
    wall = time.monotonic() - started
    pressure.stop()
    return {"tag": tag, "status": "OK" if proc.returncode == 0 else "FAILED",
            "rc": proc.returncode, "command": shlex.join(cmd), "wall_s": wall,
            "loadavg_before": before, "loadavg_after": loadavg(),
            "throughput_s": args.samples / wall if wall > 0 else None,
            "stderr": (proc.stderr or "").strip()[:400],
            "jsonl": os.path.join(run_dir, "samples-%s.jsonl" % tag),
            "summary": os.path.join(run_dir, "summary-%s.json" % tag),
            "interference": pressure.describe()}


def args_mode(tag):
    """'cpu-run1' -> 'cpu': the mode is the prefix of the run tag."""
    return tag.split("-run")[0]


def analyse(run, tolerance, keep_samples=True):
    """Cross-check the C summary against host/timing/wcet.py, add the EVT bound."""
    if run["status"] != "OK":
        return run
    if not os.path.exists(run["summary"]) or not os.path.exists(run["jsonl"]):
        run["status"] = "FAILED"
        run["stderr"] = run.get("stderr", "") + " (no output file)"
        return run
    probe_summary = W.read_json(run["summary"])
    samples = W.load_samples(run["jsonl"])
    recomputed = W.describe(samples)
    agree, gap = W.cross_check(probe_summary["segments"]["path"], recomputed,
                               tolerance)
    evt = W.gumbel_pot_bound(samples, 0.99, 1e-6, 20)
    run.update({"probe_summary": probe_summary, "recomputed_path": recomputed,
                "agreement": agree, "gap_pct": gap, "samples_read": len(samples),
                "evt_bound_us": (evt or {}).get("bound"),
                "evt_ci": [evt["lo"], evt["hi"]] if evt else None})
    if not agree:
        run["status"] = "MISMATCH"
    if not keep_samples:
        # The raw JSONL is reproducible with the recorded command; on a machine
        # short of disk the aggregates and the C summary are the evidence kept.
        try:
            os.remove(run["jsonl"])
            run["samples_retained"] = False
        except OSError:
            run["samples_retained"] = True
    else:
        run["samples_retained"] = True
    return run


def aggregate(runs):
    """Per-segment worst-tail/median-body merge + run-to-run dispersion."""
    ok = [r for r in runs if r["status"] == "OK"]
    segments = {}
    disp = {}
    for seg in W.SEGMENTS:
        descs = [r["probe_summary"]["segments"][seg] for r in ok
                 if seg in r["probe_summary"].get("segments", {})]
        segments[seg] = W.merge_descriptors(descs)
        disp[seg] = {key: W.dispersion([d[key] for d in descs])
                     for key in ("p50", "p99", "p99_9", "max")}
    return {"runs_ok": len(ok), "segments": segments, "dispersion": disp}


def disp_line(disp, seg):
    """One dispersion line: how far two successive runs drift apart."""
    parts = []
    for key in ("p50", "p99", "p99_9", "max"):
        entry = disp.get(seg, {}).get(key) or {}
        pct = entry.get("spread_pct")
        parts.append("%s=%s" % (key, "n/a" if pct is None else "%.1f%%" % pct))
    return "  dispersion between runs (%-6s): %s" % (seg, "  ".join(parts))


def render_report(payload):
    cfg = payload["config"]
    lines = ["Relais M40 - critical path \"STATUT received -> ORDRE emitted\"",
             "host=%s  probe=%s  samples=%d  warmup=%d  runs=%d  integrity=%s"
             % (payload["host"], os.path.basename(cfg["probe"]), cfg["samples"],
                cfg["warmup"], cfg["runs"], "mac(level C)" if cfg["mac"]
                else "crc(level S)"),
             "isolation: %s  cpu=%s  lock=%s  prio=%s  env=%s"
             % (payload["environment"].get("status", "n/a"), cfg["cpu"],
                cfg["lock"], cfg["prio"], payload["env_path"]),
             ""]
    for mode, block in sorted(payload["modes"].items()):
        runs = block["runs"]
        agree = all(r.get("agreement", False) for r in runs if r["status"] == "OK")
        gap = max([r.get("gap_pct", 0.0) for r in runs] or [0.0])
        lines.append("mode %s: %d/%d runs OK, C-summary vs host/timing/wcet.py %s "
                     "(max gap %.3f%%)" % (mode, block["aggregate"]["runs_ok"],
                                           len(runs), "agree" if agree else "DIFFER",
                                           gap))
        for seg in W.SEGMENTS:
            desc = block["aggregate"]["segments"].get(seg)
            if desc:
                lines.append(W.stats_row(seg, desc))
        lines.append(disp_line(block["aggregate"]["dispersion"], "path"))
        bounds = [r["evt_bound_us"] for r in runs if r.get("evt_bound_us")]
        if bounds:
            lines.append("  extrapolated bound (Gumbel-POT, survival 1e-6, "
                         "host/timing/wcet.py): %.3f us" % max(bounds))
        loads = [r.get("loadavg_before") for r in runs if r.get("loadavg_before")]
        if loads:
            lines.append("  ambient loadavg during the runs: %s (1 min value: %s)"
                         % (loads, ", ".join("%.1f" % entry[0] for entry in loads)))
        for run in runs:
            if run["status"] != "OK":
                lines.append("  run %s: %s %s" % (run["tag"], run["status"],
                                                  run.get("stderr", "")))
        lines.append("")
    lines.append("margin to the PERF-01 deadline: python3 host/wcet/rta.py "
                 "--measure %s" % payload["measure_path"])
    return "\n".join(lines)


def parse_args(argv):
    ap = argparse.ArgumentParser(description="Portable WCET measurement (M40)")
    ap.add_argument("--samples", type=int, default=20000,
                    help="samples per run (default 20000)")
    ap.add_argument("--warmup", type=int, default=500,
                    help="warm-up ticks, not recorded (default 500)")
    ap.add_argument("--runs", type=int, default=2,
                    help="successive runs per mode: the dispersion (default 2)")
    ap.add_argument("--interference", default="none",
                    help="comma list among none,cpu,journal,net (default none)")
    ap.add_argument("--cpu", type=int, default=None,
                    help="pin the probe to that CPU (no root needed)")
    ap.add_argument("--lock", action="store_true", help="probe calls mlockall")
    ap.add_argument("--prio", type=int, default=0,
                    help="probe asks for SCHED_FIFO at that priority (0 = do not)")
    ap.add_argument("--params", default=None, help='"T=5,p=1,N=8" override')
    ap.add_argument("--probe", default=None, help="path of relais-wcet-probe")
    ap.add_argument("--host", default=None, help="label of this machine")
    ap.add_argument("--out", default=None, help="output directory")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 2,
                    help="busy processes of the cpu interference")
    ap.add_argument("--burn", type=float, default=120.0,
                    help="seconds of cpu interference available per run")
    ap.add_argument("--timeout", type=float, default=900.0,
                    help="seconds allowed for one probe run")
    ap.add_argument("--tolerance", type=float, default=1.0,
                    help="percent gap allowed between the C and Python stats")
    ap.add_argument("--no-isolate", action="store_true",
                    help="do not run isolate.sh before measuring")
    ap.add_argument("--no-keep-samples", dest="keep_samples",
                    action="store_false",
                    help="delete each raw JSONL once analysed (the aggregates and "
                         "the C summaries are kept; the command is recorded)")
    ap.set_defaults(keep_samples=True)
    ap.add_argument("--no-mac", dest="mac", action="store_false",
                    help="measure the CRC-32C level S instead of the MAC level C")
    ap.set_defaults(mac=True)
    return ap.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    host = args.host or W.host_label()
    probe = args.probe or default_probe()
    outdir = args.out or os.path.join(W.ROOT, "runs", "wcet", host)
    os.makedirs(outdir, exist_ok=True)
    if not os.path.exists(probe):
        print("BLOCKED(no probe binary at %s)" % probe)
        print(BUILD_HINT)
        return 2
    env, table, status = prepare_env(outdir, args.cpu, not args.no_isolate)
    if table:
        print(table, end="")
    env["status"] = status
    modes = [m.strip() for m in args.interference.split(",") if m.strip()]
    payload = {"tool": "host/wcet/measure.py", "host": host, "probe": probe,
               "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
               "environment": env, "env_path": os.path.join(outdir, "env.json"),
               "measure_path": os.path.join(outdir, "measure.json"),
               "config": {"samples": args.samples, "warmup": args.warmup,
                          "runs": args.runs, "modes": modes, "cpu": args.cpu,
                          "lock": args.lock, "prio": args.prio,
                          "params": args.params, "mac": args.mac,
                          "workers": args.workers, "probe": probe,
                          "keep_samples": args.keep_samples}}
    payload["modes"] = run_modes(args, probe, host, outdir, modes)
    failed = sum(1 for block in payload["modes"].values()
                 for run in block["runs"] if run["status"] != "OK")
    payload["status"] = "OK" if failed == 0 else "PARTIAL(%d runs failed)" % failed
    report = render_report(payload)
    with open(os.path.join(outdir, "report.txt"), "w", encoding="utf-8") as fh:
        fh.write(report + "\n")
    W.write_json(os.path.join(outdir, "measure.json"), payload)
    print(report)
    return 0 if failed == 0 else 1


def clean_stale(outdir, mode, runs):
    """Remove the artefacts of a previous campaign that had more runs, so the
    evidence directory never mixes two campaigns."""
    for pattern in ("samples-%s-run*.jsonl", "summary-%s-run*.json"):
        for path in glob.glob(os.path.join(outdir, pattern % mode)):
            try:
                index = int(os.path.basename(path).split("-run")[1].split(".")[0])
            except (IndexError, ValueError):
                continue
            if index > runs:
                try:
                    os.remove(path)
                except OSError:
                    pass


def run_modes(args, probe, host, outdir, modes):
    """One block per interference mode, each holding its runs and its aggregate."""
    blocks = {}
    for mode in modes:
        if mode not in INTERFERENCE.MODES:
            print("BLOCKED(unknown interference %s)" % mode)
            continue
        clean_stale(outdir, mode, args.runs)
        runs = []
        for index in range(1, args.runs + 1):
            tag = "%s-run%d" % (mode, index)
            run = run_once(args, probe, host, outdir, tag, args.workers)
            runs.append(analyse(run, args.tolerance, args.keep_samples))
        blocks[mode] = {"runs": runs, "aggregate": aggregate(runs)}
    return blocks


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

