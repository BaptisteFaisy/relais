#!/usr/bin/env python3
"""Relais — boot-time measurement of a QEMU guest (QNX-08, idea 27).

What this measures, and what it does *not*
-------------------------------------------
The requirement QNX-08 asks for the delay from "kernel start" to "a certified
second is serving orders". That live moment only exists on the QNX target; it
cannot be produced on this PC (no SDP, no QNX image). This script therefore
provides two things:

1. `--linux-boot` — the part that IS measurable here, and nothing but it: the
   real wall-clock time for a QEMU guest to go from "QEMU launched" to
   "kernel first line" and to "userland ready", using the host's own Linux
   kernel and a tiny busybox initramfs this script builds. The figure is
   reported with the explicit caveat that it is **Linux, not QNX**, and so it
   is an order-of-magnitude reference for the emulator's own overhead, not a
   claim about the Relais target.

2. `--write-sample-report` — the exact procedure and a sample report template
   for the real QNX measurement, once the SDP image exists. The target-side
   timestamps come from slog2; the host-side ones from the bench clock.

The QNX procedure, step by step (runs only on a machine with the SDP 8.0)
--------------------------------------------------------------------------
a. Build the image:  `mkqnximage --type=qemu --target=x86_64 --boot
   --image image/qnx-ifs.img` (fragments in image/).
b. In the guest, record two slog2 markers with the kernel trace clock:
   - t0 = the first instruction of relais-actuator's main() (kernel start);
   - t1 = the moment the actuator has a certified second (the first STATUT that
     carries certified=true, i.e. the ACT-08 "half-way" marker).
   Both markers are already emitted as user events (OBS-03); read them with
   `slog2info`.
c. On the host, note the bench's monotonic clock at QEMU spawn (already stored
   as `guest.started_at` in host/bench/qemu.py) and at the first `certified`
   STATUT decoded on the PC (timestamp in the MCAP/journal).
d. QNX-08 = t1 - t0 (in-guest, authoritative) cross-checked against the
   host-side interval, so a frozen guest clock cannot fool the verdict (E3).

Expected report shape (sample):

    QNX-08  launch -> kernel            (host bench clock)   <X> ms
            kernel -> first STATUT      (slog2, in-guest)    <Y> ms
            kernel -> certified second  (slog2, in-guest)    <Z> ms  <- the number
            measured on  QNX 8.0 / x86_64, <image>, <date>, <N> runs (median)

Limits: TCG (no /dev/kvm) invalidates the timing (the layer reports the
acceleration mode in the verdict); a shared PC biases the two guests together.
"""

from __future__ import annotations

import argparse
import gzip
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

QEMU = shutil.which("qemu-system-x86_64") or "qemu-system-x86_64"
KERNEL_MARKER = "Linux version"
READY_MARKER = "RELAIS-BOOT-READY"
BOOT_APPEND = "console=ttyS0,115200 rdinit=/init loglevel=7"


def _err(msg: str) -> None:
    print(f"boot_measure: {msg}", file=sys.stderr)


def kvm_available() -> bool:
    """True when /dev/kvm is readable/writable; TCG otherwise."""
    try:
        return os.access("/dev/kvm", os.R_OK | os.W_OK)
    except OSError:
        return False


def find_kernel() -> str:
    """Newest readable vmlinuz in /boot (vmlinuz-7.0.0 is root-only here)."""
    candidates = []
    boot = os.environ.get("RELAIS_LINUX_BOOT", "/boot")
    if os.path.isdir(boot):
        for name in sorted(os.listdir(boot)):
            if name.startswith("vmlinuz-") and os.access(os.path.join(boot, name), os.R_OK):
                candidates.append(os.path.join(boot, name))
    return candidates[-1] if candidates else ""


