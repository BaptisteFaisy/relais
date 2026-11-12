#!/usr/bin/env python3
"""Relais — aggregate the fuzzing runs into a markdown report (ROB-09).

Reads the logs written by `make fuzz` / `make fuzz-long`, whichever backend ran
(libFuzzer when clang is available, the home-grown seeded generator otherwise),
counts the executions and the crashes, lists the cases kept under
host/fuzz/corpus/crashes/, minimises them with the target binary itself, and
writes the result into docs/RESULTS.md between two markers so the section can be
regenerated without touching the rest of the file.

Standard library only. Usage:
    python3 tools/fuzz-report.py [--logs DIR] [--crashes DIR] [--bin DIR]
                                 [--results FILE] [--no-minimize]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time
from typing import Dict, List, Optional, Tuple

BEGIN = "<!-- FUZZ-REPORT-BEGIN -->"
END = "<!-- FUZZ-REPORT-END -->"
TARGETS = ("fuzz_frame", "fuzz_journal", "fuzz_arbiter")

SUMMARY_RE = re.compile(
    r"FUZZ-SUMMARY target=(?P<target>\S+) backend=(?P<backend>\S+) "
    r"execs=(?P<execs>\d+) time=(?P<time>[\d.]+) exec_per_s=(?P<rate>[\d.]+)"
)
STAT_UNITS_RE = re.compile(r"stat::number_of_executed_units:\s*(\d+)")
STAT_RATE_RE = re.compile(r"stat::average_exec_per_sec:\s*(\d+)")
STAT_NEW_RE = re.compile(r"stat::new_units_added:\s*(\d+)")
COV_RE = re.compile(r"\bcov:\s*(\d+)")
DONE_RE = re.compile(r"Done (\d+) runs in ([\d.]+) second")
CRASH_MARKERS = (
    "ERROR: AddressSanitizer",
    "ERROR: UndefinedBehaviorSanitizer",
    "ERROR: libFuzzer: deadly signal",
    "runtime error:",
    "FUZZ-CONTRACT-VIOLATION",
    "FUZZ-CRASH",
)


class Run:
    """One target, one log file, one backend."""

    def __init__(self, target: str, log: str) -> None:
        self.target = target
        self.log = log
        self.backend = "unknown"
        self.execs = 0
        self.seconds = 0.0
        self.rate = 0.0
        self.cov: Optional[int] = None
        self.new_units = 0
        self.crashed = False
        self.reason = ""

    def parse(self) -> None:
        try:
            with open(self.log, "r", errors="replace") as f:
                text = f.read()
        except OSError:
            return
        for line in text.splitlines():
            m = SUMMARY_RE.search(line)
            if m:
                self.backend = m.group("backend")
                self.execs = int(m.group("execs"))
                self.seconds = float(m.group("time"))
                self.rate = float(m.group("rate"))
            for rx, attr in ((STAT_UNITS_RE, "execs"), (STAT_NEW_RE, "new_units")):
                g = rx.search(line)
                if g:
                    setattr(self, attr, int(g.group(1)))
                    self.backend = "libfuzzer"
            g = STAT_RATE_RE.search(line)
            if g:
                self.rate = float(g.group(1))
                self.backend = "libfuzzer"
            g = COV_RE.search(line)
            if g:
                self.cov = int(g.group(1))
            g = DONE_RE.search(line)
            if g:
                self.execs = max(self.execs, int(g.group(1)))
                self.seconds = max(self.seconds, float(g.group(2)))
        for marker in CRASH_MARKERS:
            if marker in text:
                self.crashed = True
                self.reason = marker
                break
        if self.backend == "unknown":
            self.backend = "libfuzzer" if "libFuzzer" in text else "homegrown"
        if self.seconds > 0 and self.rate == 0:
            self.rate = self.execs / self.seconds


def collect_runs(logs_dir: str) -> List[Run]:
    runs: List[Run] = []
    if not os.path.isdir(logs_dir):
        return runs
    for name in sorted(os.listdir(logs_dir)):
        if not name.endswith(".log"):
            continue
        target = name[:-4].replace("-long", "")
        run = Run(target, os.path.join(logs_dir, name))
        run.parse()
        runs.append(run)
    return runs


def list_crashes(crashes_dir: str) -> List[str]:
    if not os.path.isdir(crashes_dir):
        return []
    out = []
    for name in sorted(os.listdir(crashes_dir)):
        path = os.path.join(crashes_dir, name)
        if os.path.isfile(path) and os.path.getsize(path) > 0:
            out.append(path)
    return out


def target_for(path: str, bin_dir: str) -> Optional[str]:
    name = os.path.basename(path)
    for t in TARGETS:
        if t in name:
            return t
    for t in TARGETS:
        if os.path.exists(os.path.join(bin_dir, t)):
            return t
    return None


def crashes(binary: str, data: bytes, max_len: int) -> bool:
    """True when this input still makes the target fail."""
    if not data:
        return False
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        tmp.write(data)
        tmp_path = tmp.name
    try:
        proc = subprocess.run(
            [binary, "--input", tmp_path, "--max-len", str(max_len)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=30,
            check=False,
        )
        return proc.returncode not in (0, 2)
    except (subprocess.TimeoutExpired, OSError):
        return True
    finally:
        os.unlink(tmp_path)


def ddmin(binary: str, data: bytes, max_len: int, budget: int = 400) -> bytes:
    """Delta debugging: drop chunks, then bytes, while the failure reproduces."""
    calls = 0
    n = 2
    while len(data) > 1 and calls < budget:
        size = max(1, len(data) // n)
        parts = [data[i : i + size] for i in range(0, len(data), size)]
        shrank = False
        for i in range(len(parts)):
            calls += 1
            if calls >= budget:
                break
            cand = b"".join(parts[:i] + parts[i + 1 :])
            if crashes(binary, cand, max_len):
                data, n, shrank = cand, max(2, n - 1), True
                break
        if not shrank:
            if n >= len(data):
                break
            n = min(len(data), n * 2)
    return data


def minimise(path: str, bin_dir: str, out_dir: str, max_len: int) -> Optional[Tuple[str, int, int]]:
    target = target_for(path, bin_dir)
    if target is None:
        return None
    binary = os.path.join(bin_dir, target)
    if not os.path.exists(binary):
        return None
    with open(path, "rb") as f:
        data = f.read()
    if not crashes(binary, data, max_len):
        return None
    small = ddmin(binary, data, max_len)
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "min-" + os.path.basename(path))
    with open(out, "wb") as f:
        f.write(small)
    return out, len(data), len(small)


def digest(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()[:16]


def count_seeds(corpus: str) -> Dict[str, int]:
    out: Dict[str, int] = {}
    for name in TARGETS:
        short = name.replace("fuzz_", "")
        d = os.path.join(corpus, short)
        out[short] = len([n for n in os.listdir(d)]) if os.path.isdir(d) else 0
    return out


def render(runs: List[Run], crashes_paths: List[str], minimised: List[Tuple[str, int, int]],
           backend: str, corpus: str) -> str:
    lines = [BEGIN, "## Fuzzing and sanitizers (ROB-09, SEC-02, MAN-11)", ""]
    lines.append("_Generated by `tools/fuzz-report.py` on %s UTC._" % time.strftime("%Y-%m-%d %H:%M"))
    lines.append("")
    lines.append("Backend: **%s**. %s" % (backend, backend_note(backend)))
    lines.append("")
    lines.append("| target | execs | time (s) | exec/s | coverage | result |")
    lines.append("|---|---:|---:|---:|---:|---|")
    total = 0
    for r in runs:
        total += r.execs
        cov = "n/a" if r.cov is None else str(r.cov)
        verdict = "CRASH (%s)" % r.reason if r.crashed else "clean"
        lines.append("| `%s` | %s | %.1f | %s | %s | %s |"
                     % (r.target, format(r.execs, ","), r.seconds, format(int(r.rate), ","),
                        cov, verdict))
    lines.append("| **total** | **%s** | | | | %d crash(es) |"
                 % (format(total, ","), sum(1 for r in runs if r.crashed)))
    lines.append("")
    seeds = count_seeds(corpus)
    lines.append("Corpus: %d seeds (%s), generated by `host/fuzz/gen_corpus.c` from the shared "
                 "encoder and from a real simulator journal."
                 % (sum(seeds.values()), ", ".join("%s=%d" % kv for kv in sorted(seeds.items()))))
    lines.append("")
    if crashes_paths:
        lines.append("### Cases kept")
        lines.append("")
        lines.append("| case | bytes | sha256/16 |")
        lines.append("|---|---:|---|")
        for p in crashes_paths:
            lines.append("| `%s` | %d | `%s` |" % (os.path.relpath(p), os.path.getsize(p), digest(p)))
        lines.append("")
        if minimised:
            lines.append("Minimised by delta debugging with the target binary itself:")
            lines.append("")
            for out, before, after in minimised:
                lines.append("- `%s`: %d -> %d bytes" % (os.path.relpath(out), before, after))
            lines.append("")
    else:
        lines.append("Cases kept: none — no input reached a sanitizer error or broke a contract.")
        lines.append("")
    lines.append("Reproduce: `make fuzz` (60 s per target, CI budget) or `make fuzz-long` "
                 "(24 h, the ROB-09 run). Replay one case: "
                 "`./build/fuzz/<target> --input <case> --max-len 4096`.")
    lines.append(END)
    return "\n".join(lines) + "\n"


def backend_note(backend: str) -> str:
    if backend == "libfuzzer":
        return "Coverage-guided libFuzzer with `-fsanitize=fuzzer,address,undefined`."
    return ("clang/libFuzzer was absent, so the targets ran under the home-grown seeded "
            "generator (`host/fuzz/fuzz_driver.c`) with gcc `-fsanitize=address,undefined "
            "-fno-sanitize-recover=undefined`; coverage is not instrumented in that mode.")


def update_results(path: str, section: str) -> bool:
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        text = ""
    if BEGIN in text and END in text:
        head, rest = text.split(BEGIN, 1)
        tail = rest.split(END, 1)[1]
        text = head + section.rstrip("\n") + tail
    else:
        text = text.rstrip("\n") + "\n\n" + section
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return True


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="aggregate the Relais fuzzing runs")
    ap.add_argument("--logs", default="host/fuzz/logs")
    ap.add_argument("--crashes", default="host/fuzz/corpus/crashes")
    ap.add_argument("--corpus", default="host/fuzz/corpus")
    ap.add_argument("--bin", default="build/fuzz")
    ap.add_argument("--backend", default="")
    ap.add_argument("--results", default="")
    ap.add_argument("--max-len", type=int, default=4096)
    ap.add_argument("--no-minimize", action="store_true")
    args = ap.parse_args(argv)

    runs = collect_runs(args.logs)
    if not runs:
        print("fuzz-report: no log under %s — run `make fuzz` first" % args.logs, file=sys.stderr)
        return 1
    paths = list_crashes(args.crashes)
    minimised: List[Tuple[str, int, int]] = []
    if paths and not args.no_minimize:
        for p in paths:
            got = minimise(p, args.bin, os.path.join(args.crashes, "min"), args.max_len)
            if got:
                minimised.append(got)
    backend = args.backend or runs[0].backend
    section = render(runs, paths, minimised, backend, args.corpus)
    print(section)
    if args.results:
        update_results(args.results, section)
        print("fuzz-report: %s updated" % args.results, file=sys.stderr)
    return 2 if any(r.crashed for r in runs) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
