#!/usr/bin/env python3
"""Relais — golden traces: the behavioural fingerprint of the core (mission M35).

The core decides, every tick, who holds the mandate and which value reaches
the actuator. Two coming missions rewrite core/ in depth (three machines and a
vote, two actuator paths). Without a fingerprint of today's behaviour a
silent regression would merge: the sequence of decisions could change while
every unit test would still be green.

This module is the ONE implementation of that fingerprint:

  * a FROZEN game of scenarios (tools/golden/scenarios.json): seeds, faults
    and parameters are fixed, so every replay walks the same ticks;
  * the seeded simulator (build/relais-simulate) plays them: no clock, no
    jitter, no socket, the very same core as the QNX target (spec 7.4);
  * the journal is NORMALISED: every volatile field is neutralised, else the
    comparison would fail on every run;
  * the normalised journal is hashed with SHA-256: that hash IS the trace.

The trace says nothing about the timing of a real system (PERF-01) and
everything about the DECISIONS: the mandate, the applied value, the source of
the value (holder / shadow / testament / fallback), the lamp and the events
(OBS-01). Standard library only.
"""

import hashlib
import json
import os
import re
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GOLDEN_DIR = os.path.dirname(os.path.abspath(__file__))
MANIFEST_PATH = os.path.join(GOLDEN_DIR, "scenarios.json")
TRACES_PATH = os.path.join(GOLDEN_DIR, "traces.json")
SIMULATOR = os.path.join(ROOT, "build", "relais-simulate")

# Volatile fields of the journal, and why they are neutralised. The rules are
# idempotent: a field already equal to its neutral value counts as normalised.
VOLATILE_RULES = (
    ("ns", re.compile(r'"ns":\s*-?\d+'), '"ns":0',
     "nanosecond stamp of the record (wall clock on a real actuator)"),
    ("pid", re.compile(r'"pid":\s*\d+'), '"pid":0',
     "process identifier (volatile)"),
    ("stamp", re.compile(r'"(timestamp|ts|time_ms|wall_ms|epoch)":\s*-?\d+'),
     r'"\1":0', "wall-clock field (volatile)"),
    ("path", re.compile(r'"(/[^"]*)"'), '"<path>"',
     "absolute path (volatile: the run directory changes)"),
)

# The simulator's fault vocabulary: "at_tick:kind:node:arg:value".
# The bench (host/bench/relais_bench.py) only knows the PC-mode faults; the
# golden game must also reach the update, the cession, the forged order and
# the stale replay, so the translation lives here, once.
FAULT_KINDS = {
    "death": "death",
    "freeze": "freeze",
    "thaw": "thaw",
    "restore": "restore",
    "slow": "slow",
    "bias": "bias",
    "restart": "restart",
    "crash": "crash",
    "update": "update",
    "handover": "handover",
    "forge": "forge",
    "forge-stale": "forge-stale",
}
CUT_DIRECTIONS = {"both": 0, "out": 1, "in": 2}
NODES = {"A": 0, "B": 1}


def load_json(path):
    """Read a JSON file and return its content."""
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def load_manifest(path=MANIFEST_PATH):
    """Load the frozen game of scenarios."""
    manifest = load_json(path)
    if "scenarios" not in manifest:
        raise SystemExit("manifest without scenarios: %s" % path)
    return manifest


def resolve_entry(raw, index):
    """Turn one manifest entry into a complete, frozen scenario.

    An entry either names a published scenario file (scenarios/*.json) or
    carries every parameter itself. The file provides the defaults, the
    manifest entry overrides them: the frozen game is the manifest, the
    published scenarios are only its starting point. The resolved entry is
    what both the capture and the verification replay, byte for byte.
    """
    values = {"ticks": 1500,
              "seed": 1,
              "params": {"T": 5, "p": 1, "N": 8},
              "trajectory": {"kind": "slalom", "amp": 0.6, "freq": 0.5},
              "faults": [],
              "level": "complet",
              "loss": 0.0,
              "delay": 0}
    source = "golden"
    if "file" in raw:
        published = load_json(os.path.join(ROOT, raw["file"]))
        values.update({
            "ticks": int(published.get("ticks", values["ticks"])),
            "seed": int(published.get("seed", values["seed"])),
            "params": dict(published.get("params", values["params"])),
            "trajectory": dict(published.get("trajectory", values["trajectory"])),
            "faults": list(published.get("faults", values["faults"])),
        })
        source = raw["file"]
    for key in ("ticks", "seed", "params", "trajectory", "faults", "level",
                "loss", "delay"):
        if key in raw:
            values[key] = raw[key]
    entry = {
        "name": raw.get("name", "scenario-%03d" % index),
        "level": str(values["level"]),
        "ticks": int(values["ticks"]),
        "seed": int(values["seed"]),
        "params": dict(values["params"]),
        "trajectory": dict(values["trajectory"]),
        "faults": list(values["faults"]),
        "loss": float(values["loss"]),
        "delay": int(values["delay"]),
        "source": source,
    }
    entry["id"] = index
    entry["specs"] = [fault_spec(fault) for fault in entry["faults"]]
    return entry


def resolve_game(manifest):
    """Resolve the whole manifest into the frozen list of scenarios."""
    return [resolve_entry(raw, index)
            for index, raw in enumerate(manifest["scenarios"])]