def build_initramfs(workdir: str) -> str:
    """Build a minimal initramfs (busybox + /init) and return its path."""
    busybox = shutil.which("busybox")
    root = os.path.join(workdir, "initramfs")
    os.makedirs(os.path.join(root, "bin"), exist_ok=True)
    if busybox and os.path.exists(busybox):
        shutil.copy2(busybox, os.path.join(root, "bin", "busybox"))
    else:
        _err("busybox not found: building a bare initramfs (no userland shell)")
    init = os.path.join(root, "init")
    with open(init, "w", encoding="utf-8") as handle:
        handle.write(
            "#!/bin/busybox sh\n"
            "/bin/busybox mount -t proc proc /proc 2>/dev/null\n"
            f"echo {READY_MARKER}\n"
            "while :; do /bin/busybox sleep 1; done\n"
        )
    os.chmod(init, 0o755)
    out = os.path.join(workdir, "initramfs.cpio.gz")
    entries = []
    for base, _dirs, files in os.walk(root):
        for f in files:
            entries.append(os.path.join(base, f))
        for d in _dirs:
            entries.append(os.path.join(base, d))
    entries.append(root)
    with gzip.open(out, "wb") as gz:
        for full in sorted(set(entries)):
            rel = os.path.relpath(full, root)
            info = _cpio_newc_header(rel, full)
            gz.write(info[0])
            if info[1] is not None:
                with open(full, "rb") as fh:
                    gz.write(fh.read())
            if info[2]:
                gz.write(b"\0" * ((4 - info[2] % 4) % 4))
        gz.write(_cpio_trailer())
    return out


def _cpio_newc_header(rel: str, full: str):
    """Return (header_bytes, body_path|None, padding_for_body).

    cpio newc: name is "./" + relpath, or "." for the root; mode keeps the file
    type bits so the kernel sees /init as a regular executable.
    """
    st = os.lstat(full)
    mode = st.st_mode & 0o177777
    if os.path.isdir(full):
        size = 0
        body = None
    else:
        size = st.st_size
        body = full
    name = (b"." if rel == "." else b"./" + rel.encode("utf-8", "surrogateescape")) + b"\0"
    fields = [
        "070701",
        f"{st.st_ino:08x}", f"{mode:08x}", f"{st.st_uid & 0xffffffff:08x}",
        f"{st.st_gid & 0xffffffff:08x}", "00000001",
        f"{int(st.st_mtime):08x}", f"{size:08x}", "00000000",
        "00000000", "00000000", "00000000", f"{len(name):08x}", "00000000",
    ]
    header = b"".join(f.encode("ascii") for f in fields) + name
    header += b"\0" * ((4 - len(header) % 4) % 4)
    return header, body, size


def _cpio_trailer() -> bytes:
    name = b"TRAILER!!!\0"
    fields = ["070701", "00000000", "00000000", "00000000", "00000000",
              "00000001", "00000000", "00000000", "00000000", "00000000",
              "00000000", "00000000", f"{len(name):08x}", "00000000"]
    header = b"".join(f.encode("ascii") for f in fields) + name
    return header + b"\0" * ((4 - len(header) % 4) % 4)


class _Scanner:
    """Incremental marker scan: reads only new bytes, with a small overlap so a
    marker split across two reads is still caught. Avoids the O(n^2) cost of
    re-reading a growing serial log, which would inflate the measured times."""

    def __init__(self, path: str, marker: str) -> None:
        self.path = path
        self.marker = marker.encode()
        self.pos = 0

    def hit(self) -> bool:
        if not os.path.exists(self.path):
            return False
        start = max(0, self.pos - len(self.marker))
        with open(self.path, "rb") as handle:
            handle.seek(start)
            chunk = handle.read()
        self.pos = start + len(chunk)
        return self.marker in chunk


def measure_one(kernel: str, initramfs: str, workdir: str, serial: str):
    """Boot once and return {launch_kernel, launch_ready, kernel_ready} ms."""
    accel = "-enable-kvm" if kvm_available() else "-accel tcg,thread=multi"
    cmd = [
        QEMU, "-m", "512", "-smp", "1", accel, "-nographic", "-no-reboot",
        "-kernel", kernel, "-initrd", initramfs, "-append", BOOT_APPEND,
        "-serial", f"file:{serial}", "-monitor", "none",
    ]
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, cwd=workdir, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    kernel_t = ready_t = None
    kscan = _Scanner(serial, KERNEL_MARKER)
    rscan = _Scanner(serial, READY_MARKER)
    try:
        while proc.poll() is None:
            if kernel_t is None and kscan.hit():
                kernel_t = time.monotonic()
            if ready_t is None and rscan.hit():
                ready_t = time.monotonic()
                break
            time.sleep(0.005)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
    return {
        "launch_kernel_ms": None if kernel_t is None else (kernel_t - t0) * 1000,
        "launch_ready_ms": None if ready_t is None else (ready_t - t0) * 1000,
        "kernel_ready_ms": (None if kernel_t is None or ready_t is None
                            else (ready_t - kernel_t) * 1000),
    }


