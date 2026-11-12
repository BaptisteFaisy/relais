#!/usr/bin/env python3
"""wcet_lib.py — shared helpers of the M40 WCET harness (host/wcet/).

Used by measure.py, rta.py and target-report.py so that one definition exists
for each of: the repository root, the percentile formula, the deadline budget
and the report tables (rule 3: zero duplication).

The statistics come from host/timing/wcet.py, imported by path: the percentiles,
the dispersion and the Gumbel-POT extrapolation published here are computed by
the SAME code that host/timing/test_timing.py already pins down. Nothing is
re-implemented, and the C probe (host/wcet/probe.c) is cross-checked against it.

Standard library only (rule 6). Requirements: PERF-01, PERF-07, PERF-08,
QNX-04, OBS-03.
"""
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# The published budget of PERF-01 (docs/SPEC-2.md): "from the STATUT received by
# the machine to the order emitted: 1 ms at the 99.9th percentile, 2 ms in the
# worst case, under a processor load of lower priority".
PERF01_P999_US = 1000.0
PERF01_WORST_US = 2000.0
# Cycle rule of spec 4.2 (docs/TIMING.md §4): 2*Delta + C + J <= T.
DEFAULT_PERIOD_US = 5000.0
DEFAULT_DELTA_US = 1000.0   # one-way frame delay, to measure on target (E2)
DEFAULT_JITTER_US = 1000.0  # release jitter, refined by this harness

# The five segments of the probe, in the order they are published.
SEGMENTS = ("path", "decode", "node", "law", "encode")
# Map from a probe segment to the QNX stage that runs it (docs/TIMING.md §1).
SEGMENT_STAGE = {
    "decode": "relais-net",
    "encode": "relais-net",
    "node": "relais-core",
    "law": "relais-ctl",
}
STAGE_PRIORITY = {"relais-net": 62, "relais-core": 61, "relais-ctl": 60}


def repo_root(start=None):
    """Walk up to the directory holding CONTRACT.md (the repository root)."""
    path = os.path.abspath(start or HERE)
    while True:
        if os.path.exists(os.path.join(path, "CONTRACT.md")):
            return path
        parent = os.path.dirname(path)
        if parent == path:
            return os.path.abspath(start or HERE)
        path = parent


ROOT = repo_root()


def load_timing_wcet():
    """Import host/timing/wcet.py by path: one percentile implementation."""
    path = os.path.join(ROOT, "host", "timing", "wcet.py")
    if not os.path.exists(path):
        raise RuntimeError("host/timing/wcet.py is missing (expected at %s)" % path)
    spec = importlib.util.spec_from_file_location("timing_wcet", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_timing_rta():
    """Import host/timing/rta.py by path: one Audsley fixed point, reused."""
    path = os.path.join(ROOT, "host", "timing", "rta.py")
    if not os.path.exists(path):
        raise RuntimeError("host/timing/rta.py is missing (expected at %s)" % path)
    spec = importlib.util.spec_from_file_location("timing_rta", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


TIMING = load_timing_wcet()
percentile = TIMING.percentile
describe = TIMING.describe
gumbel_pot_bound = TIMING.gumbel_pot_bound
load_samples = TIMING.load_samples
TIMING_RTA = load_timing_rta()


def host_label():
    """Stable label of the measuring machine: <sysname>-<machine>."""
    try:
        u = os.uname()
        return "%s-%s" % (u.sysname.lower(), u.machine)
    except AttributeError:  # pragma: no cover - non POSIX
        return "unknown"


def read_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def write_json(path, payload):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, sort_keys=True)
        fh.write("\n")


def fmt_us(value):
    """Microseconds, fixed width, so two tables can be diffed by eye."""
    return "%10.3f" % value


def stats_row(name, desc):
    """One line of the published table (n, p50, p90, p99, p99.9, max, stdev)."""
    return ("  %-8s n=%-8d p50=%s p90=%s p99=%s p99.9=%s max=%s  sd=%s us"
            % (name, desc["n"], fmt_us(desc["p50"]), fmt_us(desc["p90"]),
               fmt_us(desc["p99"]), fmt_us(desc["p99_9"]), fmt_us(desc["max"]),
               fmt_us(desc["stdev"])))


def dispersion(values):
    """Spread of one statistic across runs: absolute and relative to the mean."""
    values = [v for v in values if v is not None]
    if len(values) < 2:
        return {"runs": len(values), "min": None, "max": None,
                "spread": None, "spread_pct": None}
    lo, hi = min(values), max(values)
    mean = sum(values) / float(len(values))
    return {"runs": len(values), "min": lo, "max": hi, "spread": hi - lo,
            "spread_pct": (100.0 * (hi - lo) / mean) if mean > 0 else None}


def merge_descriptors(descs):
    """Aggregate the per-run descriptors of one segment across runs.

    The published number of a campaign is the WORST run for the tail (p99.9,
    max) and the median run for the body (p50): a WCET argument may not average
    a worst case away. n is the total number of samples.
    """
    if not descs:
        return None
    tail_keys = ("p99", "p99_9", "max", "stdev")
    body_keys = ("p50", "p90", "mean", "min")
    out = {"n": sum(int(d["n"]) for d in descs)}
    for key in tail_keys:
        out[key] = max(d[key] for d in descs)
    for key in body_keys:
        values = sorted(d[key] for d in descs)
        out[key] = values[len(values) // 2] if len(values) % 2 else \
            0.5 * (values[len(values) // 2 - 1] + values[len(values) // 2])
    out["runs"] = len(descs)
    return out


def cross_check(probe_desc, recomputed, tolerance_pct=1.0):
    """Probe summary (C) against the recomputation (host/timing/wcet.py).

    Returns (agreement, worst_relative_gap_pct). The two must agree: they read
    the same samples, one from C, one from Python. A gap means a truncated file
    or a different percentile definition, and is reported instead of hidden.
    """
    worst = 0.0
    for key in ("n", "p50", "p90", "p99", "p99_9", "max"):
        a = float(probe_desc.get(key, 0.0))
        b = float(recomputed.get(key, 0.0))
        if key == "n":
            if int(a) != int(b):
                return False, 100.0
            continue
        scale = max(abs(a), abs(b), 1e-9)
        worst = max(worst, 100.0 * abs(a - b) / scale)
    return worst <= tolerance_pct, worst


def eprint(*args):
    print(*args, file=sys.stderr)
