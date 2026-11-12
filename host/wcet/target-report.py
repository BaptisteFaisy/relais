#!/usr/bin/env python3
"""target-report.py — the WCET status of every target, with evidence (M40).

The specification promises microseconds on the critical path. Three machines can
carry that promise: the development PC, a Raspberry Pi (an ARM SoC close to the
automotive class) and the QNX 8.0 target. This tool publishes, per target:

  * MEASURED  — an evidence file exists (runs/wcet/<host>/measure.json, produced
                by host/wcet/measure.py on that machine) and its numbers are
                reproduced here verbatim;
  * BLOCKED(<exact reason>) — no evidence, with the detection command that was
                actually run on this machine and its output, plus the exact
                command sequence to execute on the target.

No number is ever invented, estimated or extrapolated from another target: a
target without an evidence file has no number (rule 9 of the mission context).

Usage:
  python3 host/wcet/target-report.py                     # markdown on stdout
  python3 host/wcet/target-report.py --out runs/wcet/TARGETS.md
"""
import argparse
import glob
import os
import platform
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wcet_lib as W  # noqa: E402

PC, PI, QNX = "pc", "pi", "qnx"

# The protocol each absent target must run. Commands are copy-pasteable; the
# ones that need root are marked ROOT and are never executed by this harness.
PROTOCOLS = {
    PI: [
        "# on the Raspberry Pi (64-bit OS), from the repository root:",
        "make -j4 all wcet-probe",
        "sh host/wcet/isolate.sh --cpu 2 --json runs/wcet/env-pi.json",
        "#   ROOT, operator's call: fix the clock before believing a tail",
        "#   sudo cpupower frequency-set -g performance",
        "#   sudo raspi-config nonint do governor performance",
        "python3 host/wcet/measure.py --samples 20000 --runs 3 --cpu 2 --lock \\",
        "    --interference none,cpu,journal,net",
        "python3 host/wcet/rta.py --sweep",
        "# bring the evidence back to the PC, then re-run this report:",
        "scp -r runs/wcet/linux-aarch64 <pc>:<repo>/runs/wcet/",
    ],
    QNX: [
        "# 1. build the probe with the SDP (host or target, same source):",
        "source ~/qnx800/qnxsdp-env.sh",
        "cd target && qcc -V gcc_ntox86_64 -std=c11 -O2 -Wall -Wextra -Werror \\",
        "    -D_POSIX_C_SOURCE=200809L -I../core/include -I../host/wcet \\",
        "    ../host/wcet/probe.c ../host/wcet/wcet_stats.c ../core/src/*.c \\",
        "    -lm -o relais-wcet-probe && cd ..",
        "# 2. on the target (or in the QEMU guest built from image/), pinned and",
        "#    at the priority of docs/TIMING.md section 1:",
        "on -C 0 ./target/relais-wcet-probe --samples 20000 --warmup 500 \\",
        "    --lock --prio 61 --host qnx800-x86_64 \\",
        "    --out /tmp/samples-qnx.jsonl --summary /tmp/summary-qnx.json",
        "# 3. the probe alone is enough: it prints the percentiles and writes the",
        "#    JSONL that host/timing/wcet.py analyses (no python needed on target):",
        "python3 host/timing/wcet.py /tmp/samples-qnx.jsonl",
        "# 4. kernel-side evidence (io-sock priority, page faults, partitioning):",
        "#    host/timing/procedures-qnx.md P1..P4, then traceprinter > trace.txt",
        "# 5. copy runs/wcet/qnx800-x86_64/ back and re-run this report.",
    ],
    PC: [
        "# on this machine:",
        "make -j4 all wcet-probe",
        "python3 host/wcet/measure.py --samples 20000 --runs 2 --cpu 3 --lock",
        "python3 host/wcet/rta.py --sweep",
    ],
}


def run_check(command):
    """Run a detection command; return (printed command, result text)."""
    try:
        proc = subprocess.run(command, shell=True, capture_output=True, text=True,
                              timeout=20)
    except (OSError, subprocess.SubprocessError) as exc:
        return command, "error: %s" % exc
    out = (proc.stdout or "").strip().replace("\n", " ")
    err = (proc.stderr or "").strip().replace("\n", " ")
    text = out or err or "(empty)"
    return command, "%s [rc=%d]" % (text[:120], proc.returncode)


def evidence():
    """The detections that justify every BLOCKED marker, with their real output."""
    checks = [
        "uname -srm",
        "ls /dev/mmcblk* 2>/dev/null",
        "ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null",
        "command -v qemu-system-aarch64",
        "command -v qcc",
        "ls -d ~/qnx800 /opt/qnx* 2>/dev/null",
        "ls ~/qnx800/host/usr/bin/qcc /opt/qnx*/host/usr/bin/qcc 2>/dev/null",
        "cat /proc/device-tree/model 2>/dev/null",
        "command -v taskset",
        "command -v python3",
    ]
    return [run_check(command) for command in checks]


def is_pi_here():
    for path in ("/proc/device-tree/model",
                 "/sys/firmware/devicetree/base/model"):
        if os.path.exists(path):
            try:
                with open(path, "r", encoding="utf-8", errors="ignore") as fh:
                    if "raspberry pi" in fh.read().lower():
                        return True
            except OSError:
                pass
    return False