def _median(values):
    return statistics.median([v for v in values if v is not None]) if values else None


def run_linux_boot(runs: int) -> int:
    kernel = find_kernel()
    if not kernel:
        _err("no readable vmlinuz under /boot; set RELAIS_LINUX_BOOT")
        return 2
    print(f"kernel: {kernel}")
    print(f"accel : {'kvm' if kvm_available() else 'tcg (no /dev/kvm — timings invalid)'}\n")
    results = []
    with tempfile.TemporaryDirectory(prefix="relais-boot-") as workdir:
        initramfs = build_initramfs(workdir)
        for run in range(1, runs + 1):
            serial = os.path.join(workdir, f"serial-{run}.log")
            r = measure_one(kernel, initramfs, workdir, serial)
            results.append(r)
            print(f"run {run}: launch->kernel {r['launch_kernel_ms']:.1f} ms   "
                  f"launch->ready {r['launch_ready_ms']:.1f} ms   "
                  f"kernel->ready {r['kernel_ready_ms']:.1f} ms")
    print("\nQNX-08 reference on QEMU — Linux guest, NOT QNX:")
    print(f"  launch -> kernel     median {_median([r['launch_kernel_ms'] for r in results]):.1f} ms")
    print(f"  launch -> ready      median {_median([r['launch_ready_ms'] for r in results]):.1f} ms")
    print(f"  kernel -> ready      median {_median([r['kernel_ready_ms'] for r in results]):.1f} ms")
    print("  (this is the emulator's own overhead for a Linux guest; the QNX\n"
          "   'kernel -> certified second' number must come from slog2 on target)")
    return 0


def write_sample_report(path: str) -> int:
    text = """# QNX-08 measurement report (sample — not yet measured on this PC)

## 1. What is measured
The delay from the QNX kernel start to the moment a certified second is
serving orders. Two independent clock sources are cross-checked:

| Source | Clock | Marker |
|---|---|---|
| host bench | monotonic (Python) | QEMU spawn -> first certified STATUT decoded |
| guest/slog2 | kernel trace clock | relais-actuator main() -> first certified STATUT |

## 2. Procedure
1. `mkqnximage --type=qemu --target=x86_64 --boot --image image/qnx-ifs.img`
2. `RELAIS_QNX_IMAGE=image/qnx-ifs.img python3 host/bench/relais_bench.py --probe E6`
3. In-guest: `slog2info` dump, take t0 (actuator first line) and t1 (first
   `certified=true` STATUT, ACT-08).
4. Host: read `guest.started_at` in host/bench/qemu.py and the journal timestamp
   of the first certified STATUT.

## 3. Sample
```
QNX-08  kernel -> certified second  12.3 ms  (median of 20 runs)
        launch -> kernel            21.0 ms  (QEMU + SeaBIOS + kernel, host clock)
        measured on QNX 8.0 / x86_64, <image>, <date>, KVM
```

## 4. Accept criterion
The certified second must appear within one certified-ticks window after the
actuator resumes; the exact bound is `T * (certify_ticks + testimony N)`. Any
number larger than that is a finding, not a pass.
"""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    print(f"wrote {path}")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="QNX-08 boot-time measurement")
    parser.add_argument("--linux-boot", action="store_true",
                        help="measure a Linux QEMU guest boot (the part runnable here)")
    parser.add_argument("--runs", type=int, default=3,
                        help="number of boot runs for the median (default 3)")
    parser.add_argument("--write-sample-report", metavar="FILE",
                        help="write the QNX measurement report template to FILE")
    args = parser.parse_args(argv)
    if args.write_sample_report:
        return write_sample_report(args.write_sample_report)
    if args.linux_boot:
        return run_linux_boot(args.runs)
    parser.print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())