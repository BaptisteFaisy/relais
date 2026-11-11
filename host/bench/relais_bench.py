#!/usr/bin/env python3
"""Relais — one command to run the demonstration on a plain PC (spec 8, INJ-01).

Two computers steer the same simulated actuator. Both run here as ordinary
processes, exactly like the QNX image does, minus QNX: the actuator owns the
tempo and the mandate, each computer computes the control law, and the bench
kills, freezes or cuts to make the handover happen. No QEMU, no root, no QNX
SDP needed: the jury can replay the whole thing with one command.

  python3 host/bench/relais_bench.py --demo
  python3 host/bench/relais_bench.py --scenario scenarios/slalom-death-A.json
  python3 host/bench/relais_bench.py --verify runs/<run>/actuator.jsonl --level complet

The authority is the actuator's journal (spec 6.4); the verifier reads it and
judges the invariants of section 7.2.
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT_ACTUATOR = 7200
PORT_A = 7201
PORT_B = 7202


def binary(name):
    path = os.path.join(ROOT, "build", name)
    if not os.path.exists(path):
        sys.exit("missing %s: run `make` first" % path)
    return path


def load_scenario(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def params_string(params):
    return ",".join("%s=%s" % (k, v) for k, v in params.items())


def trajectory_string(traj):
    return " ".join("%s=%s" % (k, v) for k, v in traj.items())


def start(cmd, log_path):
    log = open(log_path, "w", encoding="utf-8")
    return subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True, text=True), log


def run_scenario(scenario, out_dir, verbose=True):
    """Run one scenario in PC mode; return the directory holding the journals."""
    os.makedirs(out_dir, exist_ok=True)
    ticks = int(scenario.get("ticks", 4000))
    params = scenario.get("params", {"T": 5, "p": 1, "N": 8})
    traj = scenario.get("trajectory", {"kind": "slalom", "amp": 0.6, "freq": 0.5})
    period_ms = int(params.get("T", 5))
    journal = os.path.join(out_dir, "actuator.jsonl")

    actuator, alog = start([binary("relais-actuator"),
                            "--port", str(PORT_ACTUATOR),
                            "--peer", "A=127.0.0.1:%d" % PORT_A,
                            "--peer", "B=127.0.0.1:%d" % PORT_B,
                            "--ticks", str(ticks),
                            "--params", params_string(params),
                            "--trajectory", trajectory_string(traj),
                            "--journal", journal,
                            "--state", os.path.join(out_dir, "arb.state")],
                           os.path.join(out_dir, "actuator.log"))
    node_a, nlog_a = start([binary("relais-node"), "--self", "A",
                            "--port", str(PORT_A),
                            "--actuator", "127.0.0.1:%d" % PORT_ACTUATOR,
                            "--peer", "127.0.0.1:%d" % PORT_B,
                            "--ticks", str(ticks),
                            "--params", params_string(params)],
                           os.path.join(out_dir, "node-A.log"))
    node_b, nlog_b = start([binary("relais-node"), "--self", "B",
                            "--port", str(PORT_B),
                            "--actuator", "127.0.0.1:%d" % PORT_ACTUATOR,
                            "--peer", "127.0.0.1:%d" % PORT_A,
                            "--ticks", str(ticks),
                            "--params", params_string(params)],
                           os.path.join(out_dir, "node-B.log"))

    nodes = {"A": node_a, "B": node_b}
    started = time.monotonic()
    applied = 0
    for fault in sorted(scenario.get("faults", []), key=lambda f: f.get("at_tick", 0)):
        at = int(fault.get("at_tick", 0))
        wait = at * period_ms / 1000.0 - (time.monotonic() - started)
        if wait > 0:
            time.sleep(wait)
        applied += 1
        apply_fault(fault, nodes, verbose, period_ms / 1000.0)
    try:
        actuator.wait(timeout=ticks * period_ms / 1000.0 + 30)
    except subprocess.TimeoutExpired:
        actuator.kill()
    finally:
        for proc in (node_a, node_b):
            if proc.poll() is None:
                proc.send_signal(signal.SIGCONT)
                proc.kill()
        for log in (alog, nlog_a, nlog_b):
            log.close()
    return journal, applied


def apply_fault(fault, nodes, verbose, period_s=0.005):
    """Death, freeze and thaw of a whole machine; the cables are the QEMU bench."""
    kind = fault.get("type")
    target = str(fault.get("target", ""))
    node = nodes.get(target)
    label = "%s(%s)" % (kind, target)
    if node is None:
        if verbose:
            print("  fault %-22s ignored (only whole-machine faults in PC mode)" % label)
        return
    if kind == "death":
        node.kill()
    elif kind == "freeze":
        node.send_signal(signal.SIGSTOP)
        time.sleep(float(fault.get("ticks", 20)) * period_s)
        node.send_signal(signal.SIGCONT)
        if node.poll() is None:
            node.send_signal(signal.SIGCONT)
    elif kind == "restart":
        node.send_signal(signal.SIGCONT)
    if verbose:
        print("  fault %-22s at tick %s" % (label, fault.get("at_tick")))


def run_qemu(scenario, out_dir, level):
    """Two QNX guests under QEMU, driven over QMP (spec 4.11, INJ-02..04).

    Needs a QNX 8.0 image built with mkqnximage; without one, the orchestrator
    says so with the exact command to build it instead of failing obscurely.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import orchestrator
    except ImportError as exc:
        sys.exit("QEMU mode is unavailable: %s" % exc)
    runner = getattr(orchestrator, "Orchestrator", None)
    if runner is None:
        sys.exit("QEMU mode: host/bench/orchestrator.py exposes no Orchestrator")
    return runner(root=ROOT, out_dir=out_dir).run(scenario, level=level)


