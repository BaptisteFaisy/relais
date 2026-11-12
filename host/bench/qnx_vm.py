#!/usr/bin/env python3
"""QNX 8.0 VM driver (mission M46): build the image with mkqnximage, boot it
under QEMU with KVM when available, talk to the guest over the serial console,
capture the transcript, run commands inside the guest, and stop it cleanly so
no orphan VM is ever left behind.

Design notes (all verified on this host):
  * mkqnximage 8.0 builds the image into <vm-dir>/output/ (ifs.bin +
    disk-qemu.vmdk). Its own `--run` path requires a bridge and a setuid
    qemu-bridge-helper (root); we run QEMU ourselves instead, with the same
    artifacts, `-accel kvm` when /dev/kvm is writable, and root-free
    `-netdev user` networking. The accelerator actually used is reported.
  * File injection uses mkqnximage's snippet model: local/snippets/
    ifs_files.custom (plain [perms=0755] entries — never [+script] on ELF,
    see qnx_image.py) and ifs_start.custom (boot commands).
  * The serial console is a bidirectional Unix socket, so commands can be
    sent to the guest ksh (`on -d -t /dev/ser1 ksh -l` in the stock startup)
    and every byte read is appended to the transcript file (the evidence).

Reuses, zero duplication: qnx_env.find_sdp (SDP detection), qemu_config
(detect_kvm, check_no_root_needed, find_qemu_binary, MIN_PORT), qmp.QmpClient.

Requirement IDs: QNX-08 (boot-to-measure on real QNX), PERF-01 (measured
timings), OBS-04 (console transcript as evidence), INJ-07 (clean stop).

Image building (SDP/mkqnximage/qcc) lives in qnx_image.py.

Usage:
    python3 host/bench/qnx_vm.py build   [--vm-dir DIR] [--add NAME=HOST]...
    python3 host/bench/qnx_vm.py proof   [--vm-dir DIR] [--run-dir DIR]
    python3 host/bench/qnx_vm.py run     --cmd "pidin" [--vm-dir DIR]

Step 2 (failover banc harness, stub + real mapping): qnx_failover.py.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import time
from typing import Optional

try:  # package layout
    from .qmp import QmpClient
    from .qemu_config import (MIN_PORT, check_no_root_needed, detect_kvm,
                              find_qemu_binary)
    from .qnx_image import (QnxVmError, create_vm, qcc_compile, rebuild_vm,
                            write_ifs_files, write_ifs_start)
except ImportError:  # flat script layout
    from qmp import QmpClient
    from qemu_config import (MIN_PORT, check_no_root_needed, detect_kvm,
                             find_qemu_binary)
    from qnx_image import (QnxVmError, create_vm, qcc_compile, rebuild_vm,
                           write_ifs_files, write_ifs_start)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_VM_DIR = os.path.join(ROOT, "build", "qnx-vm")
DEFAULT_RUN_DIR = os.path.join(ROOT, "runs", "qnx-vm")
DEFAULT_MAC = "52:54:00:46:4d:36"          # "FM6" for mission 46
BOOT_TIMEOUT = 300.0                        # a QNX boot takes 1-3 min
CMD_TIMEOUT = 120.0
SENTINEL = "__RL_DONE__"
READY_SENTINEL = "__RL_SHELL_READY__"
BOOT_COMPLETE = "Startup complete"   # display_msg at end of ifs startup


def build_qnx_command(vm_dir: str, run_dir: str, accel: Optional[str] = None,
                      cpus: int = 2, ram: str = "1G",
                      hostfwd: Optional[list[str]] = None,
                      mac: str = DEFAULT_MAC,
                      binary: Optional[str] = None) -> tuple[list[str], str]:
    """The QEMU command line for a mkqnximage-built QNX VM.

    Returns (args, accel). accel is "kvm" only when /dev/kvm is really usable
    (qemu_config.detect_kvm); anything else is TCG and the caller must say so.
    """
    qemu = binary or find_qemu_binary()
    if not qemu:
        raise QnxVmError("qemu-system-x86_64 not installed")
    ifs = os.path.join(vm_dir, "output", "ifs.bin")
    disk = os.path.join(vm_dir, "output", "disk-qemu.vmdk")
    for artifact in (ifs, disk):
        if not os.path.exists(artifact):
            raise QnxVmError(f"{artifact} missing: run the build step first")
    probe = detect_kvm()
    used = accel or probe["accel"]
    if used == "kvm" and not probe["available"]:
        raise QnxVmError("kvm requested but unavailable: " + probe["reason"])
    args = [qemu, "-smp", str(cpus), "-m", ram]
    args += (["-enable-kvm", "-cpu", "host"] if used == "kvm"
             else ["-cpu", "max"])
    args += ["-kernel", ifs, "-drive", f"file={disk},if=ide,id=drv0"]
    args += _net_args(run_dir, mac, hostfwd)
    args += ["-object", "rng-random,filename=/dev/urandom,id=rng0",
             "-device", "virtio-rng-pci,rng=rng0",
             "-chardev", f"socket,id=ser0,path={serial_socket(run_dir)},"
                         "server=on,wait=off",
             "-serial", "chardev:ser0",
             "-qmp", f"unix:{qmp_socket(run_dir)},server=on,wait=off",
             "-display", "none", "-monitor", "none",
             "-pidfile", pid_file(run_dir)]
    problems = check_no_root_needed(args)
    if problems:
        raise QnxVmError("command line would need root: " + "; ".join(problems))
    return args, used


def _net_args(run_dir: str, mac: str, hostfwd: Optional[list[str]]) -> list[str]:
    """Root-free user networking; QNX stock startup loads vtnet_pci only."""
    for item in hostfwd or []:
        port = int(item.split(":")[-1].split("-")[0])
        if port < MIN_PORT:
            raise QnxVmError(f"hostfwd {item}: host port below {MIN_PORT}")
    fwd = "".join(f",hostfwd={item}" for item in (hostfwd or []))
    return ["-netdev", f"user,id=net0,host=10.0.2.2{fwd}",
            "-device", f"virtio-net-pci,netdev=net0,mac={mac}"]


def serial_socket(run_dir: str) -> str:
    return os.path.join(run_dir, "console.sock")


def qmp_socket(run_dir: str) -> str:
    return os.path.join(run_dir, "qmp.sock")


def pid_file(run_dir: str) -> str:
    return os.path.join(run_dir, "qemu.pid")


def transcript_path(run_dir: str) -> str:
    return os.path.join(run_dir, "console.log")


def split_sentinel(sentinel: str, at: int = 6) -> str:
    """Quote-split a sentinel: `echo "__RL""_X__"`.

    ksh concatenates adjacent quoted words, so the OUTPUT is the clean
    sentinel, while the ECHO of the typed input contains quote characters
    and can never match it. This is what makes console round-trips
    deterministic on a boot console shared with streaming boot messages.
    """
    return f'"{sentinel[:at]}""{sentinel[at:]}"'


class SerialConsole:
    """Bidirectional serial over the QEMU chardev Unix socket + transcript."""

    def __init__(self, path: str, transcript: str,
                 connect_wait: float = 30.0) -> None:
        self.path = path
        self.transcript = transcript
        self._sock: Optional[socket.socket] = None
        self._buffer = b""
        self._connect(connect_wait)
        self._log = open(transcript, "ab")

    def _connect(self, wait: float) -> None:
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(1.0)
                sock.connect(self.path)
                self._sock = sock
                return
            except OSError:
                sock.close()
                time.sleep(0.25)
        raise QnxVmError(f"serial socket {self.path} not up in {wait}s")

    def send(self, line: str) -> None:
        assert self._sock is not None
        self._sock.sendall((line + "\n").encode("utf-8"))

    def read_until(self, pattern: str, timeout: float) -> str:
        """Read (and transcript) until *pattern* appears; return all text."""
        assert self._sock is not None
        deadline = time.monotonic() + timeout
        text = self._buffer.decode("utf-8", "replace")
        while pattern not in text:
            left = deadline - time.monotonic()
            if left <= 0:
                raise QnxVmError(f"console timeout waiting for {pattern!r}; "
                                 f"tail of transcript:\n{text[-800:]}")
            self._sock.settimeout(min(left, 2.0))
            try:
                chunk = self._sock.recv(65536)
            except socket.timeout:
                continue
            except OSError as exc:
                raise QnxVmError(f"serial socket closed: {exc}") from exc
            if not chunk:
                raise QnxVmError("serial socket closed by QEMU (guest died?)")
            self._log.write(chunk)
            self._log.flush()
            text += chunk.decode("utf-8", "replace")
        index = text.find(pattern) + len(pattern)
        seen, self._buffer = text[:index], text[index:].encode("utf-8")
        return seen

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None
        self._log.close()


class QnxVm:
    """One QNX VM: start, wait for the shell, run commands, stop cleanly."""

    def __init__(self, vm_dir: str = DEFAULT_VM_DIR,
                 run_dir: str = DEFAULT_RUN_DIR, **kwargs) -> None:
        self.vm_dir = os.path.abspath(vm_dir)
        self.run_dir = os.path.abspath(run_dir)
        self.kwargs = kwargs
        self.process: Optional[subprocess.Popen] = None
        self.console: Optional[SerialConsole] = None
        self.qmp: Optional[QmpClient] = None
        self.accel = "tcg"
        self.boot_seconds: Optional[float] = None

    def start(self) -> "QnxVm":
        os.makedirs(self.run_dir, exist_ok=True)
        for stale in (serial_socket(self.run_dir), qmp_socket(self.run_dir),
                      pid_file(self.run_dir), transcript_path(self.run_dir)):
            if os.path.exists(stale):
                os.unlink(stale)
        args, self.accel = build_qnx_command(self.vm_dir, self.run_dir,
                                             **self.kwargs)
        with open(os.path.join(self.run_dir, "qemu-command.txt"), "w",
                  encoding="utf-8") as out:
            out.write(" ".join(shlex.quote(a) for a in args) + "\n")
        self._t0 = time.monotonic()
        self.process = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.PIPE)
        try:
            self.qmp = QmpClient(qmp_socket(self.run_dir))
            self.qmp.connect(wait=30.0)
            self.console = SerialConsole(serial_socket(self.run_dir),
                                         transcript_path(self.run_dir))
        except Exception:
            self.stop()
            raise
        return self

    def wait_shell(self, timeout: float = BOOT_TIMEOUT) -> float:
        """Wait for full boot ("Startup complete") then a shell round-trip.

        The sentinels are quote-split in the sent command (`echo "__RL""_X"`)
        so ksh's input echo never matches them: only the real output does.
        Returns the honest host-side boot time: QEMU spawn -> usable shell.
        """
        assert self.console is not None
        try:
            self.console.read_until(BOOT_COMPLETE, timeout=timeout)
        except QnxVmError:
            self._check_alive()
            raise QnxVmError(f"guest never printed {BOOT_COMPLETE!r} "
                             f"in {timeout}s (see {self.console.transcript})")
        deadline = time.monotonic() + 60.0
        while time.monotonic() < deadline:
            try:
                self.console.send(f"echo {split_sentinel(READY_SENTINEL)}")
                self.console.read_until(READY_SENTINEL, timeout=10.0)
            except QnxVmError:
                self._check_alive()
                continue
            self.boot_seconds = time.monotonic() - self._t0
            return self.boot_seconds
        self._check_alive()
        raise QnxVmError("guest booted but the shell does not answer")

    def _check_alive(self) -> None:
        if self.process is not None and self.process.poll() is not None:
            stderr = self.process.stderr
            tail = stderr.read().decode("utf-8", "replace") if stderr else ""
            raise QnxVmError(f"QEMU exited with {self.process.returncode}: "
                             f"{tail[-800:]}")

    def run(self, command: str, timeout: float = CMD_TIMEOUT) -> tuple[str, int]:
        """Run one command in the guest ksh; return (output, exit status).

        The done marker is quote-split so only the command's real output (with
        the exit status appended) can match, never ksh's echo of the input.
        """
        assert self.console is not None
        self.console.send(f"{command} ; echo {split_sentinel(SENTINEL, 9)}$?")
        text = self.console.read_until(SENTINEL, timeout=timeout)
        match = re.search(re.escape(SENTINEL) + r"(\d+)", text)
        if match is None:  # marker seen but status digits not arrived yet
            text += self.console.read_until("\n", timeout=5.0)
            match = re.search(re.escape(SENTINEL) + r"(\d+)", text)
        status = int(match.group(1)) if match else -1
        return text, status

    def stop(self, grace: float = 10.0) -> int:
        """Clean stop: QMP quit, then TERM, then KILL. Returns QEMU exit."""
        code = -1
        if self.qmp is not None and self.qmp.connected:
            try:
                self.qmp.execute("quit")
            except Exception:
                pass
        if self.process is not None:
            try:
                code = self.process.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    code = self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    code = self.process.wait(timeout=5)
        for closer in (self.console, self.qmp):
            if closer is not None:
                closer.close()
        self.console = self.qmp = None
        return code

    def __enter__(self) -> "QnxVm":
        return self.start()

    def __exit__(self, *_exc: object) -> None:
        self.stop()


def parse_marker_lines(text: str, prefix: str) -> dict:
    """Extract every `<prefix>key k=v k=v...` line from a transcript.

    Shared by the proof (`rl-proof:`) and failover (`rl-failover:`) harnesses:
    one grammar for every machine-readable guest output (OBS-04).
    """
    pattern = re.compile(re.escape(prefix) + r"(?P<key>\w+) (?P<rest>[^\r\n]+)")
    results: dict = {}
    for match in pattern.finditer(text):
        fields = {}
        for pair in match.group("rest").split():
            if "=" in pair:
                key, _, value = pair.partition("=")
                fields[key] = value
        results[match.group("key")] = fields
    return results


def parse_proof(text: str) -> dict:
    """rl-proof lines: uname, gettime_us, sleep_overshoot_us, done rc=N."""
    return parse_marker_lines(text, "rl-proof: ")


def cmd_build(args: argparse.Namespace) -> int:
    entries = dict(pair.split("=", 1) for pair in (args.add or []))
    create_vm(args.vm_dir)
    if entries:
        write_ifs_files(args.vm_dir, entries)
    print(rebuild_vm(args.vm_dir).strip().splitlines()[-1] or "built")
    print(f"image ready: {args.vm_dir}/output/ifs.bin")
    return 0


def prepare_proof(vm_dir: str) -> dict[str, str]:
    """Compile rl-proof and declare what to inject; shared by build/proof."""
    binary = os.path.join(ROOT, "build", "qnx-vm-bin", "rl-proof")
    qcc_compile(os.path.join(ROOT, "host", "bench", "guest", "qnx_proof.c"),
                binary)
    return {"rl-proof": binary}


def cmd_proof(args: argparse.Namespace) -> int:
    """The whole M46 step-1 loop: compile, image, boot (KVM), measure, stop."""
    entries = prepare_proof(args.vm_dir)
    if args.rebuild or not os.path.exists(
            os.path.join(args.vm_dir, "output", "ifs.bin")):
        create_vm(args.vm_dir)
        write_ifs_files(args.vm_dir, entries)
        rebuild_vm(args.vm_dir)
    with QnxVm(args.vm_dir, args.run_dir) as vm:
        boot = vm.wait_shell(timeout=args.boot_timeout)
        print(f"guest shell up after {boot:.1f}s (accel={vm.accel})")
        output, status = vm.run("rl-proof", timeout=args.cmd_timeout)
    results = parse_proof(output)
    summary = {"accel": vm.accel, "boot_seconds": round(boot, 1),
               "guest_status": status, "proof": results,
               "host_loadavg": [round(x, 2) for x in os.getloadavg()],
               "transcript": transcript_path(args.run_dir),
               "qemu_command": open(os.path.join(
                   args.run_dir, "qemu-command.txt"),
                   encoding="utf-8").read().strip()}
    with open(os.path.join(args.run_dir, "proof.json"), "w",
              encoding="utf-8") as out:
        json.dump(summary, out, indent=2)
    print(json.dumps(summary, indent=2))
    if status != 0 or results.get("done", {}).get("rc") != "0":
        raise QnxVmError("rl-proof did not complete in the guest "
                         f"(status={status}, results={results})")
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    """Boot an already-built VM, run one command, print output, stop."""
    with QnxVm(args.vm_dir, args.run_dir) as vm:
        boot = vm.wait_shell(timeout=args.boot_timeout)
        print(f"guest shell up after {boot:.1f}s (accel={vm.accel})",
              file=sys.stderr)
        output, status = vm.run(args.cmd, timeout=args.cmd_timeout)
        print(output)
    return 0 if status == 0 else 1


def _build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--vm-dir", default=DEFAULT_VM_DIR,
                    help="mkqnximage VM directory (default: build/qnx-vm)")
    ap.add_argument("--run-dir", default=DEFAULT_RUN_DIR,
                    help="run artifacts: sockets, transcript (runs/qnx-vm)")
    ap.add_argument("--boot-timeout", type=float, default=BOOT_TIMEOUT)
    ap.add_argument("--cmd-timeout", type=float, default=CMD_TIMEOUT)
    sub = ap.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build", help="create/rebuild the image")
    b.add_argument("--add", action="append", metavar="NAME=HOST",
                   help="file to inject (repeatable)")
    b.add_argument("--rebuild", action="store_true")
    b.set_defaults(func=cmd_build)
    p = sub.add_parser("proof", help="minimal proof loop (M46 step 1)")
    p.add_argument("--rebuild", action="store_true", default=True)
    p.set_defaults(func=cmd_proof)
    r = sub.add_parser("run", help="run one command in a booted guest")
    r.add_argument("--cmd", required=True)
    r.set_defaults(func=cmd_run)
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
