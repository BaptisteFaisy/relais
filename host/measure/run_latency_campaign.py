#!/usr/bin/env python3
"""run_latency_campaign.py — the M02 latency campaign (PERF-01, PERF-08, OBS-03).

Starts the actuator and two instrumented computers (`relais-node --latency
FILE`) for each configuration, waits for the actuator to finish its ticks, then
runs `build/relais-latency` on every sample file and keeps the report next to
the samples. Everything lands under --out (runs/latency by default, gitignored).

Each configuration answers a different question:

  pc-nominal  the reference: a real PC, no injected load           (PERF-01)
  pc-load     the spec's condition: CPU hogs at a LOWER priority   (PERF-01)
  pc-tight    the period lowered to 1 ms, the breaking point probe (PERF-08)
  pc-slow     CMD-07 slow motion: same ticks, 50 ms of wall clock  (determinism)
  pc-long     40 000 ticks at T=5 ms: a p99.9 with real weight      (PERF-01)
  pc-pinned   actuator, both nodes and the hogs on ONE core        (contention)

A QNX guest configuration would need a QNX 8.0 image built with the SDP
(mkqnximage); without one this script says BLOCKED instead of pretending.

Standard library only (rule 6). Usage:
  python3 host/measure/run_latency_campaign.py --out runs/latency
  python3 host/measure/run_latency_campaign.py --configs pc-nominal,pc-load
  python3 host/measure/run_latency_campaign.py --list
"""
import argparse
import json
import os
import platform
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

CONFIGS = [
    {"name": "pc-nominal", "params": "T=5,p=1,N=8", "ticks": 4000, "load": 0,
     "note": "reference run on the real PC, no injected load"},
    {"name": "pc-load", "params": "T=5,p=1,N=8", "ticks": 4000, "load": -1,
     "note": "PERF-01 condition: one nice-19 CPU hog per core (lower priority)"},
    {"name": "pc-tight", "params": "T=1,p=1,N=8", "ticks": 4000, "load": 0,
     "note": "PERF-08 probe: period lowered to 1 ms"},
    {"name": "pc-slow", "params": "T=5,p=1,N=8,slow=10", "ticks": 600, "load": 0,
     "note": "CMD-07 slow motion: 600 ticks of 50 ms, physics unchanged"},
    {"name": "pc-long", "params": "T=5,p=1,N=8", "ticks": 40000, "load": 0,
     "note": "long run at the nominal period: 40 000 samples per segment"},
    {"name": "pc-pinned", "params": "T=5,p=1,N=8", "ticks": 4000, "load": 2,
     "pin": "2",
     "note": "the three processes and 2 hogs pinned to one core (taskset)"},
]
TRAJECTORY = "kind=slalom amp=0.6 freq=0.5"


def binary(name):
    path = os.path.join(ROOT, "build", name)
    if not os.path.exists(path):
        sys.exit("missing %s: run `make` first" % path)
    return path


def period_ms(params):
    """Effective tick period of a --params string, including slow motion."""
    values = {}
    for item in params.split(","):
        if "=" in item:
            key, val = item.split("=", 1)
            values[key.strip()] = val.strip()
    period = int(values.get("T", 5))
    slow = int(values.get("slow", 1))
    return period * slow if slow > 1 else period


def describe_machine():
    """What the numbers depend on: kernel, CPU, cores, load at start."""
    info = {"kernel": "%s %s" % (platform.system(), platform.release()),
            "machine": platform.machine(),
            "python": platform.python_version(),
            "cores": os.cpu_count(),
            "loadavg": list(os.getloadavg()),
            "cpu_model": "unknown"}
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as handle:
            for line in handle:
                if line.lower().startswith("model name"):
                    info["cpu_model"] = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    return info


def wrap(pin, cmd):
    """Pin a command to a CPU list with taskset (emulates a small target)."""
    return ["taskset", "-c", str(pin)] + cmd if pin else cmd


def start(proc_cmd, log_path):
    log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(proc_cmd, cwd=ROOT, stdout=log,
                            stderr=subprocess.STDOUT, start_new_session=True)
    return proc, log


def lower_priority():
    """preexec_fn: the hog runs below the measured chain (PERF-01)."""
    os.nice(19)


def start_load(count, pin=None):
    """CPU hogs at nice 19: a load of strictly LOWER priority (PERF-01)."""
    procs = []
    for _ in range(count):
        procs.append(subprocess.Popen(
            wrap(pin, [sys.executable, "-c", "while True:\n    pass"]),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            preexec_fn=lower_priority, start_new_session=True))
    time.sleep(1.0)  # let the load settle before the first tick
    return procs


