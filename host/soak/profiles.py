#!/usr/bin/env python3
"""Relais -- the fault profiles of the long soak (mission M42).

One profile per family of the fault model (spec 3.3). A profile says what the
bench is asked to do, which levels its published claims stay inside, and what
the hunter may assume about the run. The pilot rotates over the catalogue.

Requirement IDs: INJ-08 (fault sweep), ROB-03 (single-fault hypothesis),
SEC-04 (forged orders), ACT-09 (stale orders), REL-09/REL-10 (handover and
rolling update), DEG-03 (a machine that comes back).
"""
from __future__ import annotations

import os
from typing import Any, Dict, List

VERIFY = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "build", "relais-verify")

# "levels" lists the levels whose published claims the profile stays inside.
PROFILES: List[Dict[str, Any]] = [
    {"name": "nominal", "tpl": [], "levels": ("socle", "complet")},
    {"name": "death", "tpl": ["--fault", "{at}:death:{node}"],
     "levels": ("socle", "complet")},
    {"name": "freeze", "tpl": ["--fault", "{at}:freeze:{node}:{dur}"],
     "levels": ("socle", "complet")},
    {"name": "cutact", "tpl": ["--fault", "{at}:cut-actuator:{node}:0",
                               "--fault", "{at2}:restore:{node}"],
     "levels": ("socle", "complet")},
    {"name": "cutpeer", "tpl": ["--fault", "{at}:cut-peer:{node}:0",
                                "--fault", "{at2}:restore:{node}"],
     "levels": ("socle", "complet")},
    {"name": "slow", "tpl": ["--fault", "{at}:slow:{node}:3"],
     "levels": ("socle", "complet")},
    {"name": "cession", "tpl": ["--fault", "{at}:handover:{node}"],
     "levels": ("socle", "complet")},
    {"name": "update", "tpl": ["--fault", "{at}:update:{node}:{dur}"],
     "levels": ("socle", "complet")},
    {"name": "crash", "tpl": ["--fault", "{at}:crash:{node}:20"],
     "levels": ("socle", "complet")},
    # A restart wipes the control state of a machine, so at level C the value
    # it then computes legitimately differs from the twin: I5 assumes a
    # control state that persists. The profile therefore stays at level S.
    {"name": "restart", "tpl": ["--fault", "{at}:restart:{node}"],
     "levels": ("socle",)},
    {"name": "forge", "tpl": ["--fault", "{at}:forge:{node}"],
     "levels": ("socle", "complet")},
    {"name": "forgestale", "tpl": ["--fault", "{at}:forge-stale:{node}"],
     "levels": ("socle", "complet")},
    {"name": "bias", "tpl": ["--fault", "{at}:bias:{node}:0:0.25"],
     "levels": ("socle", "complet")},
    {"name": "lossy", "tpl": ["--loss", "0.01"], "levels": ("socle", "complet")},
    {"name": "auto", "tpl": ["--auto-faults", "1"], "levels": ("socle", "complet")},
]


def build_run(profile: Dict[str, Any], seed: int, ticks: int, level: str,
              out: str, index: int, sim: str) -> Dict[str, Any]:
    """Build the command line and the description of one run."""
    at = ticks // 4 + (seed * 7919) % max(1, ticks // 2)
    at2 = min(at + 200 + (seed % 800), max(1, ticks - 10))
    fields = {"at": at, "at2": at2, "node": seed % 2, "dur": 20 + (seed % 180)}
    argv = [sim, "--ticks", str(ticks), "--seed", str(seed)]
    faults: List[Dict[str, Any]] = []
    for item in profile["tpl"]:
        text = item.format(**fields) if "{" in item else item
        argv.append(text)
        if ":" in text and not text.startswith("--"):
            parts = text.split(":")
            faults.append({"type": parts[1], "at_tick": int(parts[0]),
                           "target": parts[2]})
    if profile["name"] == "auto":
        faults = [{"type": "auto", "at_tick": at, "target": str(seed % 2)}]
    argv += ["--level", level]
    journal = os.path.join(out, "journal-%05d.jsonl" % index)
    argv += ["--journal", journal]
    return {
        "index": index, "profile": profile, "level": level, "seed": seed,
        "ticks": ticks, "argv": argv, "journal_path": journal,
        "stdout_path": os.path.join(out, "sim-%05d.out" % index),
        "flags": flags_for(profile, faults, level),
    }


def flags_for(profile: Dict[str, Any], faults: List[Dict[str, Any]],
              level: str) -> Dict[str, Any]:
    """What the hunter may assume about one run."""
    name = profile["name"]
    return {
        "faults": faults,
        "fault_count": len(faults),
        "lossy": name == "lossy",
        "bias": name == "bias",
        "fault_free": not faults and name != "lossy",
        "double_fault": name == "auto",
        # The offline verifier cannot express a lossy channel nor a wrong
        # value: I4 and I5 are relaxed by the core in those cases (SPEC 7.3).
        "verify": not (name == "lossy" or (name == "bias" and level == "complet")),
        "verify_bin": VERIFY,
    }


def profile_context(spec: Dict[str, Any]) -> Dict[str, Any]:
    """Return what the hunter needs to know about one run."""
    context = dict(spec["flags"])
    context["name"] = spec["profile"]["name"]
    return context
