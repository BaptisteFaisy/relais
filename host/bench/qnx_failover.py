#!/usr/bin/env python3
"""Relais banc-in-the-guest harness (mission M46, step 2).

Two modes, one harness (fault injected from inside the guest, takeover delay
measured on the guest clock, journal = the console transcript recovered on the
host):

  stub    VALIDATED mode. A substitute binary (host/bench/guest/
          qnx_failover_stub.c) plays holder + second over 127.0.0.1 UDP; the
          scenario (image/fragments/rl-failover.sh) SIGKILLs the holder from
          inside the guest and the second reports `rl-failover:
          takeover_us=...`. This proves the harness mechanics only — it is
          NOT a protocol measurement (the protocol lives in core/ and runs
          through the target/ binaries).

  relais  REAL mode, BLOCKED(target/ does not compile with qcc yet — see
          M45). The mapping image/fragments/vm_files.relais is ready: when
          `make -C target` produces the five binaries, this mode injects them
          and refuses to fake anything until then.

Usage:
    python3 host/bench/qnx_failover.py stub   [--vm-dir DIR] [--run-dir DIR]
    python3 host/bench/qnx_failover.py relais [--vm-dir DIR]

Requirement IDs: INJ-01 (kill from inside the guest), OBS-04 (transcript),
PERF-02 (takeover delay measured, never invented), QNX-06 (restart/relève).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Optional

try:  # package layout
    from .qnx_image import (QnxVmError, create_vm, qcc_compile, rebuild_vm,
                            write_ifs_files)
    from .qnx_vm import (DEFAULT_RUN_DIR, DEFAULT_VM_DIR, QnxVm, ROOT,
                         parse_marker_lines, transcript_path)
except ImportError:  # flat script layout
    from qnx_image import (QnxVmError, create_vm, qcc_compile, rebuild_vm,
                           write_ifs_files)
    from qnx_vm import (DEFAULT_RUN_DIR, DEFAULT_VM_DIR, QnxVm, ROOT,
                        parse_marker_lines, transcript_path)

FRAGMENTS = os.path.join(ROOT, "image", "fragments")
STUB_SRC = os.path.join(ROOT, "host", "bench", "guest", "qnx_failover_stub.c")
STUB_BIN = os.path.join(ROOT, "build", "qnx-vm-bin", "rl-fail")
SCENARIO = "/proc/boot/rl-failover.sh"


def parse_failover(text: str) -> dict:
    """rl-failover lines: holder/second up, killing, takeover_us, done rc."""
    return parse_marker_lines(text, "rl-failover: ")


def load_vm_files(path: str, root: str = ROOT) -> dict[str, str]:
    """Parse a `name=host/path` mapping file (comments and blanks ignored).

    Relative host paths are resolved against the repository root; absolute
    paths are kept as-is.
    """
    entries: dict[str, str] = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if not line or "=" not in line:
                continue
            name, _, host = line.partition("=")
            host = host.strip()
            entries[name.strip()] = (host if os.path.isabs(host)
                                     else os.path.join(root, host))
    return entries


def build_stub() -> str:
    """Cross-compile the substitute binary (needs -lsocket on QNX)."""
    return qcc_compile(STUB_SRC, STUB_BIN, libs="-lsocket")


def inject_and_build(vm_dir: str, entries: dict[str, str]) -> None:
    """Shared injection path: snippets -> mkqnximage rebuild."""
    missing = sorted(name for name, host in entries.items()
                     if not os.path.exists(host))
    if missing:
        raise QnxVmError(f"cannot inject, files missing for: {missing}")
    create_vm(vm_dir)
    write_ifs_files(vm_dir, entries)
    rebuild_vm(vm_dir)


def summarize(vm: QnxVm, boot: float, status: int, results: dict,
              run_dir: str, extra: Optional[dict] = None) -> dict:
    """One JSON shape for every guest measurement run (honesty fields)."""
    summary = {"accel": vm.accel, "boot_seconds": round(boot, 1),
               "guest_status": status, "results": results,
               "host_loadavg": [round(x, 2) for x in os.getloadavg()],
               "transcript": transcript_path(run_dir)}
    summary.update(extra or {})
    return summary


def cmd_stub(args: argparse.Namespace) -> int:
    """Validate the whole harness with the substitute binary."""
    build_stub()
    entries = load_vm_files(os.path.join(FRAGMENTS, "vm_files.failover"))
    inject_and_build(args.vm_dir, entries)
    with QnxVm(args.vm_dir, args.run_dir) as vm:
        boot = vm.wait_shell(timeout=args.boot_timeout)
        print(f"guest shell up after {boot:.1f}s (accel={vm.accel})",
              file=sys.stderr)
        output, status = vm.run(SCENARIO, timeout=args.cmd_timeout)
    results = parse_failover(output)
    summary = summarize(vm, boot, status, results, args.run_dir,
                        {"mode": "stub",
                         "note": "harness mechanics only, NOT the protocol; "
                                 "real banc BLOCKED on M45"})
    _write_json(args.run_dir, "failover.json", summary)
    takeover = results.get("takeover", {}).get("takeover_us")
    if status != 0 or takeover is None or \
            results.get("done", {}).get("rc") != "0":
        raise QnxVmError(f"stub scenario did not complete (status={status}, "
                         f"results={results})")
    print(json.dumps(summary, indent=2))
    return 0


def cmd_relais(args: argparse.Namespace) -> int:
    """Inject the REAL target/ binaries — refuses to run until M45 lands."""
    entries = load_vm_files(os.path.join(FRAGMENTS, "vm_files.relais"))
    missing = sorted(host for host in entries.values()
                     if not os.path.exists(host))
    if missing:
        raise QnxVmError(
            "BLOCKED(target/ does not compile with qcc yet — see M45). "
            f"Missing binaries: {missing}. Once `make -C target` succeeds, "
            "this mode injects them with the same harness as `stub`.")
    inject_and_build(args.vm_dir, entries)
    print("target binaries injected; the real in-guest scenario script is "
          "the next step once M45 lands (see docs/QNX-RUN.md).")
    return 0


def _write_json(run_dir: str, name: str, payload: dict) -> None:
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(run_dir, name), "w", encoding="utf-8") as out:
        json.dump(payload, out, indent=2)


def _build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--vm-dir", default=DEFAULT_VM_DIR)
    ap.add_argument("--run-dir", default=DEFAULT_RUN_DIR)
    ap.add_argument("--boot-timeout", type=float, default=300.0)
    ap.add_argument("--cmd-timeout", type=float, default=120.0)
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("stub", help="validated harness with substitute binary"
                   ).set_defaults(func=cmd_stub)
    sub.add_parser("relais", help="real banc (BLOCKED until M45)"
                   ).set_defaults(func=cmd_relais)
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
