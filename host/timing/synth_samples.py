#!/usr/bin/env python3
"""synth_samples.py — synthetic latency sample generator for wcet.py.

M02 may not have delivered real samples yet when wcet.py and the tests are
written. This generator produces a JSONL file in the EXACT format wcet.py
expects (a "latency_us" key per line, plus optional "tick"/"source" context),
so the two are decoupled: wcet.py and test_timing.py run against synthetic
data, and M02 only has to emit the same keys.

The distribution is deliberately latency-like: a log-normal body (median a few
hundred microseconds) plus an exponential strong tail, so the Gumbel-POT
extrapolation has genuine exceedances to fit. Deterministic (fixed seed).

Usage:
  python3 host/timing/synth_samples.py 50000 > /tmp/syn.jsonl
  python3 host/timing/wcet.py /tmp/syn.jsonl
"""
import json
import math
import random
import sys


def generate(n, seed=0):
    rng = random.Random(seed)
    out = []
    for tick in range(n):
        # Log-normal body: median ~300 us, heavy but not extreme.
        body = rng.lognormvariate(math.log(300.0), 0.55)
        # Strong tail: a few percent draw an exponential of mean 1500 us.
        if rng.random() < 0.03:
            body += rng.expovariate(1.0 / 1500.0)
        latency_us = max(1.0, body)
        out.append({"latency_us": round(latency_us, 3), "tick": tick,
                    "source": "net_rx"})
    return out


def main(argv):
    n = int(argv[0]) if argv else 50000
    for obj in generate(n):
        print(json.dumps(obj))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))