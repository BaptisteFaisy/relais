#!/usr/bin/env python3
"""Relais -- what the soak knows about the machine it runs on (M42).

The campaign must never overload a shared machine: the pilot reads the load
average, the available memory and the free disk, and shortens or stops itself
when the box cannot carry the run. The rules are constants, published here so
that the pilot, the docs and the report agree on them.
"""
from __future__ import annotations

import os
from typing import Tuple

LOAD_MAX = 20.0        # load average above which the campaign is shortened
MEM_MIN_GB = 4.0       # available memory below which the campaign is shortened
LOAD_FACTOR = 0.5      # shortening factor when the machine is busy
FLOOR_MINUTES = 20.0   # a shortened campaign stays inside the 20..40 min band
MIN_MINUTES = 5.0      # shortening floor for a short request
DISK_MIN_GB = 2.0      # stop before the disk fills up


def mem_available_gb() -> float:
    """Return the available memory of the machine, in GiB."""
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / (1024.0 * 1024.0)
    except (OSError, ValueError, IndexError):
        pass
    return 99.0


def effective_minutes(requested: float, load: float, mem_gb: float) -> Tuple[float, str]:
    """Shorten the campaign when the machine is too busy to carry it."""
    if load <= LOAD_MAX and mem_gb >= MEM_MIN_GB:
        return requested, ("machine free enough (load %.1f, %.1f GB available)"
                           % (load, mem_gb))
    if requested < FLOOR_MINUTES:
        return requested, ("machine busy (load %.1f > %.0f or %.1f GB available < %.0f GB), "
                           "short campaign kept as is" % (load, LOAD_MAX, mem_gb, MEM_MIN_GB))
    floor = FLOOR_MINUTES if requested >= FLOOR_MINUTES else MIN_MINUTES
    shortened = min(requested, max(floor, requested * LOAD_FACTOR))
    return shortened, ("machine busy (load %.1f > %.0f or %.1f GB available < %.0f GB), "
                       "campaign shortened from %.0f to %.0f min"
                       % (load, LOAD_MAX, mem_gb, MEM_MIN_GB, requested, shortened))


def disk_free_gb(path: str) -> float:
    """Return the free space of the filesystem holding path, in GiB."""
    try:
        st = os.statvfs(path)
        return st.f_bavail * st.f_frsize / (1024.0 ** 3)
    except OSError:
        return 99.0


def self_rss_kb() -> int:
    """Resident memory of the calling process, in kB."""
    try:
        with open("/proc/self/statm", "r", encoding="utf-8") as fh:
            pages = int(fh.read().split()[1])
    except (OSError, ValueError, IndexError):
        return 0
    return pages * (os.sysconf("SC_PAGE_SIZE") // 1024)
