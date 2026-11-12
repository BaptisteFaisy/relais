#!/usr/bin/env python3
"""Relais M48 — the real failover of the Relais protocol, played in a real
QNX 8.0 guest (QEMU + KVM).

What the mission asks for, and where each piece lives:

  * the scenario that runs INSIDE the guest: image/fragments/
    rl-relais-failover.sh (the actuator, the two computers, the SIGKILL of
    the holder from the guest, the reading of the actuator's journal in the
    guest);
  * the in-guest probe that measures the takeover on the GUEST clock and
    parses the journal with the very same code the verifier uses:
    host/bench/guest/rl_relais_failover.c;
  * the guest binaries: the node (host/node/relais-node.c), the actuator
    (actuator/pc/relais-actuator.c) and the verifier (host/verify/
    relais-verify.c), cross-compiled for QNX 8.0 with the real SDP qcc;
  * the evidence: docs/evidence/qnx-m48-relais-console.log (the transcript)
    and docs/evidence/qnx-m48-relais.json (the numbers).

The binaries of target/ (M45) are injected too, and the same scenario can be
played with the QNX actuator of target/ -- see docs/QNX-RUN.md for what that
run shows and for the defect it exposes.

Requirement IDs: INJ-01 (the fault is injected from inside the guest),
PERF-02 (the takeover delay is measured, never invented), OBS-04 (the console
transcript is the evidence), QNX-08 (a real QNX 8.0 guest under KVM).

Usage:
    python3 host/bench/qnx_relais.py run [--actuator paced|qnx] [--ticks N]
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from typing import Optional

try:  # package layout
    from .qnx_image import QnxVmError, create_vm, rebuild_vm, sdp_env_script, \
        write_ifs_files
    from .qnx_vm import DEFAULT_RUN_DIR, DEFAULT_VM_DIR, QnxVm, ROOT, \
        parse_marker_lines, transcript_path
except ImportError:  # flat script layout
    from qnx_image import QnxVmError, create_vm, rebuild_vm, sdp_env_script, \
        write_ifs_files
    from qnx_vm import DEFAULT_RUN_DIR, DEFAULT_VM_DIR, QnxVm, ROOT, \
        parse_marker_lines, transcript_path

FRAGMENTS = os.path.join(ROOT, "image", "fragments")
BIN_DIR = os.path.join(ROOT, "build", "qnx-vm-bin")
CORE_SRC = os.path.join(ROOT, "core", "src")
SCENARIO = "/proc/boot/rl-relais-failover.sh"

QCCFLAGS = ("-V gcc_ntox86_64 -std=c11 -O2 -Wall -Wextra -Werror -Wshadow "
            "-Wconversion -Wno-sign-conversion -Wno-conversion "
            "-D_POSIX_C_SOURCE=200809L")

CORE_FILES = sorted(f for f in os.listdir(CORE_SRC) if f.endswith(".c"))

# The guest binaries: (name, sources, extra includes, extra libs).
GUEST_BINARIES = [
    ("rl-node",
     ["host/node/relais-node.c", "host/measure/latency.c",
      "host/measure/latency_json.c"] + ["core/src/%s" % n for n in CORE_FILES],
     ["host/measure"], "-lsocket -lm"),
    ("rl-actuator",
     ["actuator/pc/relais-actuator.c", "actuator/journal_writer.c"]
     + ["core/src/%s" % n for n in CORE_FILES],
     ["actuator"], "-lsocket -lm"),
    ("rl-relais-failover",
     ["host/bench/guest/rl_relais_failover.c"]
     + ["core/src/%s" % n for n in CORE_FILES],
     [], "-lsocket -lm"),
    ("rl-verify",
     ["host/verify/relais-verify.c"]
     + ["core/src/%s" % n for n in CORE_FILES],
     [], "-lsocket -lm"),
]


def qcc_build(sources: list[str], out: str, includes: list[str],
              libs: str) -> str:
    """Cross-compile one guest binary with the real SDP qcc.

    -Wno-conversion is the only relaxation: the guest's `struct timeval` has
    an `int` microsecond field where the PC build has a `long` one, and the
    source is the same file on both sides.
    """
    script = sdp_env_script()
    os.makedirs(os.path.dirname(out), exist_ok=True)
    inc = " ".join("-I%s" % os.path.join(ROOT, i)
                   for i in ["core/include"] + includes)
    quoted = " ".join(shlex.quote(os.path.join(ROOT, s)) for s in sources)
    shell = (f". {shlex.quote(script)} >/dev/null 2>&1; "
             f"qcc {QCCFLAGS} {inc} {quoted} {libs} -o {shlex.quote(out)}")
    done = subprocess.run(["bash", "-c", shell], capture_output=True, text=True,
                          timeout=300, check=False)
    if done.returncode != 0:
        raise QnxVmError(f"qcc failed for {out}:\n{done.stderr[-2000:]}")
    return out


def prepare_guest_binaries() -> dict[str, str]:
    """Build the guest binaries and return {image_name: host_path}."""
    entries: dict[str, str] = {}
    for name, sources, includes, libs in GUEST_BINARIES:
        out = os.path.join(BIN_DIR, name)
        qcc_build(sources, out, includes, libs)
        entries[name] = out
    return entries


def guest_files() -> dict[str, str]:
    """Everything injected in the image: the guest binaries, the five
    binaries of target/ (M45) and the scenario script."""
    entries = prepare_guest_binaries()
    for name in ("relais-net", "relais-core", "relais-ctl", "relaisctl",
                 "relais-actuator"):
        entries[name] = os.path.join(ROOT, "target", "build", name)
    entries["rl-relais-failover.sh"] = os.path.join(
        FRAGMENTS, "rl-relais-failover.sh")
    missing = sorted(p for p in entries.values() if not os.path.exists(p))
    if missing:
        raise QnxVmError("missing files to inject: %s "
                         "(run `make -C target` first)" % missing)
    return entries


def build_image(vm_dir: str) -> None:
    entries = guest_files()
    create_vm(vm_dir)
    write_ifs_files(vm_dir, entries)
    rebuild_vm(vm_dir)


def parse_relais(text: str) -> dict:
    """The `rl-relais: ...` lines of the guest (one fact per line)."""
    return parse_marker_lines(text, "rl-relais: ")


def parse_failover(text: str) -> dict:
    """The `rl-relais-failover: ...` lines of the guest scenario."""
    return parse_marker_lines(text, "rl-relais-failover: ")


def summarize(vm: QnxVm, boot: float, status: int, output: str,
              run_dir: str, actuator: str, tag: str) -> dict:
    """One JSON shape for the run: what the host engaged, what the guest
    measured, and the files that prove it."""
    results = parse_relais(output)
    failover = parse_failover(output)
    summary = {
        "accel": vm.accel,
        "boot_seconds": round(boot, 1),
        "guest_status": status,
        "host_engaged": "QEMU 8.2.2 + KVM, QNX 8.0 guest, serial console",
        "guest_uname": _uname(output),
        "actuator": actuator,
        "relais": results,
        "failover": failover,
        "invariants": {
            "socle": failover.get("verify_socle", {}).get("violations"),
            "complet": failover.get("verify_complet", {}).get("violations"),
        },
        "host_loadavg": [round(x, 2) for x in os.getloadavg()],
        "transcript": transcript_path(run_dir),
    }
    with open(os.path.join(run_dir, "qemu-command.txt"), encoding="utf-8") as f:
        summary["qemu_command"] = f.read().strip()
    return summary


def _uname(output: str) -> str:
    """The guest's own uname line, as the guest printed it."""
    for line in output.splitlines():
        if line.startswith("QNX "):
            return line.strip()
    return ""