def stop(procs):
    for proc in procs:
        if proc.poll() is None:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except (OSError, ProcessLookupError):
                proc.kill()
    for proc in procs:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def run_config(config, out_dir, ports, node_bin, latency=True):
    """One configuration: actuator + two instrumented nodes, then the logs."""
    os.makedirs(out_dir, exist_ok=True)
    ticks = int(config["ticks"])
    params = config["params"]
    pin = config.get("pin")
    seconds = ticks * period_ms(params) / 1000.0
    samples = {}
    actuator, alog = start(wrap(pin,
        [binary("relais-actuator"), "--port", str(ports[0]),
         "--peer", "A=127.0.0.1:%d" % ports[1],
         "--peer", "B=127.0.0.1:%d" % ports[2],
         "--ticks", str(ticks), "--params", params, "--trajectory", TRAJECTORY,
         "--journal", os.path.join(out_dir, "actuator.jsonl"),
         "--state", os.path.join(out_dir, "arb.state")]),
        os.path.join(out_dir, "actuator.log"))
    nodes, logs = [], []
    for index, role in enumerate(("A", "B")):
        path = os.path.join(out_dir, "lat-%s.jsonl" % role)
        samples[role] = path
        command = wrap(pin, [node_bin, "--self", role, "--port", str(ports[index + 1]),
                   "--actuator", "127.0.0.1:%d" % ports[0],
                   "--peer", "127.0.0.1:%d" % ports[2 - index],
                   "--ticks", str(ticks), "--params", params])
        if latency:
            command += ["--latency", path]
        proc, log = start(command, os.path.join(out_dir, "node-%s.log" % role))
        nodes.append(proc)
        logs.append(log)
    try:
        actuator.wait(timeout=seconds + 30)
    except subprocess.TimeoutExpired:
        actuator.kill()
    finally:
        for proc in nodes:
            if proc.poll() is None:
                proc.kill()
        for log in [alog] + logs:
            log.close()
    return samples, actuator.returncode


def report(samples, out_dir):
    """build/relais-latency on the samples: human report + JSON summaries."""
    files = [samples[role] for role in ("A", "B") if os.path.exists(samples[role])]
    if not files:
        return None
    text = subprocess.run([binary("relais-latency"), "--worst", "3"] + files,
                          cwd=ROOT, capture_output=True, text=True)
    with open(os.path.join(out_dir, "report.txt"), "w", encoding="utf-8") as fh:
        fh.write(text.stdout + text.stderr)
    machine = subprocess.run([binary("relais-latency"), "--json"] + files,
                             cwd=ROOT, capture_output=True, text=True)
    summary = []
    for line in machine.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            summary.append(json.loads(line))
    with open(os.path.join(out_dir, "summary.jsonl"), "w", encoding="utf-8") as fh:
        for item in summary:
            fh.write(json.dumps(item, sort_keys=True) + "\n")
    print(text.stdout)
    return summary


def build_parser():
    ap = argparse.ArgumentParser(description="Relais M02 latency campaign")
    ap.add_argument("--out", default=os.path.join("runs", "latency"))
    ap.add_argument("--configs", default=",".join(c["name"] for c in CONFIGS))
    ap.add_argument("--port-base", type=int, default=7410)
    ap.add_argument("--node-bin", default=None,
                    help="alternate relais-node binary (equivalence checks)")
    ap.add_argument("--load-multiplier", type=int, default=1)
    ap.add_argument("--no-latency", action="store_true",
                    help="run the nodes uninstrumented: equivalence check of "
                         "--node-bin against the instrumented build")
    ap.add_argument("--list", action="store_true")
    return ap


def selected_configs(wanted):
    known = {c["name"]: c for c in CONFIGS}
    for name in wanted:
        if name not in known:
            sys.exit("unknown configuration %s (try --list)" % name)
    return [known[name] for name in wanted]


def run_one(config, index, args, out_root, node_bin, cores):
    """One configuration: optional load, the two nodes, the report."""
    out_dir = os.path.join(out_root, config["name"])
    ports = [args.port_base + 10 * index + offset for offset in range(3)]
    hogs = max(1, cores) if config["load"] < 0 else config["load"]
    hogs *= args.load_multiplier
    print("\n-- %s: %s, %d ticks, %d CPU hog(s), pin=%s --" % (
        config["name"], config["params"], config["ticks"], hogs,
        config.get("pin") or "any"))
    load_procs = start_load(hogs, config.get("pin")) if hogs > 0 else []
    before = os.getloadavg()
    started = time.monotonic()
    try:
        samples, code = run_config(config, out_dir, ports, node_bin,
                                   not args.no_latency)
    finally:
        stop(load_procs)
    summary = None if args.no_latency else report(samples, out_dir)
    record = {"name": config["name"], "params": config["params"],
              "ticks": config["ticks"], "load": hogs, "pin": config.get("pin"),
              "note": config["note"], "actuator_rc": code,
              "wall_s": round(time.monotonic() - started, 2),
              "loadavg_before": [round(x, 2) for x in before],
              "loadavg_after": [round(x, 2) for x in os.getloadavg()],
              "summary": summary or []}
    print("   actuator exit %s, %.1f s wall clock, ambient load %s -> %s" % (
        code, record["wall_s"], record["loadavg_before"][0],
        record["loadavg_after"][0]))
    return record


def main():
    args = build_parser().parse_args()
    if args.list:
        for config in CONFIGS:
            print("%-12s %-24s %5d ticks  %s" % (
                config["name"], config["params"], config["ticks"], config["note"]))
        return 0
    wanted = [n.strip() for n in args.configs.split(",") if n.strip()]
    configs = selected_configs(wanted)
    out_root = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(out_root, exist_ok=True)
    node_bin = args.node_bin or binary("relais-node")
    machine = describe_machine()
    with open(os.path.join(out_root, "env.json"), "w", encoding="utf-8") as fh:
        json.dump(machine, fh, indent=2, sort_keys=True)
    print("== M02 latency campaign on %s (%s cores) ==" % (
        machine["kernel"], machine["cores"]))
    print("   cpu: %s" % machine["cpu_model"])
    campaign = {"machine": machine, "node_bin": node_bin, "configs": []}
    for index, config in enumerate(configs):
        campaign["configs"].append(
            run_one(config, index, args, out_root, node_bin,
                    machine["cores"] or 1))
    with open(os.path.join(out_root, "campaign.json"), "w", encoding="utf-8") as fh:
        json.dump(campaign, fh, indent=2, sort_keys=True)
    print("\ncampaign written to %s" % out_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
