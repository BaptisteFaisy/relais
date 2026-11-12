"""Fixed-size binary envelope for Relais Event Data Recorder records (spec 12 #9).

An EDR record is the durable trace of one mandate change ("handover"). This
module holds the record dataclass and three interchangeable encodings:

  * binary  -- fixed-size records behind a small header, so an after-accident
               auditor can read the fields without the bench (OBS-05);
  * JSON    -- one line per record, self-describing, carries the surrounding
               journal extract and the field names;
  * CSV     -- a flat table, one row per record, for a spreadsheet.

The binary layout is fixed and documented byte by byte below so it can be read
by a third party with no Python: this is the "readable after the accident"
guarantee.

Node enum (relais/types.h RELAIS_NODE_*): 0 none, 1 A, 2 B, 3 C, 4 actuator.
EDR cause enum (local, stable): 0 none, 1 death, 2 freeze, 3 cession, 4 cut,
5 missing, 6 disagreement, 7 fallback, 8 other.
Source enum (relais/types.h RELAIS_SRC_*): 0 none, 1 holder, 2 shadow,
3 testament, 4 fallback.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field, asdict
from typing import Dict, List, Optional, Tuple

EDR_MAGIC = b"EDR1"
EDR_VERSION = 1
EDR_SOURCE_SLOTS = 16  # fixed width of the source-of-ticks run (window <= 16)

# cause enum
EDR_CAUSE = {
    "none": 0, "death": 1, "freeze": 2, "cession": 3, "cut": 4,
    "missing": 5, "disagreement": 6, "fallback": 7, "other": 8,
}
EDR_CAUSE_NAME = {v: k for k, v in EDR_CAUSE.items()}

NODE_NAME = {0: "", 1: "A", 2: "B", 3: "C", 4: "actuator"}
NODE_OF = {v: k for k, v in NODE_NAME.items() if k}
SOURCE_NAME = {0: "none", 1: "holder", 2: "shadow", 3: "testament", 4: "fallback"}

# Binary header: magic (4) + version (2) + record_size (2) + count (4).
_BIN_HEADER = struct.Struct("<4sHHI")

# Binary record, field order (widths), from least to most:
#   tick Q, ns Q, t_ms d, mandate_old I, mandate_new I,
#   holder_old B, holder_new B, cause B, lamp B, certified_new B,
#   fdti H, frti H, window_len H, reserved B,
#   source_run 16B (SOURCE enum per tick, zero padded),
#   max_dev d, max_pos_dev d, detail I, reserved2 H
_BIN_RECORD = struct.Struct("<QQdIIBBBBBHHHB16sddIH")
BINARY_RECORD_SIZE = _BIN_RECORD.size


@dataclass
class EdrRecord:
    """One handover record. FDTI/FRTI are in ticks, per docs/SPEC.md."""
    tick: int = 0          # detection tick (first tick with the new holder)
    ns: int = 0            # actuator ns when the change was journaled
    t_ms: float = 0.0      # wall-clock timestamp = tick * period_ms
    mandate_old: int = 0
    mandate_new: int = 0
    holder_old: str = ""
    holder_new: str = ""
    cause: str = "none"    # death|freeze|cession|cut|missing|...
    lamp: str = "off"      # worst lamp during the handover window
    certified_new: bool = False
    fdti_ticks: int = 0    # fault -> detection (p+1 for a death, p=1)
    frti_ticks: int = 0    # detection -> first fresh order of the new holder
    source_run: List[str] = field(default_factory=list)
    max_dev: float = 0.0      # max |value - twin value| over the window
    max_pos_dev: float = 0.0  # max |pos   - twin pos|   over the window
    detail: int = 0           # event detail: miss count evidence, reject reason
    extract: List[str] = field(default_factory=list)  # surrounding journal lines


def _lamp_code(lamp: str) -> int:
    return {"off": 0, "yellow": 1, "red": 2}.get(lamp, 0)


def _lamp_name(code: int) -> str:
    return {0: "off", 1: "yellow", 2: "red"}.get(code, "off")


def _pad_sources(run: List[str]) -> bytes:
    codes = [SOURCE_NAME.get("none", 0)] * EDR_SOURCE_SLOTS
    _m = {v: k for k, v in SOURCE_NAME.items()}
    for i, s in enumerate(run[:EDR_SOURCE_SLOTS]):
        codes[i] = _m.get(s, 0)
    return bytes(codes)


def _unpad_sources(raw: bytes, length: int) -> List[str]:
    return [SOURCE_NAME.get(raw[i], "none") for i in range(min(length, EDR_SOURCE_SLOTS))]


def record_to_binary(rec: EdrRecord) -> bytes:
    """Encode one record into its fixed-size binary form."""
    return _BIN_RECORD.pack(
        rec.tick, rec.ns, rec.t_ms, rec.mandate_old, rec.mandate_new,
        NODE_OF.get(rec.holder_old, 0), NODE_OF.get(rec.holder_new, 0),
        EDR_CAUSE.get(rec.cause, 8), _lamp_code(rec.lamp),
        1 if rec.certified_new else 0,
        rec.fdti_ticks, rec.frti_ticks, len(rec.source_run), 0,
        _pad_sources(rec.source_run),
        float(rec.max_dev), float(rec.max_pos_dev), rec.detail, 0,
    )


def binary_to_record(raw: bytes) -> EdrRecord:
    """Decode one fixed-size record; raises struct.error on a bad buffer."""
    (tick, ns, t_ms, mo, mn, ho, hn, cause, lamp, cert, fdti, frti, wlen,
     _reserved, sources, max_dev, max_pos_dev, detail, _res2) = _BIN_RECORD.unpack(raw)
    return EdrRecord(
        tick=tick, ns=ns, t_ms=t_ms, mandate_old=mo, mandate_new=mn,
        holder_old=NODE_NAME.get(ho, ""), holder_new=NODE_NAME.get(hn, ""),
        cause=EDR_CAUSE_NAME.get(cause, "other"), lamp=_lamp_name(lamp),
        certified_new=bool(cert), fdti_ticks=fdti, frti_ticks=frti,
        source_run=_unpad_sources(sources, wlen),
        max_dev=max_dev, max_pos_dev=max_pos_dev, detail=detail, extract=[],
    )


def write_binary(path: str, records: List[EdrRecord]) -> int:
    """Write records to a .edr file; returns bytes written."""
    header = _BIN_HEADER.pack(EDR_MAGIC, EDR_VERSION, BINARY_RECORD_SIZE, len(records))
    body = b"".join(record_to_binary(r) for r in records)
    with open(path, "wb") as f:
        f.write(header + body)
    return len(header) + len(body)


def read_binary(path: str) -> List[EdrRecord]:
    """Read a .edr file into records; raises on a corrupt header."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < _BIN_HEADER.size:
        raise ValueError("EDR file too short: %s" % path)
    magic, version, record_size, count = _BIN_HEADER.unpack_from(data, 0)
    if magic != EDR_MAGIC:
        raise ValueError("not an EDR file: %s" % path)
    if record_size != BINARY_RECORD_SIZE:
        raise ValueError("EDR record size mismatch: %d != %d" % (record_size, BINARY_RECORD_SIZE))
    records: List[EdrRecord] = []
    off = _BIN_HEADER.size
    for _ in range(count):
        records.append(binary_to_record(data[off:off + record_size]))
        off += record_size
    return records