def fault_spec(fault):
    """Translate one scenario fault into the simulator's at:kind:node:arg:value.

    INJ-01..INJ-10: the faults of the reference model of the spec 3.3. The
    published scenario files speak the vocabulary of the bench (targets "A"
    or "B", a link name, a duration in ticks); the simulator speaks the
    vocabulary of the fault model.
    """
    kind = str(fault.get("type", "death"))
    if kind == "cut":
        link = str(fault.get("link", ""))
        kind = "cut-peer" if link == "A-B" else "cut-actuator"
        arg = CUT_DIRECTIONS.get(str(fault.get("direction", "both")), 0)
    elif kind in FAULT_KINDS:
        arg = int(fault.get("ticks", fault.get("every", 0)) or 0)
    else:
        raise SystemExit("unknown fault type: %s" % kind)
    node = NODES.get(str(fault.get("target", "A")), 0)
    value = fault.get("value", 0)
    if isinstance(value, float):
        value = "%g" % value
    return "%d:%s:%d:%d:%s" % (int(fault.get("at_tick", 0)), FAULT_KINDS.get(kind, kind),
                              node, arg, value)


def build_command(entry, journal_path):
    """The exact command line replayed for one scenario."""
    params = ",".join("%s=%s" % (key, entry["params"][key])
                       for key in sorted(entry["params"]))
    trajectory = " ".join("%s=%s" % (key, entry["trajectory"][key])
                          for key in sorted(entry["trajectory"]))
    cmd = [SIMULATOR,
           "--ticks", str(entry["ticks"]),
           "--seed", str(entry["seed"]),
           "--params", params,
           "--trajectory", trajectory,
           "--level", entry["level"],
           "--scenario", entry["name"],
           "--id", str(entry["id"]),
           "--journal", journal_path]
    for spec in entry["specs"]:
        cmd += ["--fault", spec]
    if entry["loss"] > 0.0:
        cmd += ["--loss", "%g" % entry["loss"]]
    if entry["delay"] > 0:
        cmd += ["--delay", str(entry["delay"])]
    return cmd


def run_entry(entry, workdir):
    """Play one scenario and return its journal, its report and its status."""
    journal_path = os.path.join(workdir, "journal.jsonl")
    env = dict(os.environ)
    env["LC_ALL"] = "C"          # the journal formats numbers in the C locale
    env["TZ"] = "UTC"            # no wall clock in the output, but be explicit
    proc = subprocess.run(build_command(entry, journal_path), cwd=ROOT, env=env,
                          capture_output=True, text=True, timeout=600)
    with open(journal_path, encoding="utf-8") as handle:
        journal = handle.read()
    return {"journal": journal, "stdout": proc.stdout,
            "stderr": proc.stderr, "exit": proc.returncode}


def normalize(text):
    """Neutralise the volatile fields and the line endings.

    Returns the normalised text and the number of fields normalised per
    rule. Nothing else is touched: the floats formatted by the core, the
    names and the tick numbers stay exactly as the core wrote them.
    """
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    counts = {}
    lines = []
    for line in text.split("\n"):
        line = line.strip()
        if not line:
            continue
        for name, pattern, replacement, _why in VOLATILE_RULES:
            line, hits = pattern.subn(replacement, line)
            if hits:
                counts[name] = counts.get(name, 0) + hits
        lines.append(line)
    return "\n".join(lines) + ("\n" if lines else ""), counts


def sha256_text(text):
    """SHA-256 of a text, as the trace of a normalised journal."""
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def line_digests(text):
    """A short digest per journal line, in order.

    The SHA-256 of the whole journal is the guard; this chain localises the
    first divergent line (and therefore its tick) when the guard bites.
    """
    digests = []
    for line in text.split("\n"):
        if not line:
            continue
        digests.append(hashlib.sha256(line.encode("utf-8")).hexdigest()[:8])
    return "".join(digests)


def journal_ticks(text):
    """The tick carried by each journal line (-1 for the header)."""
    ticks = []
    for line in text.split("\n"):
        if not line:
            continue
        match = re.search(r'"tick":(\d+)', line)
        ticks.append(int(match.group(1)) if match else -1)
    return ticks


def fingerprint(entry, result):
    """The trace of one scenario: hashes, counters and the localisation chain."""
    journal, counts = normalize(result["journal"])
    stdout, _stdout_counts = normalize(result["stdout"])
    ticks = journal_ticks(journal)
    return {
        "name": entry["name"],
        "source": entry["source"],
        "level": entry["level"],
        "seed": entry["seed"],
        "ticks": entry["ticks"],
        "faults": entry["specs"],
        "exit": result["exit"],
        "journal_sha256": sha256_text(journal),
        "report_sha256": sha256_text(stdout),
        "journal_lines": len(ticks),
        "journal_ticks": len([t for t in ticks if t >= 0]),
        "normalized_fields": counts,
        "line_digests": line_digests(journal),
    }


def capture_game(game):
    """Play the whole frozen game.

    Returns one record per scenario: the entry that was replayed, its trace
    and its normalised journal (the journal localises a divergence, it is not
    stored in the reference file).
    """
    records = []
    with tempfile.TemporaryDirectory(prefix="relais-golden-") as workdir:
        for entry in game:
            result = run_entry(entry, workdir)
            trace = fingerprint(entry, result)
            journal, _counts = normalize(result["journal"])
            records.append({"entry": entry, "trace": trace, "journal": journal,
                            "report": result["stdout"]})
    return records


def write_traces(traces, path=TRACES_PATH):
    """Write the traces file (the versioned reference)."""
    payload = {
        "version": 1,
        "tool": "tools/golden/capture.py",
        "normalization": [{"field": name, "rule": why}
                          for name, _pattern, _replacement, why in VOLATILE_RULES],
        "scenarios": traces,
    }
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=1, sort_keys=False)
        handle.write("\n")


def read_traces(path=TRACES_PATH):
    """Read the traces file, the versioned reference."""
    if not os.path.exists(path):
        raise SystemExit("missing %s: run `make golden` first" % path)
    return load_json(path)
