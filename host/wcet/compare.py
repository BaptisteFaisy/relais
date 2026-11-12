#!/usr/bin/env python3
"""compare.py — dispersion between two successive executions of the harness.

The mission asks for the reproducibility of the measurement, not only for the
measurement: two campaigns run one after the other on the same machine, with the
same command, must be compared statistic by statistic. This tool prints that
comparison and classifies it:

  * the BODY of the distribution (p50, p90, p99) is the cost of the code;
  * the TAIL (p99.9, max, the extrapolated bound) is the cost of the operating
    system around it. On a general-purpose kernel shared with other work the
    tail is expected to move; that is a property of the host, not of the
    protocol, and it is the reason the same harness must be re-run on the
    QNX target (docs/WCET-TARGETS.md, host/timing/procedures-qnx.md).

Usage:
  python3 host/wcet/compare.py runs/wcet/A/measure.json runs/wcet/B/measure.json
  python3 host/wcet/compare.py A.json B.json --segments path,decode --stats p50,max
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wcet_lib as W  # noqa: E402

BODY = ("p50", "p90", "p99")
TAIL = ("p99_9", "max")


def relative_gap(first, second):
    """Signed gap of second against first, in percent of the two-value mean."""
    if first is None or second is None:
        return None
    scale = (abs(first) + abs(second)) / 2.0
    if scale <= 0.0:
        return 0.0
    return 100.0 * (second - first) / scale


def segment_table(segments_a, segments_b, segment, stats):
    lines = []
    first = (segments_a or {}).get(segment)
    second = (segments_b or {}).get(segment)
    if not first or not second:
        return lines
    for stat in stats:
        gap = relative_gap(first.get(stat), second.get(stat))
        lines.append("    %-6s %12.3f %12.3f %12s %s" % (
            stat, first[stat], second[stat],
            "n/a" if gap is None else "%+.3f" % gap,
            "n/a" if gap is None else "%+7.1f%%" % gap))
    return lines


def worst_gaps(payload_a, payload_b, segments, stats):
    """Largest absolute relative gap per statistic, over modes and segments."""
    worst = {stat: 0.0 for stat in stats}
    modes = set(payload_a.get("modes", {})) & set(payload_b.get("modes", {}))
    for mode in modes:
        seg_a = payload_a["modes"][mode].get("aggregate", {}).get("segments", {})
        seg_b = payload_b["modes"][mode].get("aggregate", {}).get("segments", {})
        for segment in segments:
            first, second = seg_a.get(segment), seg_b.get(segment)
            if not first or not second:
                continue
            for stat in stats:
                gap = relative_gap(first.get(stat), second.get(stat))
                if gap is not None:
                    worst[stat] = max(worst[stat], abs(gap))
    return worst


def render(payload_a, payload_b, path_a, path_b, segments, stats):
    modes = sorted(set(payload_a.get("modes", {})) & set(payload_b.get("modes", {})))
    lines = ["Relais M40 - dispersion between two successive executions",
             "  A: %s  (host=%s, date=%s)" % (path_a, payload_a.get("host"),
                                              payload_a.get("date")),
             "  B: %s  (host=%s, date=%s)" % (path_b, payload_b.get("host"),
                                              payload_b.get("date")),
             "  config A=%s  B=%s" % (payload_a.get("config", {}).get("samples"),
                                      payload_b.get("config", {}).get("samples")),
             ""]
    for mode in modes:
        seg_a = payload_a["modes"][mode].get("aggregate", {}).get("segments", {})
        seg_b = payload_b["modes"][mode].get("aggregate", {}).get("segments", {})
        lines.append("  mode %s" % mode)
        for segment in segments:
            lines.append("    segment %s" % segment)
            lines.append("    %-6s %12s %12s %12s %9s"
                         % ("stat", "A (us)", "B (us)", "B-A (us)", "gap"))
            lines += segment_table(seg_a, seg_b, segment, stats)
        lines.append("")
    worst = worst_gaps(payload_a, payload_b, segments, stats)
    lines.append("  largest |gap| per statistic, over every mode and segment:")
    for stat in stats:
        family = "body" if stat in BODY else ("tail" if stat in TAIL else "other")
        lines.append("    %-6s %8.1f%%   (%s)" % (stat, worst[stat], family))
    body = max([worst[s] for s in BODY if s in worst] or [0.0])
    tail = max([worst[s] for s in TAIL if s in worst] or [0.0])
    lines.append("")
    lines.append("  body (p50/p90/p99) reproducible within %.1f%%; tail "
                 "(p99.9/max) within %.1f%%" % (body, tail))
    lines.append("  reading: the body is the cost of the code and repeats; the "
                 "tail is the cost of the host kernel around it and moves on a "
                 "shared general-purpose machine. Only a measurement on the "
                 "target (fixed priority, isolated CPU) can bound the tail.")
    return "\n".join(lines)


def main(argv):
    ap = argparse.ArgumentParser(
        description="Dispersion between two executions of host/wcet/measure.py")
    ap.add_argument("first", help="measure.json of the first execution")
    ap.add_argument("second", help="measure.json of the second execution")
    ap.add_argument("--segments", default="path",
                    help="comma list among %s" % ",".join(W.SEGMENTS))
    ap.add_argument("--stats", default="p50,p90,p99,p99_9,max",
                    help="comma list of statistics to compare")
    ap.add_argument("--out", default=None, help="also write the report here")
    args = ap.parse_args(argv)

    for path in (args.first, args.second):
        if not os.path.exists(path):
            print("BLOCKED(no measurement file %s)" % path)
            return 2
    payload_a = W.read_json(args.first)
    payload_b = W.read_json(args.second)
    segments = [s for s in args.segments.split(",") if s in W.SEGMENTS]
    stats = [s.strip() for s in args.stats.split(",") if s.strip()]
    text = render(payload_a, payload_b, args.first, args.second, segments, stats)
    if args.out:
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