def _row(rec: EdrRecord) -> Dict[str, object]:
    d: Dict[str, object] = {}
    for key, value in asdict(rec).items():
        if key == "extract":
            continue
        d[key] = value
    return d


def record_to_json(rec: EdrRecord) -> str:
    """One JSON line, verifiable by hand, including the journal extract."""
    import json
    d = _row(rec)
    d["extract"] = list(rec.extract)
    return json.dumps(d, ensure_ascii=True, separators=(",", ":"))


def write_json(path: str, records: List[EdrRecord]) -> int:
    """Write records as JSON lines; returns number of records written."""
    with open(path, "w", encoding="utf-8") as f:
        for r in records:
            f.write(record_to_json(r) + "\n")
    return len(records)


CSV_COLUMNS = (
    "tick", "t_ms", "mandate_old", "mandate_new", "holder_old", "holder_new",
    "cause", "lamp", "fdti_ticks", "frti_ticks", "source_run", "max_dev",
    "max_pos_dev", "detail",
)


def record_to_csv(rec: EdrRecord) -> str:
    """One CSV row; the source_run is a compact slash-joined string."""
    cells = [
        str(rec.tick), "%.3f" % rec.t_ms, str(rec.mandate_old), str(rec.mandate_new),
        rec.holder_old, rec.holder_new, rec.cause, rec.lamp,
        str(rec.fdti_ticks), str(rec.frti_ticks),
        "/".join(rec.source_run) if rec.source_run else "",
        "%.6g" % rec.max_dev, "%.6g" % rec.max_pos_dev, str(rec.detail),
    ]
    return ",".join(cells) + "\n"


def write_csv(path: str, records: List[EdrRecord]) -> int:
    """Write records as CSV (header + one row each)."""
    with open(path, "w", encoding="utf-8") as f:
        f.write(",".join(CSV_COLUMNS) + "\n")
        for r in records:
            f.write(record_to_csv(r))
    return len(records)


def format_record_text(rec: EdrRecord) -> str:
    """Human-readable single record (the 'lisible sans le banc' rendering)."""
    lines = [
        "=== handover @ tick %d (t=%.3f ms) ===" % (rec.tick, rec.t_ms),
        "  mandate %d -> %d, holder '%s' -> '%s'" % (
            rec.mandate_old, rec.mandate_new, rec.holder_old or "<vacant>", rec.holder_new or "<vacant>"),
        "  cause: %-12s lamp: %-6s certified_new: %s" % (rec.cause, rec.lamp, rec.certified_new),
        "  FDTI: %d tick(s)   FRTI: %d tick(s)" % (rec.fdti_ticks, rec.frti_ticks),
        "  source of ticks during handover: %s" % (" ".join(rec.source_run) or "-"),
        "  max |value-twin|: %.6g   max |pos-twin|: %.6g   detail: %d" % (
            rec.max_dev, rec.max_pos_dev, rec.detail),
    ]
    for line in rec.extract:
        lines.append("      " + line.rstrip())
    return "\n".join(lines) + "\n"