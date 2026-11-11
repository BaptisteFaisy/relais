"""Tolerant reader and analyser for Relais campaign journals (OBS-05, OBS-07).

Reads the JSON-lines journal described in CONTRACT.md ("Journal format
(authoritative)"). One malformed line is counted and kept as a sample, never
raised. Also discovers a campaign tree (run folders holding actuator.jsonl,
twin.jsonl and an optional scenario.json) and analyses each run against the
safety invariants, mirroring the subset checked by host/verify/relais-verify.c
(I1, I2, I3, I4, I5, I6, I8, I10, I11). The C verifier stays the authority;
this module reproduces its judgement so the HTML report can rank worst cases.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional

DEFAULT_PARAMS: Dict[str, float] = {
    "T": 5.0, "p": 1.0, "N": 8.0, "certify": 3.0, "bail": 4.0,
    "eps": 0.005, "delta": 0.02, "degraded": 6000.0,
}
SOURCES = ("none", "holder", "shadow", "testament", "fallback")
FRESH_SOURCES = ("holder", "shadow")
MANDATE_EVENTS = ("mandate_granted", "mandate_auto_switch", "mandate_revoked")


@dataclass
class Header:
    """Journal header line: version, mutant, scenario, parameters."""
    version: int = 1
    mutant: int = 0
    scenario: str = ""
    scenario_id: int = 0
    params: Dict[str, float] = field(default_factory=lambda: dict(DEFAULT_PARAMS))


@dataclass
class Tick:
    """One applied-value record of the actuator journal."""
    tick: int = 0
    ns: int = 0
    mandate: int = 0
    holder: str = ""
    source: str = "none"
    value: float = 0.0
    pos: float = 0.0
    vel: float = 0.0
    setpoint: float = 0.0
    misses: int = 0
    lamp: str = "off"
    cause: str = "none"
    certified: bool = False
    certified_node: str = ""
    stop: bool = False


@dataclass
class Event:
    """One event record of the actuator journal."""
    tick: int = 0
    ns: int = 0
    event: str = ""
    node: str = ""
    mandate: int = 0
    detail: int = 0
    text: str = ""


@dataclass
class Journal:
    """A parsed journal: header (if any), ticks, events, malformed lines."""
    path: str = ""
    header: Optional[Header] = None
    ticks: List[Tick] = field(default_factory=list)
    events: List[Event] = field(default_factory=list)
    malformed: int = 0
    malformed_samples: List[str] = field(default_factory=list)


def _num(d: Dict, key: str, default: float = 0.0) -> float:
    """Best-effort numeric field; default when absent or not a number."""
    v = d.get(key, default)
    try:
        return float(v)
    except (TypeError, ValueError):
        return float(default)


def _parse_header(d: Dict) -> Header:
    params = dict(DEFAULT_PARAMS)
    raw = d.get("params")
    if isinstance(raw, dict):
        for k, v in raw.items():
            try:
                params[str(k)] = float(v)
            except (TypeError, ValueError):
                pass
    return Header(version=int(_num(d, "version", 1)), mutant=int(_num(d, "mutant", 0)),
                  scenario=str(d.get("scenario", "")),
                  scenario_id=int(_num(d, "scenario_id", 0)), params=params)


def _parse_tick(d: Dict) -> Tick:
    src = str(d.get("source", "none"))
    if src not in SOURCES:
        raise ValueError("bad source %r" % src)
    return Tick(tick=int(_num(d, "tick")), ns=int(_num(d, "ns")),
                mandate=int(_num(d, "mandate")), holder=str(d.get("holder", "")),
                source=src, value=_num(d, "value"), pos=_num(d, "pos"),
                vel=_num(d, "vel"), setpoint=_num(d, "setpoint"),
                misses=int(_num(d, "misses")), lamp=str(d.get("lamp", "off")),
                cause=str(d.get("cause", "none")),
                certified=bool(d.get("certified", False)),
                certified_node=str(d.get("certified_node", "")),
                stop=bool(d.get("stop", False)))


def _parse_event(d: Dict) -> Event:
    name = str(d.get("event", ""))
    if not name:
        raise ValueError("event without a name")
    return Event(tick=int(_num(d, "tick")), ns=int(_num(d, "ns")), event=name,
                 node=str(d.get("node", "")), mandate=int(_num(d, "mandate")),
                 detail=int(_num(d, "detail", 0)), text=str(d.get("text", "")))


def read_journal(path: str) -> Journal:
    """Read a JSON-lines journal. Tolerant: never raises on bad content."""
    j = Journal(path=path)
    try:
        f = open(path, "r", encoding="utf-8", errors="replace")
    except OSError:
        j.malformed = 1
        j.malformed_samples.append("unreadable file: %s" % path)
        return j
    with f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            _feed_line(j, line)
    return j


def _feed_line(j: Journal, line: str) -> None:
    """Parse one journal line into j; count it as malformed on any problem."""
    try:
        d = json.loads(line)
        if not isinstance(d, dict):
            raise ValueError("not a JSON object")
        kind = d.get("kind")
        if kind == "header":
            j.header = _parse_header(d)
        elif kind == "tick":
            j.ticks.append(_parse_tick(d))
        elif kind == "event":
            j.events.append(_parse_event(d))
        else:
            raise ValueError("unknown kind %r" % kind)
    except (ValueError, KeyError, TypeError) as exc:
        j.malformed += 1
        if len(j.malformed_samples) < 5:
            j.malformed_samples.append("%s: %s" % (exc, line[:120]))


def twin_values(twin: Optional[Journal]) -> Dict[int, float]:
    """Map tick -> applied value of the fault-free twin journal."""
    if twin is None:
        return {}
    return {t.tick: t.value for t in twin.ticks}


@dataclass
class Run:
    """One campaign run: a folder with actuator.jsonl (+ twin, scenario)."""
    name: str
    path: str
    journal: Journal
    twin: Optional[Journal] = None
    scenario: Optional[Dict] = None
    fault_class: str = "nominal"


def classify_faults(scenario: Optional[Dict], dirname: str) -> str:
    """Classify a run: nominal, death, freeze, cut, slow, double, other."""
    faults = (scenario or {}).get("faults") or []
    types = [str(f.get("type", "")) for f in faults if isinstance(f, dict)]
    if len([t for t in types if t not in ("", "thaw", "restore")]) >= 2:
        return "double"
    if types:
        t = types[0]
        if t in ("death", "freeze", "cut", "slow"):
            return t
        return "other"
    low = dirname.lower()
    for key in ("double", "death", "freeze", "cut", "slow"):
        if key in low:
            return key
    return "nominal"


def load_run(dirpath: str) -> Optional[Run]:
    """Load one run folder; None when actuator.jsonl is missing."""
    act = os.path.join(dirpath, "actuator.jsonl")
    if not os.path.isfile(act):
        return None
    journal = read_journal(act)
    twin_path = os.path.join(dirpath, "twin.jsonl")
    twin = read_journal(twin_path) if os.path.isfile(twin_path) else None
    scenario: Optional[Dict] = None
    sc_path = os.path.join(dirpath, "scenario.json")
    if os.path.isfile(sc_path):
        try:
            with open(sc_path, "r", encoding="utf-8") as f:
                loaded = json.load(f)
            scenario = loaded if isinstance(loaded, dict) else None
        except (OSError, ValueError):
            scenario = None
    name = (scenario or {}).get("name") or os.path.basename(os.path.normpath(dirpath))
    return Run(name=str(name), path=dirpath, journal=journal, twin=twin,
               scenario=scenario,
               fault_class=classify_faults(scenario, os.path.basename(dirpath)))


def discover_runs(root: str) -> List[Run]:
    """Walk a campaign tree and load every run folder, sorted by name."""
    runs: List[Run] = []
    if os.path.isfile(root):
        root = os.path.dirname(root)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        if "actuator.jsonl" not in filenames:
            continue
        run = load_run(dirpath)
        if run is not None:
            runs.append(run)
    runs.sort(key=lambda r: r.name)
    return runs
