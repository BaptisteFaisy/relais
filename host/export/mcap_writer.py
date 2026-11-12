#!/usr/bin/env python3
"""Minimal MCAP writer and reader for Relais journals (OBS-08).

MCAP is the container format described at mcap.dev. This module implements the
subset we need, in the Python standard library only:

* header, schema (JSON Schema), channel, message (JSON payload), statistics,
  footer and data-end records;
* a flat (non-chunked) data section and a flat summary section, each with a
  real CRC-32, so `mcap info` / `mcap doctor` can verify the file;
* one channel per Relais stream: "tick", "event", "twin" (fault-free twin).

The JSON schemas are derived from the actual keys of the journal, one per
channel. Message log-time is the record `ns` when non-zero, otherwise the tick
number scaled by the run period T (so archived replays keep a monotonic
timeline). Deliberately NOT implemented (documented subset): chunking,
message/chunk indexes, attachments, arbitrary metadata — all optional parts of
MCAP and unnecessary for these journals. Unchunked files let readers iterate
messages without a summary; we still emit the summary so summary-based readers
and `mcap doctor` are happy.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib

MAGIC = b"\x89MCAP0\r\n"

OP_HEADER = 0x01
OP_FOOTER = 0x02
OP_SCHEMA = 0x03
OP_CHANNEL = 0x04
OP_MESSAGE = 0x05
OP_STATISTICS = 0x0B
OP_DATA_END = 0x0F


def _u16(v):
    return struct.pack("<H", v)


def _u32(v):
    return struct.pack("<I", v)


def _u64(v):
    return struct.pack("<Q", v)


def _string(s):
    b = s.encode("utf-8")
    return _u32(len(b)) + b


def _map(pairs):
    body = b"".join(_string(k) + _string(v) for k, v in pairs)
    return _u32(len(body)) + body


def _crc(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def _record(opcode, content):
    return bytes([opcode]) + _u64(len(content)) + content


def _derive_type(value):
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    return None


def _derive_properties(records):
    """JSON-Schema properties from the union of keys actually seen."""
    props = {}
    for rec in records:
        for key, value in rec.items():
            t = _derive_type(value)
            if t is not None:
                props.setdefault(key, {"type": t})
    return props


def _schema_json(properties):
    return json.dumps(
        {"type": "object", "properties": properties,
         "additionalProperties": True},
        separators=(",", ":")).encode("utf-8")


def _time_ns(rec, period_ns):
    ns = rec.get("ns", 0)
    if isinstance(ns, (int, float)) and ns > 0:
        return int(ns)
    tick = rec.get("tick", 0)
    return int(tick) * period_ns


def _records_by_kind(records):
    ticks = [r for r in records if r.get("kind") == "tick"]
    events = [r for r in records if r.get("kind") == "event"]
    return ticks, events


def _period_ns(records):
    for r in records:
        if r.get("kind") == "header" and isinstance(r.get("params"), dict):
            try:
                return int(float(r["params"].get("T", 5.0)) * 1_000_000)
            except (TypeError, ValueError):
                return 5_000_000
    return 5_000_000


class McapWriter(object):
    """Incrementally builds a valid MCAP byte stream."""

    def __init__(self):
        self._buf = bytearray(MAGIC)
        self._schemas = []
        self._channels = []
        self._schema_ids = []
        self._channel_ids = []
        self._topic_counts = {}
        self._msg_count = 0
        self._first_time = None
        self._last_time = None
        self._header_end = None

    def write_header(self, profile="relais", library="relais-mcap-py"):
        self._buf += _record(OP_HEADER, _string(profile) + _string(library))
        self._header_end = len(self._buf)

    def write_schema(self, sid, name, data):
        self._schemas.append((sid, name, data))
        if sid not in self._schema_ids:
            self._schema_ids.append(sid)
        self._buf += _record(OP_SCHEMA,
                             _u16(sid) + _string(name) + _string("jsonschema")
                             + _u32(len(data)) + data)

    def write_channel(self, cid, sid, topic):
        self._channels.append((cid, sid, topic))
        if cid not in self._channel_ids:
            self._channel_ids.append(cid)
        self._buf += _record(OP_CHANNEL, _u16(cid) + _u16(sid)
                             + _string(topic) + _string("json") + _map([]))

    def write_message(self, cid, log_time, payload):
        seq = self._topic_counts.get(cid, 0)
        self._topic_counts[cid] = seq + 1
        self._msg_count += 1
        if self._first_time is None or log_time < self._first_time:
            self._first_time = log_time
        if self._last_time is None or log_time > self._last_time:
            self._last_time = log_time
        self._buf += _record(OP_MESSAGE, _u16(cid) + _u32(seq)
                             + _u64(log_time) + _u64(log_time)
                             + _u32(len(payload)) + payload)

    def _statistics(self):
        counts = b"".join(_u16(cid) + _u64(self._topic_counts[cid])
                          for cid in sorted(self._topic_counts))
        first = self._first_time or 0
        last = self._last_time or first
        # Wire order/sizes per mcap.dev: message_count u64, schema_count u16,
        # channel_count u32, attachment_count u32, metadata_count u32,
        # chunk_count u32, message_start_time u64, message_end_time u64,
        # channel_message_counts map<u16,u64>.
        body = (_u64(self._msg_count) + _u16(len(self._schema_ids))
                + _u32(len(self._channel_ids)) + _u32(0) + _u32(0) + _u32(0)
                + _u64(first) + _u64(last) + _u32(len(counts)) + counts)
        return _record(OP_STATISTICS, body)

    def _summary(self):
        out = bytearray()
        for sid, name, data in self._schemas:
            out += _record(OP_SCHEMA, _u16(sid) + _string(name)
                           + _string("jsonschema") + _u32(len(data)) + data)
        for cid, sid, topic in self._channels:
            out += _record(OP_CHANNEL, _u16(cid) + _u16(sid) + _string(topic)
                           + _string("json") + _map([]))
        out += self._statistics()
        return bytes(out)

    def finish(self):
        """Seal the file: data end, summary, footer, closing magic."""
        if self._header_end is None:
            self.write_header()
        data_end = len(self._buf)
        data_crc = _crc(self._buf[self._header_end:data_end])
        self._buf += _record(OP_DATA_END, _u32(data_crc))

        summary_start = len(self._buf)
        summary = self._summary()
        self._buf += summary
        summary_end = len(self._buf)
        summary_crc = _crc(self._buf[summary_start:summary_end])
        self._buf += _record(OP_FOOTER, _u64(summary_start) + _u64(0)
                             + _u32(summary_crc))
        self._buf += MAGIC
        return bytes(self._buf)


def convert(act_records, twin_records=None):
    """Convert journal records into a complete, verified MCAP byte string."""
    twin_records = twin_records or []
    period = _period_ns(act_records)
    ticks, events = _records_by_kind(act_records)
    twin_ticks, _ = _records_by_kind(twin_records)

    specs = {
        "tick": (1, "relais.Tick", ticks),
        "event": (2, "relais.Event", events),
        "twin": (3, "relais.Twin", twin_ticks),
    }
    channels = {
        "tick": (1, 1, "/relais/tick"),
        "event": (2, 2, "/relais/event"),
        "twin": (3, 3, "/relais/twin"),
    }

    w = McapWriter()
    w.write_header()
    for name in ("tick", "event", "twin"):
        sid, sname, recs = specs[name]
        if not recs:
            continue
        w.write_schema(sid, sname, _schema_json(_derive_properties(recs)))
    for name in ("tick", "event", "twin"):
        cid, sid, topic = channels[name]
        if not specs[name][2]:
            continue
        w.write_channel(cid, sid, topic)

    for name in ("tick", "event", "twin"):
        cid = channels[name][0]
        for rec in specs[name][2]:
            payload = json.dumps(rec, separators=(",", ":")).encode("utf-8")
            w.write_message(cid, _time_ns(rec, period), payload)
    return w.finish()


def _iter_records(data):
    """Yield (opcode, offset, content) for every record between the magics."""
    pos = len(MAGIC)
    limit = len(data) - len(MAGIC)
    while pos < limit:
        opcode = data[pos]
        length = struct.unpack_from("<Q", data, pos + 1)[0]
        content_start = pos + 9
        yield opcode, pos, data[content_start:content_start + length]
        pos = content_start + length


def read_mcap(data):
    """Parse an MCAP byte string back into a dict (round-trip verification)."""
    out = {"schemas": {}, "channels": {}, "messages": [], "statistics": None,
           "header": {}, "footer": None, "data_section_crc": None,
           "topics": {}}
    for opcode, offset, content in _iter_records(data):
        if opcode == OP_HEADER:
            pl = struct.unpack_from("<I", content, 0)[0]
            lib_off = 4 + pl
            ll = struct.unpack_from("<I", content, lib_off)[0]
            out["header"] = {"profile": content[4:4 + pl].decode("utf-8"),
                             "library": content[lib_off + 4:lib_off + 4 + ll].decode("utf-8")}
        elif opcode == OP_SCHEMA:
            sid = struct.unpack_from("<H", content, 0)[0]
            nl = struct.unpack_from("<I", content, 2)[0]
            name = content[6:6 + nl].decode("utf-8")
            out["schemas"][sid] = name
        elif opcode == OP_CHANNEL:
            cid = struct.unpack_from("<H", content, 0)[0]
            sid = struct.unpack_from("<H", content, 2)[0]
            tl = struct.unpack_from("<I", content, 4)[0]
            topic = content[8:8 + tl].decode("utf-8")
            out["channels"][cid] = {"schema_id": sid, "topic": topic}
            out["topics"][topic] = cid
        elif opcode == OP_MESSAGE:
            cid = struct.unpack_from("<H", content, 0)[0]
            seq = struct.unpack_from("<I", content, 2)[0]
            log_time = struct.unpack_from("<Q", content, 6)[0]
            dl = struct.unpack_from("<I", content, 22)[0]
            payload = content[26:26 + dl]
            out["messages"].append({"channel_id": cid, "sequence": seq,
                                    "log_time": log_time,
                                    "data": json.loads(payload.decode("utf-8"))})
        elif opcode == OP_STATISTICS:
            mc = struct.unpack_from("<Q", content, 0)[0]
            out["statistics"] = {"message_count": mc}
        elif opcode == OP_DATA_END:
            out["data_section_crc"] = struct.unpack_from("<I", content, 0)[0]
        elif opcode == OP_FOOTER:
            out["footer"] = {"summary_start": struct.unpack_from("<Q", content, 0)[0],
                             "summary_offset_start": struct.unpack_from("<Q", content, 8)[0],
                             "summary_crc": struct.unpack_from("<I", content, 16)[0]}
    return out


def verify(data):
    """Return a list of structural problems; empty means the file is sound."""
    problems = []
    if data[:8] != MAGIC or data[-8:] != MAGIC:
        problems.append("bad magic")
        return problems
    ops = [(op, off) for op, off, _ in _iter_records(data)]
    if not ops or ops[0][0] != OP_HEADER:
        problems.append("first record is not a header")
    if len(ops) < 2 or ops[-1][0] != OP_FOOTER:
        problems.append("last record is not a footer")
    # Size consistency: magics + sum(1 + 8 + len) == total.
    total = 16 + sum(9 + len(c) for _, _, c in _iter_records(data))
    if total != len(data):
        problems.append("record lengths do not tile the file")
    info = read_mcap(data)
    stats = info["statistics"] or {}
    if stats.get("message_count") != len(info["messages"]):
        problems.append("statistics message_count mismatch")
    for m in info["messages"]:
        if m["channel_id"] not in info["channels"]:
            problems.append("message references unknown channel %d" % m["channel_id"])
    return problems


def load_journal(path):
    """Tolerant JSON-lines load: returns (records, malformed_count)."""
    records, malformed = [], 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                malformed += 1
                continue
            if isinstance(obj, dict):
                records.append(obj)
    return records, malformed


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Convert a Relais journal to a valid MCAP file.")
    parser.add_argument("--journal", required=True,
                        help="actuator JSON-lines journal")
    parser.add_argument("--twin", default=None,
                        help="optional fault-free twin journal")
    parser.add_argument("--out", required=True, help="output .mcap path")
    args = parser.parse_args(argv)
    act, malformed = load_journal(args.journal)
    twin, twin_mal = ([], 0) if not args.twin else load_journal(args.twin)
    data = convert(act, twin)
    with open(args.out, "wb") as fh:
        fh.write(data)
    problems = verify(data)
    info = read_mcap(data)
    print("mcap: %s (%d bytes)" % (args.out, len(data)))
    print("  messages: %d  channels: %d  malformed: %d (+%d twin)"
          % (len(info["messages"]), len(info["channels"]), malformed, twin_mal))
    print("  verification: %s" % ("OK" if not problems else "; ".join(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())