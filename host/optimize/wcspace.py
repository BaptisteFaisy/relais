#!/usr/bin/env python3
"""Relais — the fault space the worst-case adversary searches (INJ-12).

One genome is GENE_COUNT real genes inside [0, 1]: five per fault (instant,
kind, target machine, duration, link direction) plus the presence of the second
fault, so a genome always decodes into one or two nested faults of the
simulator's own vocabulary (host/simulate/sim_faults.c). The same object is
rendered two ways: the `at:kind:node:arg` string the simulator takes, and the
scenario object frozen by CONTRACT.md that the bench replays.

Keeping the encoding here, and nowhere else, is what makes the search testable:
host/optimize/test_worstcase.py checks the decoding without running a single
simulation.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Sequence, Tuple

# The fault kinds the adversary may pick (spec 3.3 F1..F8, INJ-01..INJ-09).
KINDS = ("death", "freeze", "cut-actuator", "cut-peer", "slow", "crash")
DIRECTIONS = ("both", "forward", "reverse")
NODES = ("A", "B")
CRASH_PROCESS = "relais-core"  # CONTRACT.md: crash names relais-net|core|ctl
AT_LO, AT_HI = 0.02, 0.95      # the fault instant, as a fraction of the run
GENES_PER_FAULT = 5
GENE_COUNT = 2 * GENES_PER_FAULT + 1
DEFAULT_TRAJECTORY = {"kind": "slalom", "amp": 0.6, "freq": 0.5}
DEFAULT_PARAMS = {"T": 5, "p": 1, "N": 8}


def quantise(gene: float, count: int) -> int:
    """Map a normalised gene onto one of `count` categories."""
    return max(0, min(count - 1, int(gene * count)))


def duration_arg(kind: str, gene: float, ticks: int) -> int:
    """Decode the duration gene; its meaning depends on the fault kind."""
    if kind == "freeze":
        return 1 + int(gene * max(1, ticks // 4))   # frozen for that many ticks
    if kind == "slow":
        return 2 + int(gene * 18)                   # one tick out of arg is lost
    if kind == "crash":
        return 1 + int(gene * 39)                   # supervisor restart, in ticks
    return 0                                        # death and cuts carry none


@dataclass(frozen=True)
class Fault:
    """One injected fault."""

    at: int
    kind: str
    node: int
    arg: int
    direction: int = 0

    @property
    def target(self) -> str:
        return NODES[self.node]

    @property
    def cli_arg(self) -> int:
        """sim_faults.c cut_link(): towards the actuator the argument IS the
        direction (0 both, 1 outgoing only, 2 incoming only)."""
        return self.direction if self.kind == "cut-actuator" else self.arg

    def window(self, ticks: int) -> int:
        """How long the fault stays active, in ticks (nested-fault analysis)."""
        if self.kind in ("freeze", "crash"):
            return self.arg
        return max(1, ticks - self.at)

    def to_cli(self) -> str:
        """The simulator's fault spec: at:kind:node[:arg[:value]]."""
        return "%d:%s:%d:%d" % (self.at, self.kind, self.node, self.cli_arg)

    def to_scenario(self, ticks: int) -> Dict[str, Any]:
        """The CONTRACT.md scenario fault object, replayable by the bench."""
        out: Dict[str, Any] = {"at_tick": self.at, "target": self.target}
        if self.kind in ("cut-actuator", "cut-peer"):
            link = "A-B" if self.kind == "cut-peer" else "%s-actuator" % self.target
            out.update({"type": "cut", "link": link,
                        "direction": DIRECTIONS[self.direction],
                        "ticks": self.window(ticks)})
            return out
        out["type"] = self.kind
        if self.kind == "freeze":
            out["ticks"] = self.arg
        elif self.kind == "crash":
            out.update({"crash": CRASH_PROCESS, "ticks": self.arg})
        elif self.kind == "slow":
            window = self.window(ticks)
            out.update({"every": self.arg, "gap_ticks": self.arg,
                        "count": max(1, window // max(1, self.arg)),
                        "ticks": window})
        return out

    def label(self) -> str:
        """A short human label, used by the reports."""
        extra = ""
        if self.kind in ("freeze", "crash"):
            extra = " %dt" % self.arg
        elif self.kind == "slow":
            extra = " 1/%d" % self.arg
        elif self.kind.startswith("cut"):
            extra = " %s" % DIRECTIONS[self.direction]
        return "%s%s@%d on %s" % (self.kind, extra, self.at, self.target)


def decode_fault(gene: Sequence[float], ticks: int) -> Fault:
    """Genes [instant, kind, target, duration, direction] -> one Fault."""
    at = int(round((AT_LO + gene[0] * (AT_HI - AT_LO)) * ticks))
    kind = KINDS[quantise(gene[1], len(KINDS))]
    return Fault(at=max(1, min(ticks, at)), kind=kind, node=quantise(gene[2], 2),
                 arg=duration_arg(kind, gene[3], ticks),
                 direction=quantise(gene[4], len(DIRECTIONS)))


@dataclass(frozen=True)
class Scenario:
    """A whole run: the faults and everything needed to replay it."""

    faults: Tuple[Fault, ...]
    ticks: int
    seed: int
    level: str
    params: str = ""
    trajectory: str = ""
    name: str = "worstcase"

    def cli(self) -> List[str]:
        """The relais-simulate arguments of this scenario (journals excluded)."""
        args = ["--ticks", str(self.ticks), "--seed", str(self.seed),
                "--scenario", self.name, "--id", "0"]
        if self.level == "complet":
            args += ["--level", "complet"]
        if self.params:
            args += ["--params", self.params]
        if self.trajectory:
            args += ["--trajectory", self.trajectory]
        for fault in self.faults:
            args += ["--fault", fault.to_cli()]
        return args

    def to_dict(self, scenario_id: int = 0) -> Dict[str, Any]:
        """The frozen scenario format of CONTRACT.md (scenarios/*.json)."""
        return {
            "id": scenario_id, "name": self.name, "ticks": self.ticks,
            "seed": self.seed, "level": self.level,
            "trajectory": spec_to_dict(self.trajectory, DEFAULT_TRAJECTORY),
            "params": spec_to_dict(self.params, DEFAULT_PARAMS),
            "faults": [f.to_scenario(self.ticks) for f in self.faults],
        }

    def label(self) -> str:
        return "%s: %s" % (self.level, " + ".join(f.label() for f in self.faults))

    def kinds(self) -> Tuple[str, ...]:
        """Fault kinds in order, deduplicated: the per-kind table keys on it."""
        return tuple(dict.fromkeys(f.kind for f in self.faults))


def spec_to_dict(spec: str, defaults: Dict[str, Any]) -> Dict[str, Any]:
    """Turn a --params/--trajectory spec string into a scenario JSON object."""
    out = dict(defaults)
    for chunk in spec.replace(",", " ").split() if spec else []:
        if "=" in chunk:
            key, value = chunk.split("=", 1)
            out[key.strip()] = value.strip()
    return out


def full_scale(params: str) -> float:
    """Full scale of the command: 2 x max_amplitude (ACT-05, RELAIS_FULL_SCALE)."""
    for chunk in params.replace(" ", ",").split(","):
        if chunk.startswith("amp="):
            try:
                return 2.0 * abs(float(chunk[4:]))
            except ValueError:
                return 2.0
    return 2.0


def decode(genome: Sequence[float], ticks: int, seed: int, level: str,
           params: str = "", trajectory: str = "") -> Scenario:
    """The whole genome -> the scenario to simulate (one or two nested faults)."""
    faults = [decode_fault(genome[0:GENES_PER_FAULT], ticks)]
    if genome[GENE_COUNT - 1] >= 0.5:
        base = GENES_PER_FAULT
        faults.append(decode_fault(genome[base:base + GENES_PER_FAULT], ticks))
    faults.sort(key=lambda f: f.at)
    return Scenario(tuple(faults), ticks, seed, level, params, trajectory)


def archetypes() -> List[Tuple[float, ...]]:
    """Warm start: one genome per fault kind, nested with the same kind on the
    other machine, so the first generation is never worse than a regular sweep."""
    out: List[Tuple[float, ...]] = []
    for index, _ in enumerate(KINDS):
        gene = [0.0] * GENE_COUNT
        gene[0] = 0.30                       # instant: 30 % into the run
        gene[1] = (index + 0.5) / len(KINDS)  # this kind
        gene[2] = 0.25                       # machine A
        gene[3] = 0.50                       # medium duration
        gene[4] = 0.10                       # both directions
        gene[GENES_PER_FAULT + 0] = 0.34     # second fault, a little later
        gene[GENES_PER_FAULT + 1] = (index + 0.5) / len(KINDS)
        gene[GENES_PER_FAULT + 2] = 0.75     # machine B
        gene[GENES_PER_FAULT + 3] = 0.50
        gene[GENE_COUNT - 1] = 0.99          # nested: yes
        out.append(tuple(gene))
    return out
