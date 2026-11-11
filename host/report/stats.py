"""Pure statistics for the Relais campaign report (OBS-05).

Standard library only, no numpy. Every function is pure: it takes numbers and
returns numbers, and it never raises on degenerate input (empty list, single
value, identical values) so that the report generator can stay linear.
"""
from __future__ import annotations

import math
from typing import Dict, List, Sequence, Tuple

__all__ = [
    "median", "percentile", "mean", "stdev", "summary", "histogram",
    "histogram_from_values",
]


def median(values: Sequence[float]) -> float:
    """Return the median; 0.0 for an empty input."""
    return percentile(values, 50.0)


def percentile(values: Sequence[float], pct: float) -> float:
    """Return the pct-th percentile (0..100), linear interpolation between
    closest ranks. 0.0 for an empty input. pct is clamped to [0, 100]."""
    if not values:
        return 0.0
    data = sorted(values)
    n = len(data)
    if n == 1:
        return float(data[0])
    pct = min(100.0, max(0.0, float(pct)))
    rank = pct / 100.0 * (n - 1)
    low = int(math.floor(rank))
    high = min(low + 1, n - 1)
    frac = rank - low
    return float(data[low]) * (1.0 - frac) + float(data[high]) * frac


def mean(values: Sequence[float]) -> float:
    """Return the arithmetic mean; 0.0 for an empty input."""
    if not values:
        return 0.0
    return float(sum(values)) / len(values)


def stdev(values: Sequence[float]) -> float:
    """Return the sample standard deviation; 0.0 when fewer than two values."""
    n = len(values)
    if n < 2:
        return 0.0
    mu = mean(values)
    var = sum((float(v) - mu) ** 2 for v in values) / (n - 1)
    return math.sqrt(var)


def summary(values: Sequence[float]) -> Dict[str, float]:
    """Return the published measurement set (spec 7.7): median, p90, p99,
    p99.9, min, max, mean, stdev, count."""
    data = list(values)
    return {
        "count": float(len(data)),
        "min": float(min(data)) if data else 0.0,
        "max": float(max(data)) if data else 0.0,
        "mean": mean(data),
        "stdev": stdev(data),
        "p50": percentile(data, 50.0),
        "p90": percentile(data, 90.0),
        "p99": percentile(data, 99.0),
        "p99.9": percentile(data, 99.9),
    }


def histogram(bins: Sequence[Tuple[float, float, int]]) -> List[Dict[str, float]]:
    """Normalise pre-binned data [(lo, hi, count), ...] into plot records."""
    out: List[Dict[str, float]] = []
    for lo, hi, count in bins:
        out.append({"lo": float(lo), "hi": float(hi), "count": float(count)})
    return out


def histogram_from_values(values: Sequence[float], step: float,
                          origin: float = 0.0) -> List[Dict[str, float]]:
    """Bin values into fixed-width buckets of size `step` starting at origin.

    Returns records {"lo", "hi", "count"}; empty input gives an empty list.
    A non-positive step falls back to 1.0 so the function never divides by 0.
    """
    if not values:
        return []
    if step <= 0.0:
        step = 1.0
    lo_v = min(float(v) for v in values)
    hi_v = max(float(v) for v in values)
    first = origin + math.floor((lo_v - origin) / step) * step
    last = origin + math.floor((hi_v - origin) / step) * step
    counts: Dict[int, int] = {}
    for v in values:
        idx = int(math.floor((float(v) - origin) / step))
        counts[idx] = counts.get(idx, 0) + 1
    out: List[Dict[str, float]] = []
    lo = first
    while lo <= last + step / 2.0:
        idx = int(round((lo - origin) / step))
        out.append({"lo": lo, "hi": lo + step, "count": float(counts.get(idx, 0))})
        lo += step
    return out
