"""Relais Event Data Recorder (spec 12 #9, OBS-05 / OBS-07).

Turns an actuator journal (see CONTRACT.md "Journal format") into one durable
EDR record per mandate change ("handover"), fit to be read after an accident
without the bench: timestamp, tick, old/new holder, mandate numbers, the cause,
the fault-detection interval (FDTI) and the fault-reaction interval (FRTI), the
source of the applied value during the switch, the lamp and cause evidence, the
gap to the fault-free twin, and the surrounding journal extract (+/- 20 ticks).

Definitions follow docs/SPEC.md (ISO-26262 terms, "Fault detection interval"
and "Fault reaction interval") and docs/SAFETY.md:

  * detection tick = the tick of the handover evidence event (mandate_revoked /
    mandate_auto_switch / cession / mandate_granted), i.e. the "constat";
  * FDTI = detection tick - last tick the old holder delivered a fresh order
    (p+1 ticks for a death when p = 1);
  * FRTI = first tick the new holder delivered a fresh order - detection tick;
  * source_run = source of the value applied on every tick strictly after the
    old holder's last fresh order, up to and including the new holder's first
    fresh order (the substitution period).

It reuses host/report/journal_io.py (single journal reader, zero duplication).

Usage:
    python3 host/edr/edr.py --journal /tmp/x.jsonl --twin /tmp/x.twin.jsonl \
        --out /tmp/x.edrout [--period-ms 5.0]
    python3 host/edr/edr.py --dump /tmp/x.edrout.edr
"""
from __future__ import annotations

import argparse
import os
import sys
from typing import Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
for _inc in (os.path.join(_HERE, "..", "report"), _HERE):
    if _inc not in sys.path:
        sys.path.insert(0, _inc)

from journal_io import Journal, read_journal  # noqa: E402
from edr_format import (  # noqa: E402
    EdrRecord, read_binary, write_binary, write_csv, write_json,
    format_record_text,
)

EXTRACT_RADIUS = 20  # surround the detection tick with +/- 20 ticks (spec 12 #9)
HANDOVER_EVENTS = ("mandate_revoked", "mandate_auto_switch", "mandate_granted")


def _runs(ticks):
    """Split the tick stream into runs of constant (holder, mandate)."""
    runs = []
    if not ticks:
        return runs
    start = 0
    for i in range(1, len(ticks)):
        if ticks[i].holder != ticks[i - 1].holder or ticks[i].mandate != ticks[i - 1].mandate:
            runs.append((start, i - 1, ticks[i - 1].holder, ticks[i - 1].mandate))
            start = i
    runs.append((start, len(ticks) - 1, ticks[-1].holder, ticks[-1].mandate))
    return runs


def handover_detections(ticks) -> List[Tuple[int, int]]:
    """Return (old_end_index, new_start_index) for each handover.

    A handover moves the mandate from one non-vacant holder to another. A short
    vacancy in between (the cession path: revoke, then DEMANDE grant, DET-05 /
    REL-06) is bridged into a single handover. The initial grant (vacant -> A)
    is not a handover and is skipped.
    """
    runs = _runs(ticks)
    dets: List[Tuple[int, int]] = []
    i = 1
    while i < len(runs):
        _s0, e0, h0, _m0 = runs[i - 1]
        s1, e1, h1, _m1 = runs[i]
        if h0 and h1 and h0 != h1:
            dets.append((e0, s1))
            i += 1
            continue
        if h0 and not h1 and i + 1 < len(runs):
            s2, _e2, h2, _m2 = runs[i + 1]
            if h2 and (e1 - s1 + 1) <= 2:
                dets.append((e0, s2))
                i += 2
                continue
        i += 1
    return dets


def _detection_tick(events, prev_tick: int, det_tick: int) -> int:
    """Tick of the handover evidence event (the 'constat'), else the new-holder tick."""
    names = HANDOVER_EVENTS + ("cession",)
    best = det_tick
    for e in events:
        if e.event not in names:
            continue
        if prev_tick <= e.tick <= det_tick:
            best = e.tick
            break  # events are journal-ordered by tick
    return best


