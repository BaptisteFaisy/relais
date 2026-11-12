#!/usr/bin/env python3
"""Relais — capture the golden traces (mission M35).

Plays the frozen game of tools/golden/scenarios.json with the seeded
simulator of the repository, normalises every journal and writes the SHA-256
of each normalised journal into tools/golden/traces.json.

  python3 tools/golden/capture.py             # refresh the reference file
  python3 tools/golden/capture.py --list      # show the frozen game

Refreshing the reference is a DELIBERATE act: it accepts the present
behaviour as the new reference. Read tools/golden/../docs/GOLDEN.md before
running it on a core that changed on purpose. Standard library only.
"""

import argparse
import os
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the repository
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import golden_lib


def describe(entry):
    """One line describing a scenario of the frozen game."""
    faults = ", ".join(entry["specs"]) if entry["specs"] else "none"
    return "%-36s %-8s %6d ticks  seed %-6d  faults: %s" % (
        entry["name"], entry["level"], entry["ticks"], entry["seed"], faults)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Capture the golden traces")
    parser.add_argument("--list", action="store_true",
                        help="list the frozen game and exit")
    parser.add_argument("--manifest", default=golden_lib.MANIFEST_PATH)
    parser.add_argument("--out", default=golden_lib.TRACES_PATH)
    args = parser.parse_args(argv)

    manifest = golden_lib.load_manifest(args.manifest)
    game = golden_lib.resolve_game(manifest)
    if args.list:
        for entry in game:
            print(describe(entry))
        print("%d scenario(s) in the frozen game" % len(game))
        return 0
    if not os.path.exists(golden_lib.SIMULATOR):
        print("missing simulator: %s (run `make` first)" % golden_lib.SIMULATOR)
        return 2

    records = golden_lib.capture_game(game)
    traces = [record["trace"] for record in records]
    golden_lib.write_traces(traces, args.out)

    lines = sum(trace["journal_lines"] for trace in traces)
    ticks = sum(trace["journal_ticks"] for trace in traces)
    print("golden traces: %d scenario(s), %d journal line(s), %d tick(s) -> %s"
          % (len(traces), lines, ticks, args.out))
    broken = [record for record in records if record["trace"]["exit"] != 0]
    for record in broken:
        print("  INVARIANT VIOLATION in %s (exit %d):"
              % (record["entry"]["name"], record["trace"]["exit"]))
        for line in record["report"].splitlines():
            print("    " + line)
    if broken:
        print("the trace records a core that violates its own invariants")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
