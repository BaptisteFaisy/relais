#!/usr/bin/env python3
"""Relais — print N free UDP ports, one per line (mission M41, bench helper).

The kernel picks the ports, so no two concurrent benches collide. The race
window (a port taken between the probe and the bind) is tiny and the caller
retries when the bind fails.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hil_env import free_port  # noqa: E402

if __name__ == "__main__":
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    print("\n".join(str(free_port()) for _ in range(count)))