def run_simulated(scenario, out_dir, level):
    """Deterministic run: the seeded simulator, no clock, no jitter (spec 7.4).

    Same core, same invariants, same journal format; only the time model
    changes. This is the mode a reviewer can trust on a busy machine.
    """
    os.makedirs(out_dir, exist_ok=True)
    journal = os.path.join(out_dir, "actuator.jsonl")
    twin = os.path.join(out_dir, "twin.jsonl")
    cmd = [binary("relais-simulate"),
           "--ticks", str(scenario.get("ticks", 2000)),
           "--seed", str(scenario.get("seed", 1)),
           "--params", params_string(scenario.get("params", {})),
           "--trajectory", trajectory_string(scenario.get("trajectory", {})),
           "--scenario", scenario.get("name", "run"),
           "--id", str(scenario.get("id", 0)),
           "--journal", journal, "--twin-journal", twin, "--quiet"]
    if level == "complet":
        cmd += ["--level", "complet"]
    for fault in scenario.get("faults", []):
        cmd += ["--fault", fault_arg(fault)]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode not in (0, 1):
        sys.exit(proc.stdout + proc.stderr)
    return journal, len(scenario.get("faults", []))


def fault_arg(fault):
    """Translate a scenario fault into the simulator's at:kind:node:arg:value."""
    kind = {"death": "death", "freeze": "freeze", "thaw": "thaw", "restart": "restart",
            "slow": "slow", "bias": "bias", "crash": "crash",
            "cut": {"A-B": "cut-peer"}.get(fault.get("link", ""), "cut-actuator"),
            "restore": "restore"}.get(fault.get("type"), "death")
    node = {"A": 0, "B": 1}.get(str(fault.get("target", "A")), 0)
    arg = int(fault.get("ticks", fault.get("every", 0)) or 0)
    return "%d:%s:%d:%d:%s" % (int(fault.get("at_tick", 0)), kind, node, arg,
                               fault.get("value", 0))


def nested_faults(scenario):
    """True when a second fault lands before the first one is over (spec 3.3)."""
    period_s = float(scenario.get("params", {}).get("T", 5)) / 1000.0
    windows = []
    for fault in scenario.get("faults", []):
        at = float(fault.get("at_tick", 0))
        kind = fault.get("type")
        if kind in ("death",):
            windows.append((at, float("inf"), fault.get("target")))
        elif kind in ("freeze", "cut", "slow"):
            windows.append((at, at + float(fault.get("ticks", 20)), fault.get("target")))
        else:
            windows.append((at, at + 1, fault.get("target")))
    for i, first in enumerate(windows):
        for second in windows[i + 1:]:
            if second[0] <= first[1] and second[2] != first[2]:
                return True
    return False