def _event_near(events, det_tick: int, name: str, node: str = None) -> bool:
    for e in events:
        if e.event != name:
            continue
        if abs(e.tick - det_tick) <= 2 and (node is None or e.node == node):
            return True
    return False


def classify_cause(ticks, events, det_index: int, holder_old: str, detection: int) -> str:
    """Evidence-based cause of a handover, from the actuator's own journal.

    The journal records the actuator's evidence (DET-03), not the injected fault
    type, so the cause is inferred: an explicit CESSION event wins, then a
    disagreement or fallback, then whether the old holder ever comes back
    (freeze) or is gone for good (death), then the lamp cause (cut for a lost
    link). The limit is acknowledged: a death and a permanent link cut are
    indistinguishable from the actuator alone. A brand-new grant from the void
    is 'missing'.
    """
    if not holder_old:
        return "missing"
    if _event_near(events, detection, "cession", holder_old):
        return "cession"
    if _event_near(events, detection, "disagreement"):
        return "disagreement"
    if _event_near(events, detection, "fallback_enter"):
        return "fallback"
    cause = ticks[det_index].cause if det_index < len(ticks) else "none"
    if _holder_returns(ticks, det_index, holder_old):
        return "cut" if cause == "link_lost" else "freeze"
    if cause == "link_lost":
        return "cut"
    return "death"


def _holder_returns(ticks, det_index: int, holder_old: str) -> bool:
    return any(t.holder == holder_old for t in ticks[det_index + 1:])


def last_fresh_of(ticks, holder: str, det_index: int) -> Optional[int]:
    """Last tick before the switch where `holder` delivered a fresh order."""
    if not holder:
        return None
    for t in reversed(ticks[:det_index]):
        if t.holder == holder and t.source == "holder":
            return t.tick
    return None


def first_fresh_from(ticks, holder: str, det_index: int) -> Optional[int]:
    """First tick at/after the switch where the new holder delivered a fresh order."""
    if not holder:
        return None
    for t in ticks[det_index:]:
        if t.holder == holder and t.source == "holder":
            return t.tick
    return None


def _source_run(ticks, last_fresh: int, first_fresh: int) -> List[str]:
    """Source of each tick strictly after the old holder's last fresh order,
    through the new holder's first fresh order."""
    out: List[str] = []
    for t in ticks:
        if last_fresh < t.tick <= first_fresh:
            out.append(t.source)
    return out


def _worst_lamp(ticks, detection: int, first_fresh: int) -> str:
    order = {"off": 0, "yellow": 1, "red": 2}
    worst = "off"
    for t in ticks:
        if detection <= t.tick <= first_fresh and order.get(t.lamp, 0) > order.get(worst, 0):
            worst = t.lamp
    return worst


def _twin_gap(ticks, twin_tick: Dict[int, object], detection: int, first_fresh: int):
    max_dv, max_dp = 0.0, 0.0
    for t in ticks:
        if t.tick < detection or t.tick > first_fresh:
            continue
        tw = twin_tick.get(t.tick)
        if tw is None:
            continue
        max_dv = max(max_dv, abs(t.value - tw.value))
        max_dp = max(max_dp, abs(t.pos - tw.pos))
    return max_dv, max_dp


def _detection_detail(events, detection: int) -> int:
    detail = 0
    for e in events:
        if e.event in HANDOVER_EVENTS and abs(e.tick - detection) <= 1:
            detail = e.detail
    return detail


