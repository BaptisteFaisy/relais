"""Tests for the Relais Event Data Recorder (spec 12 #9).

Each handover type must produce the right fields; FDTI/FRTI are hand-verified
against the definitions in docs/SPEC.md and docs/SAFETY.md; the surrounding
extract is centred and bounded; a truncated journal must not break the recorder;
the fixed-size binary round-trips.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
for _inc in (HERE, os.path.join(HERE, "..", "report")):
    if _inc not in sys.path:
        sys.path.insert(0, _inc)

from journal_io import read_journal  # noqa: E402
import edr  # noqa: E402
from edr import build_records, handover_detections  # noqa: E402
from edr_format import (  # noqa: E402
    EdrRecord, record_to_binary, binary_to_record, write_binary, read_binary,
    write_json, write_csv, BINARY_RECORD_SIZE,
)


def _tick(tick, holder="", mandate=0, source="none", misses=0, lamp="off",
          cause="none", value=0.0, pos=0.0, vel=0.0, setpoint=0.0,
          certified=False, certified_node="", stop=False):
    return ('{"kind":"tick","tick":%d,"ns":0,"mandate":%d,"holder":"%s",'
            '"source":"%s","value":%g,"pos":%g,"vel":%g,"setpoint":%g,'
            '"misses":%d,"lamp":"%s","cause":"%s","certified":%s,'
            '"certified_node":"%s","stop":%s}\n' % (
                tick, mandate, holder, source, value, pos, vel, setpoint,
                misses, lamp, cause, "true" if certified else "false",
                certified_node, "true" if stop else "false"))


def _event(tick, event, node="", mandate=0, detail=0, text=""):
    return ('{"kind":"event","tick":%d,"ns":0,"event":"%s","node":"%s",'
            '"mandate":%d,"detail":%d,"text":"%s"}\n' % (tick, event, node, mandate, detail, text))


def _header():
    return ('{"kind":"header","version":1,"mutant":0,"scenario":"test",'
            '"scenario_id":0,"params":{"T":5,"p":1,"N":8,"certify":3,"bail":4,'
            '"eps":0.005,"delta":0.02,"degraded":6000}}\n')


def _write(lines):
    fd, path = tempfile.mkstemp(suffix=".jsonl")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.writelines(lines)
    return path


class TestDeathHandover(unittest.TestCase):
    """A death forces p+1 misses then an auto-switch to the certified second."""

    def _journal(self, path_out):
        lines = [_header()]
        # holder A holds the mandate alone, fresh until tick 100, certified B.
        for t in range(1, 101):
            lines.append(_tick(t, holder="A", mandate=1, source="holder",
                               certified=True, certified_node="B", value=0.1, pos=0.01 * t))
        lines.append(_event(1, "mandate_granted", "A", 1, 2, "demande"))
        # tick 101: A misses (shadow of the certified B covers it).
        lines.append(_tick(101, holder="A", mandate=1, source="shadow", misses=1,
                           lamp="yellow", cause="node_missing",
                           certified=True, certified_node="B", value=0.1, pos=0.01 * 101))
        # tick 102: second consecutive miss -> auto-switch to B (mandate 2).
        lines.append(_tick(102, holder="B", mandate=2, source="shadow", misses=0,
                           lamp="yellow", cause="node_missing",
                           certified=True, certified_node="B", value=0.1, pos=0.01 * 102))
        lines.append(_event(102, "mandate_auto_switch", "B", 2, 2, "evidence"))
        # tick 103: B's first fresh order.
        lines.append(_tick(103, holder="B", mandate=2, source="holder", misses=0,
                           lamp="yellow", cause="second_uncertified",
                           certified=False, value=0.1, pos=0.01 * 103))
        with open(path_out, "w", encoding="utf-8") as f:
            f.writelines(lines)

    def test_single_record_with_correct_fields(self):
        p = tempfile.mktemp(suffix=".jsonl")
        self._journal(p)
        act = read_journal(p)
        records = build_records(act, None, 5.0, p)
        self.assertEqual(len(records), 1, "the initial grant must not count as a handover")
        r = records[0]
        self.assertEqual((r.holder_old, r.holder_new), ("A", "B"))
        self.assertEqual((r.mandate_old, r.mandate_new), (1, 2))
        self.assertEqual(r.cause, "death")
        # Hand-verified: A's last fresh order is 100, detection is 102 -> FDTI = 2
        # (p+1 with p = 1); B's first fresh order is 103 -> FRTI = 1.
        self.assertEqual(r.fdti_ticks, 2, "FDTI must be p+1 = 2 ticks")
        self.assertEqual(r.frti_ticks, 1, "FRTI must be 1 tick to B's first order")
        # Substitution period: ticks 101, 102 (shadow) then 103 (holder).
        self.assertEqual(r.source_run, ["shadow", "shadow", "holder"])
        self.assertEqual(r.tick, 102, "record tick is the detection (constat) tick")
        os.unlink(p)


class TestCessionHandover(unittest.TestCase):
    """A process crash = cession: revoke to vacancy, then a DEMANDE grant."""

    def _journal(self, path_out):
        lines = [_header()]
        for t in range(1, 151):
            lines.append(_tick(t, holder="A", mandate=1, source="holder",
                               value=0.1, pos=0.01 * t))
        lines.append(_event(1, "mandate_granted", "A", 1, 2, "demande"))
        lines.append(_event(150, "cession", "A", 1, 1, "crash"))
        lines.append(_tick(151, holder="", mandate=1, source="testament", misses=0,
                           lamp="yellow", cause="node_missing", value=0.1, pos=0.01 * 151))
        lines.append(_event(151, "mandate_revoked", "A", 1, 0, ""))
        lines.append(_tick(152, holder="B", mandate=2, source="holder", misses=0,
                           lamp="yellow", cause="second_uncertified", value=0.1, pos=0.01 * 152))
        lines.append(_event(152, "mandate_granted", "B", 2, 2, "demande"))
        with open(path_out, "w", encoding="utf-8") as f:
            f.writelines(lines)

    def test_cession_bridged_into_one_record(self):
        p = tempfile.mktemp(suffix=".jsonl")
        self._journal(p)
        act = read_journal(p)
        records = build_records(act, None, 5.0, p)
        self.assertEqual(len(records), 1, "cession (A -> vacant -> B) is one handover")
        r = records[0]
        self.assertEqual((r.holder_old, r.holder_new), ("A", "B"))
        self.assertEqual(r.cause, "cession")
        # Detection (cession event) is 150; A's last fresh = 150 -> FDTI = 0;
        # B's first fresh order is 152 -> FRTI = 2 (revoke + grant at level S).
        self.assertEqual(r.tick, 150)
        self.assertEqual(r.fdti_ticks, 0, "a cession is detected the instant it is recorded")
        self.assertEqual(r.frti_ticks, 2)
        os.unlink(p)


class TestExtractAndTruncation(unittest.TestCase):
    def _long_journal(self, path_out, ticks=200):
        lines = [_header()]
        for t in range(1, ticks + 1):
            if t <= 100:
                lines.append(_tick(t, holder="A", mandate=1, source="holder", value=0.1, pos=0.01 * t))
            elif t == 101:
                lines.append(_tick(101, holder="A", mandate=1, source="shadow", misses=1,
                                   lamp="yellow", cause="node_missing", value=0.1, pos=0.01 * t))
            elif t == 102:
                lines.append(_tick(102, holder="B", mandate=2, source="shadow", misses=0,
                                   lamp="yellow", cause="node_missing", value=0.1, pos=0.01 * t))
            else:
                lines.append(_tick(t, holder="B", mandate=2, source="holder", value=0.1, pos=0.01 * t))
        lines.append(_event(102, "mandate_auto_switch", "B", 2, 2, "evidence"))
        with open(path_out, "w", encoding="utf-8") as f:
            f.writelines(lines)

    def test_extract_centered_and_bounded(self):
        p = tempfile.mktemp(suffix=".jsonl")
        self._long_journal(p)
        act = read_journal(p)
        records = build_records(act, None, 5.0, p)
        r = records[0]
        self.assertTrue(r.extract, "the surrounding extract must not be empty")
        ticks = []
        import json
        for line in r.extract:
            ticks.append(int(json.loads(line)["tick"]))
        self.assertEqual(min(ticks), 102 - 20, "extract starts at detection - 20")
        self.assertEqual(max(ticks), 102 + 20, "extract ends at detection + 20")
        # 41 tick lines plus the one auto-switch event line in the same window.
        self.assertLessEqual(len(r.extract), 43, "extract is bounded to the +/- 20 window")
        os.unlink(p)

    def test_truncated_journal_does_not_break(self):
        p = tempfile.mktemp(suffix=".jsonl")
        self._long_journal(p)
        with open(p, "r", encoding="utf-8") as f:
            data = f.read()
        cut = data[: len(data) // 2]
        p2 = p + ".truncated.jsonl"
        with open(p2, "w", encoding="utf-8") as f:
            f.write(cut)
        act = read_journal(p2)
        records = build_records(act, None, 5.0, p2)
        self.assertGreaterEqual(len(records), 0, "a truncated journal yields records or none")
        os.unlink(p)
        os.unlink(p2)


class TestBinaryRoundTrip(unittest.TestCase):
    def test_fixed_size_round_trip(self):
        rec = EdrRecord(
            tick=123, ns=7, t_ms=615.0, mandate_old=1, mandate_new=2,
            holder_old="A", holder_new="B", cause="death", lamp="yellow",
            certified_new=False, fdti_ticks=2, frti_ticks=1,
            source_run=["shadow", "holder"], max_dev=0.001, max_pos_dev=0.002,
            detail=2, extract=["line1", "line2"],
        )
        raw = record_to_binary(rec)
        self.assertEqual(len(raw), BINARY_RECORD_SIZE, "record must be fixed size")
        back = binary_to_record(raw)
        for field in ("tick", "ns", "t_ms", "mandate_old", "mandate_new",
                      "holder_old", "holder_new", "cause", "lamp", "fdti_ticks",
                      "frti_ticks", "source_run", "detail"):
            self.assertEqual(getattr(rec, field), getattr(back, field), field)
        self.assertAlmostEqual(rec.max_dev, back.max_dev)
        self.assertAlmostEqual(rec.max_pos_dev, back.max_pos_dev)

    def test_write_read_binary_file(self):
        recs = [EdrRecord(tick=i, mandate_new=1, holder_new="A", cause="death")
                for i in range(1, 4)]
        p = tempfile.mktemp(suffix=".edr")
        write_binary(p, recs)
        back = read_binary(p)
        self.assertEqual(len(back), 3)
        self.assertEqual([r.tick for r in back], [1, 2, 3])
        os.unlink(p)

    def test_json_csv_exports(self):
        recs = [EdrRecord(tick=42, mandate_new=2, holder_old="A", holder_new="B",
                          cause="freeze", source_run=["testament", "holder"])]
        jp = tempfile.mktemp(suffix=".json")
        cp = tempfile.mktemp(suffix=".csv")
        write_json(jp, recs)
        write_csv(cp, recs)
        with open(jp, encoding="utf-8") as f:
            jtxt = f.read()
        self.assertIn('"fdti_ticks":', jtxt)
        self.assertIn('"cause":"freeze"', jtxt)
        with open(cp, encoding="utf-8") as f:
            header = f.readline()
            row = f.readline()
        self.assertTrue(header.startswith("tick,t_ms,mandate_old"))
        self.assertIn("freeze", row)
        os.unlink(jp)
        os.unlink(cp)


if __name__ == "__main__":
    unittest.main(verbosity=2)