def verify(journal, level, double_fault=False):
    verifier = binary("relais-verify")
    cmd = [verifier, journal, "--level", level]
    if double_fault:
        cmd.append("--double-fault")
    twin = os.path.join(os.path.dirname(journal), "twin.jsonl")
    if os.path.exists(twin):
        cmd += ["--twin", twin]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print(proc.stdout.strip())
    return proc.returncode


def summary(journal, limit=6):
    """Show the handover moments: who commanded, from where, under which mandate."""
    rows = []
    with open(journal, encoding="utf-8") as fh:
        for line in fh:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("kind") == "event" and record.get("event") in (
                    "mandate_granted", "mandate_revoked", "mandate_auto_switch",
                    "cession", "fallback_enter", "fallback_exit", "stop_requested"):
                rows.append(record)
    print("\n  handover journal")
    print("  %-8s %-22s %-8s %s" % ("tick", "event", "node", "mandate"))
    for record in rows[:limit]:
        print("  %-8s %-22s %-8s %s" % (record.get("tick"), record.get("event"),
                                        record.get("node") or "-", record.get("mandate")))
    if len(rows) > limit:
        print("  ... %d more events" % (len(rows) - limit))


# The demonstration of spec 8, on a plain PC. The period is 25 ms instead of
# 5 ms (CMD-07, slow motion): a shared PC cannot promise a 5 ms scheduling
# jitter, and the protocol is expressed in ticks, not in milliseconds. The
# 5 ms / 200 Hz figures are measured on the QNX target under QEMU with KVM.
DEMO = {
    "name": "demo",
    "ticks": 1600,
    # p is 3 here instead of the protocol default of 1: on a shared PC the
    # scheduler alone can cost more than 25 ms. The QNX target under KVM is
    # where p = 1 and T = 5 ms are claimed and measured (spec 5.8, 11).
    "params": {"T": 25, "p": 3, "N": 8},
    "trajectory": {"kind": "slalom", "amp": 0.6, "freq": 0.5},
    "faults": [
        {"at_tick": 400, "type": "freeze", "target": "B", "ticks": 40},
        {"at_tick": 900, "type": "death", "target": "A"},
    ],
}


def main():
    ap = argparse.ArgumentParser(description="Relais bench, PC mode")
    ap.add_argument("--scenario", help="scenario file (see scenarios/)")
    ap.add_argument("--demo", action="store_true", help="the demonstration of spec 8")
    ap.add_argument("--out", default="", help="run directory")
    ap.add_argument("--level", default="complet", choices=["socle", "complet"])
    ap.add_argument("--verify", help="verify an existing journal and exit")
    ap.add_argument("--mode", default="pc", choices=["pc", "sim", "qemu"],
                    help="pc: three real processes on this host (default); "
                         "sim: the seeded simulator, deterministic, no clock; "
                         "qemu: two QNX 8.0 guests driven over QMP")
    ap.add_argument("--sim", action="store_true",
                    help="shortcut for --mode sim")
    args = ap.parse_args()

    if args.verify:
        return verify(args.verify, args.level)

    scenario = DEMO if args.demo else load_scenario(args.scenario) if args.scenario else None
    if scenario is None:
        ap.print_help()
        return 2

    out_dir = args.out or os.path.join("runs", "%s-%d" % (
        scenario.get("name", "run"), int(time.time())))
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    print("== Relais: %s (%d ticks, %d ms per tick, mode %s) ==" % (
        scenario.get("name", "run"), scenario.get("ticks", 0),
        scenario.get("params", {}).get("T", 5), "sim" if args.sim else args.mode))
    mode = "sim" if args.sim else args.mode
    if mode == "qemu":
        journal, faults = run_qemu(scenario, out_dir, args.level)
    elif mode == "sim":
        journal, faults = run_simulated(scenario, out_dir, args.level)
    else:
        journal, faults = run_scenario(scenario, out_dir)
    print("  run directory: %s (%d fault(s) injected)" % (out_dir, faults))
    nested = nested_faults(scenario)
    if nested:
        print("  nested faults: the claim is the safe state, not the continuity (spec 3.3)")
    summary(journal)
    return verify(journal, args.level, nested)


if __name__ == "__main__":
    sys.exit(main())