def build_records(act: Journal, twin: Optional[Journal], period_ms: float,
                  raw_path: Optional[str]) -> List[EdrRecord]:
    """Build one EdrRecord per handover detected in the actuator journal."""
    ticks = act.ticks
    events = act.events
    twin_tick: Dict[int, object] = {}
    if twin is not None:
        twin_tick = {t.tick: t for t in twin.ticks}
    raw_by_tick: Dict[int, List[str]] = {}
    if raw_path:
        raw_by_tick = _raw_extract_around(raw_path)

    records: List[EdrRecord] = []
    for prev_index, det_index in handover_detections(ticks):
        holder_old = ticks[prev_index].holder
        holder_new = ticks[det_index].holder
        detection = _detection_tick(events, ticks[prev_index].tick, ticks[det_index].tick)
        last_fresh = last_fresh_of(ticks, holder_old, det_index)
        first_fresh = first_fresh_from(ticks, holder_new, det_index)
        fdti = detection - last_fresh if last_fresh is not None else 0
        frti = first_fresh - detection if first_fresh is not None else 0
        last_f = last_fresh if last_fresh is not None else detection
        first_f = first_fresh if first_fresh is not None else detection
        rec = EdrRecord(
            tick=detection, ns=ticks[det_index].ns, t_ms=detection * period_ms,
            mandate_old=ticks[prev_index].mandate, mandate_new=ticks[det_index].mandate,
            holder_old=holder_old, holder_new=holder_new,
            cause=classify_cause(ticks, events, det_index, holder_old, detection),
            lamp=_worst_lamp(ticks, detection, first_f),
            certified_new=ticks[det_index].certified,
            fdti_ticks=fdti, frti_ticks=frti,
            source_run=_source_run(ticks, last_f, first_f),
            detail=_detection_detail(events, detection), extract=[],
        )
        rec.max_dev, rec.max_pos_dev = _twin_gap(ticks, twin_tick, detection, first_f)
        rec.extract = _extract(raw_by_tick, detection)
        records.append(rec)
    return records


def _raw_extract_around(path: str) -> Dict[int, List[str]]:
    """Map a journal line to the tick number it documents."""
    import json
    out: Dict[int, List[str]] = {}
    try:
        f = open(path, "r", encoding="utf-8", errors="replace")
    except OSError:
        return out
    with f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            try:
                t = int(json.loads(s).get("tick", -1))
            except (ValueError, TypeError):
                continue
            if t >= 0:
                out.setdefault(t, []).append(s)
    return out


def _extract(raw_by_tick: Dict[int, List[str]], det_tick: int) -> List[str]:
    out: List[str] = []
    for t in range(det_tick - EXTRACT_RADIUS, det_tick + EXTRACT_RADIUS + 1):
        for line in raw_by_tick.get(t, []):
            out.append(line)
    return out


def _load_optional(path: Optional[str]) -> Optional[Journal]:
    if path and os.path.isfile(path):
        return read_journal(path)
    return None


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Relais Event Data Recorder (spec 12 #9)")
    ap.add_argument("--journal", help="actuator JSON-lines journal")
    ap.add_argument("--twin", help="fault-free twin journal (optional)")
    ap.add_argument("--out", help="output prefix for .edr/.json/.csv")
    ap.add_argument("--period-ms", type=float, default=5.0, help="tick period in ms")
    ap.add_argument("--dump", help="decode an existing .edr file to text")
    ap.add_argument("--verbose", action="store_true", help="print records as text")
    args = ap.parse_args(argv)

    if args.dump:
        for rec in read_binary(args.dump):
            sys.stdout.write(format_record_text(rec))
        return 0
    if not args.journal or not args.out:
        ap.error("--journal and --out are required (or use --dump)")
    act = read_journal(args.journal)
    twin = _load_optional(args.twin)
    records = build_records(act, twin, args.period_ms, args.journal)
    base = args.out
    write_binary(base + ".edr", records)
    write_json(base + ".json", records)
    write_csv(base + ".csv", records)
    if args.verbose:
        for rec in records:
            sys.stdout.write(format_record_text(rec))
    print("edr: %d record(s) -> %s.edr / .json / .csv" % (len(records), base))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())