def write_evidence(summary: dict, run_dir: str, tag: str,
                   evidence_dir: Optional[str]) -> None:
    """Copy the transcript and the summary into the evidence directory."""
    if not evidence_dir:
        return
    os.makedirs(evidence_dir, exist_ok=True)
    transcript = os.path.join(run_dir, "console.log")
    if os.path.exists(transcript):
        with open(transcript, "rb") as src:
            data = src.read()
        with open(os.path.join(evidence_dir, f"{tag}-console.log"), "wb") as dst:
            dst.write(data)
    with open(os.path.join(evidence_dir, f"{tag}.json"), "w",
              encoding="utf-8") as out:
        json.dump(summary, out, indent=2)


def cmd_run(args: argparse.Namespace) -> int:
    """Build the image, boot the guest, play the scenario, collect."""
    build_image(args.vm_dir)
    actuator = "relais-actuator" if args.actuator == "qnx" else "rl-actuator"
    with QnxVm(args.vm_dir, args.run_dir) as vm:
        boot = vm.wait_shell(timeout=args.boot_timeout)
        print(f"guest shell up after {boot:.1f}s (accel={vm.accel})",
              file=sys.stderr)
        command = (f"{SCENARIO} {actuator} rl-node rl-relais-failover "
                   f"{args.ticks} '{args.params}' '{args.expect_second}'")
        output, status = vm.run(command, timeout=args.cmd_timeout)
    summary = summarize(vm, boot, status, output, args.run_dir, actuator, args.tag)
    out_path = os.path.join(args.run_dir, "relais.json")
    with open(out_path, "w", encoding="utf-8") as out:
        json.dump(summary, out, indent=2)
    write_evidence(summary, args.run_dir, args.tag, args.evidence)
    print(json.dumps(summary["relais"], indent=2))
    print(f"json: {out_path}")
    return 0 if status == 0 else 1


def _build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--vm-dir", default=DEFAULT_VM_DIR)
    ap.add_argument("--run-dir", default=DEFAULT_RUN_DIR)
    ap.add_argument("--boot-timeout", type=float, default=300.0)
    ap.add_argument("--cmd-timeout", type=float, default=180.0)
    ap.add_argument("--ticks", type=int, default=1200)
    ap.add_argument("--actuator", choices=("paced", "qnx"), default="paced",
                    help="paced: the actuator cross-compiled for QNX "
                         "(clock_nanosleep tempo); qnx: the actuator of target/ "
                         "(M45, kernel pulse tempo)")
    sub = ap.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="build, boot, play the scenario")
    run.add_argument("--ticks", type=int, default=1200)
    run.add_argument("--actuator", choices=("paced", "qnx"), default="paced",
                     help="paced: the actuator cross-compiled for QNX "
                          "(clock_nanosleep tempo); qnx: the actuator of "
                          "target/ (M45, kernel pulse tempo)")
    run.add_argument("--tag", default="qnx-m48-relais",
                     help="prefix of the evidence files")
    run.add_argument("--params", default="T=5,p=1,N=8",
                     help="the parameters of the protocol (T, p, N)")
    run.add_argument("--expect-second", default="B",
                     help="the node that must be certified before the kill "
                          "(empty: the kill does not wait for a certification)")
    run.add_argument("--evidence", default=None,
                     help="directory where the evidence is written")
    run.set_defaults(func=cmd_run)
    return ap


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        return args.func(args)
    except QnxVmError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
