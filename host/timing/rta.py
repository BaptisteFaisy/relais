#!/usr/bin/env python3
"""rta.py — fixed-priority response-time analysis of the critical chain.

The critical path is one chain of three QNX processes, released once per tick
and carrying the mandate end to end:

    relais-net (prio 62) -> relais-core (prio 61) -> relais-ctl (prio 60)
    receive + validate       protocol state machine      control law
    + emit

This tool answers the schedulability question at fixed priority (QNX-03):
given a period T and a worst-case execution time per stage, compute the
response time R_i of each stage (worst-case completion measured from the chain
release, including pre-emption by strictly higher-priority stages), iterate to
a fixed point, then compare the end-to-end chain response to T.

Model and assumptions (see docs/TIMING.md):
  * every stage is released once per tick, so all share the same period T;
  * R_i = C_i + sum_{j in hp(i)} ceil(R_i / T) * C_j  (Audsley's iteration);
  * the chain response is the sum of the stage responses, a conservative upper
    bound that IGNORES priority inheritance (QNX message passing raises the
    server to the client's priority); with inheritance the bound tightens;
  * verdict "tenable" requires BOTH that each R_i <= T and that the chain
    response <= T.

Configuration (JSON, standard library only):

    {
      "period_us": 5000,
      "stages": [
        {"name": "relais-net",  "priority": 62, "wcet_us": 150},
        {"name": "relais-core", "priority": 61, "wcet_us": 250},
        {"name": "relais-ctl",  "priority": 60, "wcet_us": 100}
      ]
    }

Run with no --config to use the built-in example above.
"""
import argparse
import json
import math
import sys

MAX_ITERS = 1000
RELAX = 0.0  # exact comparison; no epsilon needed at integer microseconds


class Stage:
    __slots__ = ("name", "priority", "wcet_us")

    def __init__(self, name, priority, wcet_us):
        self.name = name
        self.priority = priority
        self.wcet_us = wcet_us

    def __repr__(self):
        return "Stage(%r, pr=%d, C=%dus)" % (self.name, self.priority, self.wcet_us)


def response_time(stage, higher_wcets, period):
    """Iterate R = C + sum ceil(R/T)*C_h to a fixed point.

    Returns the response time in microseconds, or None if the iteration
    diverges (the stage is unschedulable).
    """
    r = float(stage.wcet_us)
    for _ in range(MAX_ITERS):
        interference = 0.0
        for c_h in higher_wcets:
            interference += math.ceil(r / period) * c_h
        r_next = float(stage.wcet_us) + interference
        if r_next == r:
            return r
        if r_next > 1e6 * period:  # no bound within any sane horizon
            return None
        r = r_next
    return None  # did not converge


def analyze(stages, period):
    """Compute per-stage response times and the chain verdict."""
    if not stages or period <= 0:
        raise ValueError("stages must be non-empty and period positive")
    ordered = sorted(stages, key=lambda s: -s.priority)
    results = []
    for idx, stage in enumerate(ordered):
        higher_wcets = [s.wcet_us for s in ordered[:idx]]
        r = response_time(stage, higher_wcets, period)
        results.append((stage, r))

    chain = None
    if all(r is not None for _, r in results):
        chain = sum(r for _, r in results)

    reasons = []
    schedulable = chain is not None
    for stage, r in results:
        if r is None:
            schedulable = False
            reasons.append("%s: response time diverges (unschedulable)" % stage.name)
        elif r > period:
            schedulable = False
            reasons.append("%s: R=%dus > T=%dus" % (stage.name, r, period))
    if chain is not None and chain > period:
        schedulable = False
        reasons.append("chain response %dus > T=%dus" % (chain, period))

    return {
        "period_us": period,
        "results": results,   # list of (Stage, response_us or None)
        "chain_us": chain,
        "schedulable": schedulable,
        "reasons": reasons,
    }


def render(analysis):
    period = analysis["period_us"]
    lines = ["fixed-priority RTA, T=%d us" % period]
    for stage, r in analysis["results"]:
        if r is None:
            lines.append("  %-12s prio %2d  C=%4d us  R=diverges" %
                         (stage.name, stage.priority, stage.wcet_us))
        else:
            lines.append("  %-12s prio %2d  C=%4d us  R=%6d us  margin=%6d us" %
                         (stage.name, stage.priority, stage.wcet_us, int(r),
                          int(period - r)))
    if analysis["chain_us"] is not None:
        lines.append("  chain response = %d us  margin = %d us" %
                     (analysis["chain_us"], period - analysis["chain_us"]))
    if analysis["schedulable"]:
        lines.append("verdict: TENABLE  (every R_i <= T and chain <= T)")
    else:
        lines.append("verdict: NOT TENABLE")
        for reason in analysis["reasons"]:
            lines.append("  - " + reason)
    return "\n".join(lines)


def load_config(path):
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    period = int(data["period_us"])
    stages = []
    for entry in data["stages"]:
        stages.append(Stage(entry["name"], int(entry["priority"]),
                            int(entry["wcet_us"])))
    return stages, period


def builtin_example():
    stages = [
        Stage("relais-net", 62, 150),
        Stage("relais-core", 61, 250),
        Stage("relais-ctl", 60, 100),
    ]
    return stages, 5000


def main(argv):
    ap = argparse.ArgumentParser(description="Fixed-priority response-time analysis")
    ap.add_argument("--config", help="JSON config with period_us and stages[]")
    args = ap.parse_args(argv)

    if args.config:
        stages, period = load_config(args.config)
    else:
        stages, period = builtin_example()

    analysis = analyze(stages, period)
    print(render(analysis))
    return 0 if analysis["schedulable"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))