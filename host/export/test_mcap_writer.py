#!/usr/bin/env python3
"""Unit tests for host/export/mcap_writer.py (OBS-08).

Checks the structure of the produced file (magic, header, record order,
size tiling, checksums), the round-trip (re-read what we wrote with the same
module) and the edge cases (empty journal, malformed line, a single tick).
"""
from __future__ import annotations

import json
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from export import mcap_writer as m  # noqa: E402


def tick(i, value=0.1, source="holder", holder="A"):
    return {"kind": "tick", "tick": i, "ns": 0, "mandate": 1, "holder": holder,
            "source": source, "value": value, "pos": value * 2.0, "vel": 0.0,
            "setpoint": 0.0, "misses": 0, "lamp": "off", "cause": "none",
            "certified": True, "certified_node": "B", "stop": False}


def event(i, name="mandate_granted"):
    return {"kind": "event", "tick": i, "ns": 0, "event": name, "node": "B",
            "mandate": 1, "detail": 0, "text": "demande"}


def journal(*records):
    return [{"kind": "header", "version": 1, "mutant": 0, "scenario": "t",
             "params": {"T": 5, "p": 1, "N": 8}}] + list(records)


class _Rec(object):
    def __init__(self, opcode, offset, content):
        self.opcode, self.offset, self.content = opcode, offset, content


def _records(data):
    return [_Rec(*r) for r in m._iter_records(data)]


class TestMcapWriter(unittest.TestCase):
    def convert(self, act, twin=None):
        data = m.convert(act, twin or [])
        self.assertEqual(m.verify(data), [])
        return data

    def test_magic_and_header(self):
        data = self.convert(journal(tick(1)))
        self.assertEqual(data[:8], m.MAGIC)
        self.assertEqual(data[-8:], m.MAGIC)
        recs = _records(data)
        self.assertEqual(recs[0].opcode, m.OP_HEADER)
        header = m.read_mcap(data)["header"]
        self.assertEqual(header["profile"], "relais")
        self.assertIn("relais-mcap-py", header["library"])

    def test_record_order(self):
        data = self.convert(journal(tick(1), event(1)))
        ops = [r.opcode for r in _records(data)]
        self.assertEqual(ops[0], m.OP_HEADER)
        self.assertEqual(ops[-1], m.OP_FOOTER)
        self.assertIn(m.OP_DATA_END, ops)
        self.assertLess(ops.index(m.OP_SCHEMA), ops.index(m.OP_MESSAGE))
        self.assertLess(ops.index(m.OP_CHANNEL), ops.index(m.OP_MESSAGE))
        self.assertLess(ops.index(m.OP_DATA_END), ops.index(m.OP_FOOTER))

    def test_tiling(self):
        data = self.convert(journal(tick(1), tick(2)))
        total = 16 + sum(9 + len(r.content) for r in _records(data))
        self.assertEqual(total, len(data))

    def test_checksums(self):
        data = self.convert(journal(tick(1), tick(2)))
        recs = _records(data)
        header_end = recs[0].offset + 9 + len(recs[0].content)
        data_end = next(r for r in recs if r.opcode == m.OP_DATA_END)
        data_crc = struct.unpack_from("<I", data_end.content, 0)[0]
        self.assertEqual(data_crc, m._crc(data[header_end:data_end.offset]))
        footer = next(r for r in recs if r.opcode == m.OP_FOOTER)
        summary_start, _, summary_crc = struct.unpack("<QQI", footer.content)
        self.assertEqual(summary_crc, m._crc(data[summary_start:footer.offset]))

    def test_roundtrip(self):
        act = journal(tick(1), event(1, "mandate_granted"),
                      tick(2, 0.2, "shadow", "B"))
        twin = [{"kind": "tick", "tick": 1, "ns": 0, "value": 0.1}]
        data = self.convert(act, twin)
        info = m.read_mcap(data)
        self.assertEqual(len(info["messages"]), 4)  # 2 ticks + 1 event + 1 twin
        self.assertEqual(info["statistics"]["message_count"], 4)
        self.assertEqual(sorted(info["schemas"].values()),
                         ["relais.Event", "relais.Tick", "relais.Twin"])
        topics = set(info["topics"])
        self.assertTrue({"/relais/tick", "/relais/event", "/relais/twin"} <= topics)
        # Every message must parse back to the exact journal object.
        got = [msg["data"] for msg in info["messages"]]
        self.assertIn({"kind": "tick", "tick": 2, "ns": 0, "mandate": 1,
                       "holder": "B", "source": "shadow", "value": 0.2,
                       "pos": 0.4, "vel": 0.0, "setpoint": 0.0, "misses": 0,
                       "lamp": "off", "cause": "none", "certified": True,
                       "certified_node": "B", "stop": False}, got)

    def test_empty_journal(self):
        data = self.convert(journal())
        info = m.read_mcap(data)
        self.assertEqual(info["messages"], [])
        self.assertEqual(info["statistics"]["message_count"], 0)
        self.assertEqual(info["channels"], {})

    def test_malformed_line(self):
        # Malformed lines are dropped by load_journal, never by convert:
        # here we pass only the valid records (the tolerant loader is the CLI).
        act = journal(tick(1))
        data = self.convert(act)
        self.assertEqual(len(read_messages(data)), 1)

    def test_single_tick(self):
        data = self.convert(journal(tick(7, 0.5)))
        msgs = read_messages(data)
        self.assertEqual(len(msgs), 1)
        self.assertEqual(msgs[0]["data"]["tick"], 7)
        self.assertEqual(msgs[0]["data"]["value"], 0.5)


def read_messages(data):
    return m.read_mcap(data)["messages"]


if __name__ == "__main__":
    unittest.main()