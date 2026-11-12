#!/usr/bin/env python3
"""Relais — verify the golden traces (mission M35).

Replays the frozen game of tools/golden/scenarios.json with the seeded
simulator and compares every trace with the reference file
tools/golden/traces.json. The first divergence stops the run with a non-zero
exit code and names the scenario and the first divergent tick.

  python3 tools/golden/verify.py            # the guard: replay and compare
  python3 tools/golden/verify.py --update   # DELIBERATE refresh of the reference

A divergence means the DECISIONS of the core changed: the mandate, the
applied value, the source of the value, the lamp or the events. A green
verify means the sequence of decisions is exactly the one of the reference.

The refresh (--update) is a deliberate act: it accepts the present behaviour
as the new reference. It must be reviewed, committed with the change that
caused it, and justified in the pull request (docs/GOLDEN.md).
Standard library only.
"""

import argparse
import os
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the repository
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import golden_lib


def first_divergent_line(stored_digests, current_digests):
    """Index of the first journal line whose digest differs (None if equal)."""
    stored = [stored_digests[i:i + 8] for i in range(0, len(stored_digests), 8)]
    current = [current_digests[i:i + 8] for i in range(0, len(current_digests), 8)]
    for index in range(min(len(stored), len(current))):
        if stored[index] != current[index]:
            return index
    if len(stored) != len(current):
        return min(len(stored), len(current))
    return None


def tick_at(journal, index):
    """The tick carried by the journal line at that index."""
    ticks = golden_lib.journal_ticks(journal)
    if index < len(ticks):
        return ticks[index]
    return ticks[-1] if ticks else -1


def compare(reference, record):
    """Return the list of divergences between the reference and the replay."""
    current = record["trace"]
    name = reference["name"]
    diffs = []
    if reference["exit"] != current["exit"]:
        diffs.append("exit code: reference %s, replay %s"
                     % (reference["exit"], current["exit"]))
    if reference["journal_sha256"] != current["journal_sha256"]:
        diffs.append("journal sha256:\n    reference %s\n    replay    %s"
                     % (reference["journal_sha256"], current["journal_sha256"]))
    if reference["report_sha256"] != current["report_sha256"]:
        diffs.append("invariant report sha256:\n    reference %s\n    replay    %s"
                     % (reference["report_sha256"], current["report_sha256"]))
    if reference["journal_lines"] != current["journal_lines"]:
        diffs.append("journal lines: reference %d, replay %d"
                     % (reference["journal_lines"], current["journal_lines"]))
    return diffs


def report_divergence(reference, record, diffs):
    """Print the scenario, the divergences and the first divergent tick."""
    print("GOLDEN DIVERGENCE on scenario %s" % reference["name"])
    print("  source: %s" % reference.get("source", "golden"))
    print("  faults: %s" % ", ".join(reference.get("faults", [])) or "none")
    for diff in diffs:
        print("  %s" % diff)
    index = first_divergent_line(reference.get("line_digests", ""),
                                 record["trace"]["line_digests"])
    if index is not None:
        print("  first divergent journal line: %d (tick %d)"
              % (index, tick_at(record["journal"], index)))
    else:
        print("  the normalised journals are identical: the difference is "
              "in the invariant report")


def check_game(reference_traces, game):
    """The frozen game and the reference must describe the same scenarios."""
    reference_names = [trace["name"] for trace in reference_traces["scenarios"]]
    game_names = [entry["name"] for entry in game]
    if reference_names == game_names:
        return None
    missing = [name for name in game_names if name not in reference_names]
    extra = [name for name in reference_names if name not in game_names]
    parts = []
    if missing:
        parts.append("not in the reference: %s" % ", ".join(missing))
    if extra:
        parts.append("not in the game: %s" % ", ".join(extra))
    return "the frozen game and the reference disagree (%s)" % "; ".join(parts)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Verify the golden traces")
    parser.add_argument("--update", action="store_true",
                        help="DELIBERATE: rewrite the reference with the "
                             "present behaviour")
    parser.add_argument("--manifest", default=golden_lib.MANIFEST_PATH)
    parser.add_argument("--traces", default=golden_lib.TRACES_PATH)
    args = parser.parse_args(argv)

    if not os.path.exists(golden_lib.SIMULATOR):
        print("missing simulator: %s (run `make` first)" % golden_lib.SIMULATOR)
        return 2
    manifest = golden_lib.load_manifest(args.manifest)
    game = golden_lib.resolve_game(manifest)
    reference_traces = golden_lib.read_traces(args.traces)
    mismatch = check_game(reference_traces, game)
    if mismatch:
        print("GOLDEN DIVERGENCE: %s" % mismatch)
        print("refresh the reference on purpose (make golden-update) after "
              "reviewing the change")
        return 1

    records = golden_lib.capture_game(game)
    references = {trace["name"]: trace for trace in reference_traces["scenarios"]}
    if args.update:
        golden_lib.write_traces([record["trace"] for record in records], args.traces)
        print("golden traces DELIBERATELY refreshed: %d scenario(s) -> %s"
              % (len(records), args.traces))
        return 0

    for record in records:
        reference = references[record["entry"]["name"]]
        diffs = compare(reference, record)
        if diffs:
            report_divergence(reference, record, diffs)
            return 1
    print("golden traces: %d scenario(s) match the reference (%s)"
          % (len(records), args.traces))
    return 0


if __name__ == "__main__":
    sys.exit(main())