def classify(payload):
    """Which target an evidence file came from, by its own recorded identity."""
    host = str(payload.get("host", "")).lower()
    env = payload.get("environment", {}) or {}
    model = str(env.get("cpu_model", "")).lower()
    system = str(env.get("os", "")).lower()
    machine = str(env.get("machine", "")).lower()
    if host.startswith(QNX) or system == "qnx":
        return QNX
    if "raspberry" in model or "bcm" in model:
        return PI
    if machine in ("aarch64", "arm64") and not model:
        return PI
    return PC


def collect(runs_dir):
    """target id -> list of (path, payload) for every evidence file found."""
    found = {PC: [], PI: [], QNX: []}
    pattern = os.path.join(runs_dir, "*", "measure.json")
    for path in sorted(glob.glob(pattern)):
        try:
            payload = W.read_json(path)
        except (OSError, ValueError):
            continue
        found[classify(payload)].append((path, payload))
    return found


def label(target):
    return {PC: "development PC (this machine, %s)" % platform.machine(),
            PI: "Raspberry Pi (aarch64 Linux, SoC near the automotive class)",
            QNX: "QNX 8.0 target (x86_64, the deployed operating system)"}[target]


def measured_block(entries):
    lines = []
    for path, payload in entries:
        cfg = payload.get("config", {})
        lines.append("- evidence: `%s` (host=%s, date=%s, %s samples x %s runs, "
                     "integrity=%s)" % (
                         os.path.relpath(path, W.ROOT), payload.get("host"),
                         payload.get("date"), cfg.get("samples"), cfg.get("runs"),
                         "MAC level C" if cfg.get("mac") else "CRC level S"))
        for mode, block in sorted(payload.get("modes", {}).items()):
            path_desc = (block.get("aggregate", {}).get("segments") or {}).get("path")
            if not path_desc:
                continue
            lines.append("  - mode `%s`: n=%d p50=%.3f p90=%.3f p99=%.3f "
                         "p99.9=%.3f max=%.3f us (stdev %.3f)" % (
                             mode, path_desc["n"], path_desc["p50"],
                             path_desc["p90"], path_desc["p99"],
                             path_desc["p99_9"], path_desc["max"],
                             path_desc["stdev"]))
        rta = os.path.join(os.path.dirname(path), "rta.json")
        if os.path.exists(rta):
            data = W.read_json(rta)
            lines.append("  - margins: %s" % ", ".join(
                "%s=%+.1f us" % (k, v) for k, v in
                sorted((data.get("margins_us") or {}).items())))
    return lines


def blocked_block(target, reason):
    lines = ["- status: **BLOCKED(%s)**" % reason,
             "- no measurement exists for this target; no number is published.",
             "- protocol to run there:",
             "  ```sh"]
    lines += ["  " + line for line in PROTOCOLS[target]]
    lines.append("  ```")
    return lines


def blocked_reason(target, results):
    """The reason is DERIVED from the detection output, never hard-coded: the
    report must not contradict the evidence it prints just above."""
    def got(command):
        return results.get(command, "(not run)")
    if target == PC:
        return "no measurement on this machine yet"
    if target == PI:
        if is_pi_here():
            return "this host is a Raspberry Pi but no measurement was taken"
        return ("no Pi bench on this host: `ls /dev/mmcblk*` -> %s; "
                "`ls /dev/ttyUSB* /dev/ttyACM*` -> %s; "
                "`command -v qemu-system-aarch64` -> %s; "
                "`cat /proc/device-tree/model` -> %s"
                % (got("ls /dev/mmcblk* 2>/dev/null"),
                   got("ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null"),
                   got("command -v qemu-system-aarch64"),
                   got("cat /proc/device-tree/model 2>/dev/null")))
    uname_s = platform.system()
    if uname_s == "QNX":
        return "this host is QNX but no measurement was taken"
    return ("no usable QNX toolchain here: `command -v qcc` -> %s; SDP "
            "directories -> %s; `qcc` binary -> %s; this host is %s"
            % (got("command -v qcc"),
               got("ls -d ~/qnx800 /opt/qnx* 2>/dev/null"),
               got("ls ~/qnx800/host/usr/bin/qcc /opt/qnx*/host/usr/bin/qcc 2>/dev/null"),
               uname_s))


def render(found, checks, runs_dir):
    lines = ["# Relais — WCET per target (generated, do not edit by hand)", "",
             "Regenerate with `python3 host/wcet/target-report.py`.", "",
             "## Detection run on this machine", ""]
    for command, result in checks:
        lines.append("- `%s` -> %s" % (command, result))
    lines.append("")
    results = dict(checks)
    for target in (PC, PI, QNX):
        lines.append("## %s — %s" % (target.upper(), label(target)))
        lines.append("")
        if found[target]:
            lines.append("- status: **MEASURED** (%d evidence file(s))"
                         % len(found[target]))
            lines += measured_block(found[target])
        else:
            lines += blocked_block(target, blocked_reason(target, results))
        lines.append("")
    lines.append("Evidence directory: `%s` (git-ignored; commit nothing but the "
                 "tables copied into docs/WCET-TARGETS.md)."
                 % os.path.relpath(runs_dir, W.ROOT))
    return "\n".join(lines)


def main(argv):
    ap = argparse.ArgumentParser(description="Per-target WCET status report (M40)")
    ap.add_argument("--runs-dir", default=os.path.join(W.ROOT, "runs", "wcet"),
                    help="directory holding the per-host evidence")
    ap.add_argument("--out", default=None, help="write the markdown here")
    args = ap.parse_args(argv)

    checks = evidence()
    found = collect(args.runs_dir)
    text = render(found, checks, args.runs_dir)
    if args.out:
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
        print("written: %s" % args.out)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
