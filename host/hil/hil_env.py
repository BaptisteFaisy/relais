#!/usr/bin/env python3
"""Relais — shared helpers of the HIL benches (mission M41).

Everything here is host glue: process launching, journal tailing with wall
clock timestamps, offline invariant verification and percentiles. The HIL
benches cut REAL processes and REAL datagram paths; nothing in this module
simulates a fault (the simulated fault model lives in host/simulate/).

Requirements: INJ-02 (death), INJ-03 (freeze/thaw), INJ-04 (link cut),
INJ-08 (sweep), OBS-07 (offline verification of the journal).
"""
import json
import os
import socket
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def binary(name):
    path = os.path.join(ROOT, "build", name)
    if not os.path.exists(path):
        raise RuntimeError("missing %s: run `make` first" % path)
    return path


def free_port():
    """Ask the kernel for an unused UDP port (small race window, retried by
    the caller: a failed bind simply invalidates the run)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def start(cmd, log_path, line_buffered=False):
    """Launch one bench process and return (Popen, log file handle).

    line_buffered runs the process under `stdbuf -oL` so its journal lines
    reach the file as they are produced (the HIL tail measures wall clock).
    Only processes started here are ever signalled: the benches keep their
    Popen handles and kill nothing else on the machine.
    """
    if line_buffered:
        cmd = ["stdbuf", "-oL"] + cmd
    log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True, text=True)
    return proc, log


class JournalTailer:
    """Incremental reader of the actuator journal (JSON lines).

    Every record is stamped with the wall clock instant it was SEEN by this
    process; with the actuator under `stdbuf -oL` that is a close upper bound
    of the instant the actuator wrote it (flush + ~1 ms poll).
    """

    def __init__(self, path):
        self.path = path
        self._buf = ""
        self._fh = None

    def read_new(self):
        """Return [(record, monotonic_ns_seen), ...] for the lines appended
        since the last call. The whole pending buffer is consumed in one go:
        a partial consumption would date the cut on an outdated tick."""
        out = []
        if self._fh is None:
            if not os.path.exists(self.path):
                return out
            self._fh = open(self.path, encoding="utf-8")
        while True:
            chunk = self._fh.read(65536)
            if not chunk:
                break
            self._buf += chunk
            while "\n" in self._buf:
                line, self._buf = self._buf.split("\n", 1)
                if not line.strip():
                    continue
                try:
                    out.append((json.loads(line), time.monotonic_ns()))
                except ValueError:
                    continue  # truncated line: never happens with -oL, be safe
        return out

    def close(self):
        if self._fh is not None:
            self._fh.close()
            self._fh = None


def wait_for(tailer, predicate, timeout_s, procs=None, tick=None):
    """Poll the journal until predicate(record) is true; return
    (record, ns_seen) or None on timeout. Aborts early if a process in
    `procs` died. `tick` optionally waits for record tick >= value first."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for rec, ns in tailer.read_new():
            if tick is not None and rec.get("tick", 0) < tick:
                continue
            if predicate(rec):
                return rec, ns
        if procs and all(p.poll() is not None for p in procs):
            for rec, ns in tailer.read_new():
                if predicate(rec):
                    return rec, ns
            return None
        time.sleep(0.001)
    return None


def cleanup(procs, grace_s=1.0):
    """Stop bench processes: only our own children, killed outright (SIGKILL)
    so a frozen machine never "comes back" by accident after the run."""
    for proc in procs:
        if proc.poll() is None:
            proc.kill()
    for proc in procs:
        try:
            proc.wait(timeout=grace_s)
        except subprocess.TimeoutExpired:
            pass


def percentile(values, pct):
    """Nearest-rank percentile over a non-empty list of numbers."""
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (pct / 100.0) * (len(ordered) - 1)
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    frac = rank - low
    return ordered[low] * (1 - frac) + ordered[high] * frac


def distribution(values):
    """p50/p95/p99/max/min/mean of a sample; None on empty input."""
    if not values:
        return None
    return {
        "n": len(values),
        "min": min(values),
        "p50": percentile(values, 50),
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
        "max": max(values),
        "mean": sum(values) / len(values),
    }
