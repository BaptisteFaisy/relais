"""Run analysis for the Relais campaign report (OBS-05, OBS-07).

Python mirror of the invariant subset checked offline by
host/verify/relais-verify.c (I1, I2, I3, I4, I5, I6, I8, I10, I11). The C
verifier stays the authority; this module reproduces its judgement so the
HTML report can count violations, measure the gap between fresh orders and
rank the worst cases.
"""
from __future__ import annotations

from bisect import bisect_left
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

from journal_io import (DEFAULT_PARAMS, FRESH_SOURCES, MANDATE_EVENTS,
                        Run, Tick, twin_values)


@dataclass
class Violation:
    """One invariant breach: id (I1..I11), tick, human detail."""
    inv: str
    tick: int
    detail: str


@dataclass
class Handover:
    """A mandate handover event with the delay since the last fresh order."""
    tick: int
    node: str
    mandate: int
    delay_ticks: int


class _Checker:
    """Incremental invariant checker; one instance per run."""

    def __init__(self, params: Dict[str, float], complet: bool, single_fault: bool):
        self.p = params
        self.complet = complet
        self.single_fault = single_fault
        self.violations: List[Violation] = []
        self.count = 0
        self.gap = 0
        self.last_fresh = 0
        self.last_mandate = -1
        self.last_holder = ""
        self.last_lamp = "off"
        self.lamp_delay = 0
        self.testament_run = 0
        self.in_fallback = False
        self.service_ticks = 0

    def bad(self, inv: str, tick: int, detail: str) -> None:
        """Record one violation."""
        self.violations.append(Violation(inv, tick, detail))

    def tick_checks(self, r: Tick, mandate_change_ticks: set) -> None:
        """Run the per-tick invariants I1, I2, I3, I10, I11 (mandate aware)."""
        self.count += 1
        if r.tick != self.count:
            self.bad("I11", r.tick, "tick numbering is not contiguous")
        if r.source == "none":
            self.bad("I1", r.tick, "no value applied at this tick")
        elif r.source == "holder" and not r.holder:
            self.bad("I1", r.tick, "order applied with no holder")
        elif r.source == "shadow" and not r.certified:
            self.bad("I1", r.tick, "substitution by a second that is not certified")
        if self.count > 1:
            if r.mandate < self.last_mandate:
                self.bad("I2", r.tick, "mandate went backwards")
            if (r.holder and self.last_holder and r.holder != self.last_holder
                    and r.mandate == self.last_mandate):
                self.bad("I2", r.tick, "two holders of the same mandate")
        if (self.count > 1 and r.source == "holder" and r.holder != self.last_holder
                and r.tick not in mandate_change_ticks):
            self.bad("I3", r.tick, "order applied from a machine that is not the holder")
        self._lamp(r)
        self.last_mandate = r.mandate
        self.last_holder = r.holder

    def _lamp(self, r: Tick) -> None:
        """I10: a loss of redundancy must show on the lamp within two ticks."""
        degraded = (r.source != "holder" or not r.holder
                    or (self.complet and not r.certified))
        if not degraded or r.lamp != "off":
            self.lamp_delay = 0
            self.last_lamp = r.lamp
            return
        self.lamp_delay += 1
        if self.lamp_delay > 2:
            self.bad("I10", r.tick, "redundancy lost and the lamp stayed off")

    def gap_checks(self, r: Tick) -> bool:
        """I4; returns True when the tick carries a fresh order."""
        fresh = r.source in FRESH_SOURCES
        if fresh:
            self.gap = 0
            self.last_fresh = r.tick
            self.service_ticks += 1
            return True
        if self.last_fresh == 0:
            return False
        self.gap += 1
        if self.complet and self.single_fault:
            self.bad("I4", r.tick, "a tick without a fresh order, second certified")
        elif self.gap > int(self.p.get("p", 1)) + 1:
            self.bad("I4", r.tick, "gap between fresh orders above p+1")
        return False

    def continuity(self, r: Tick, twin: Optional[float], max_dev: List[float]) -> None:
        """I5: deviation against the fault-free twin, enforced at level C."""
        if twin is None or self.last_fresh == 0 or r.source == "fallback":
            return
        if self.service_ticks < 10:
            return
        dev = abs(r.value - twin)
        if dev > max_dev[0]:
            max_dev[0] = dev
        if self.complet and self.single_fault and dev > float(self.p.get("delta", 0.02)):
            self.bad("I5", r.tick, "applied value too far from the twin")

    def horizon(self, r: Tick) -> None:
        """I6 and I8: the will stays inside its horizon, fallback is reached."""
        n = int(self.p.get("N", 8))
        if r.source == "testament":
            self.testament_run += 1
            if self.testament_run > n:
                self.bad("I6", r.tick, "will played past its horizon")
        else:
            self.testament_run = 0
        if r.source == "fallback":
            if not self.in_fallback:
                self.in_fallback = True
                if (self.last_fresh != 0
                        and r.tick - self.last_fresh > n + 1):
                    self.bad("I8", r.tick, "fallback reached too late after the last order")
        else:
            self.in_fallback = False


@dataclass
class RunAnalysis:
    """Everything the report needs about one run."""
    run: Run
    level: str
    period_ms: float
    ticks_total: int
    violations: List[Violation] = field(default_factory=list)
    max_gap: int = 0
    max_gap_tick: int = 0
    gaps: List[int] = field(default_factory=list)
    max_deviation: float = 0.0
    source_counts: Dict[str, int] = field(default_factory=dict)
    handovers: List[Handover] = field(default_factory=list)
    malformed: int = 0
    single_fault: bool = True

    @property
    def violation_count(self) -> int:
        """Total number of invariant breaches."""
        return len(self.violations)

    @property
    def gap_bound(self) -> int:
        """Allowed ticks without a fresh order: 0 at level C, p+1 at level S.
        A double fault relaxes the bound: the claim is the safe state, not the
        continuity of the command (spec 3.3, F9/F10), as in relais-verify."""
        if not self.single_fault:
            return 10 ** 9
        if self.level == "complet":
            return 0
        p = int(self.run.journal.header.params.get("p", 1)) if self.run.journal.header else 1
        return p + 1

    @property
    def max_interval_ticks(self) -> int:
        """Worst fresh-to-fresh interval, in ticks (0 when never stale)."""
        return self.max_gap + 1 if self.max_gap else 0

    @property
    def max_interval_ms(self) -> float:
        """Worst fresh-to-fresh interval, in milliseconds."""
        return self.max_interval_ticks * self.period_ms

    @property
    def max_handover_delay(self) -> int:
        """Worst handover delay (ticks between last fresh order and grant)."""
        return max((h.delay_ticks for h in self.handovers), default=0)

    @property
    def ok(self) -> bool:
        """True when the run passes: no violation, gap inside the bound."""
        return self.violation_count == 0 and self.max_gap <= self.gap_bound


def _gap_runs(ticks: Sequence[Tick]) -> Tuple[List[int], int, int]:
    """Lengths of the non-fresh runs after the first fresh order."""
    runs: List[int] = []
    streak = 0
    seen_fresh = False
    max_gap = 0
    max_at = 0
    for t in ticks:
        if t.source in FRESH_SOURCES:
            if seen_fresh and streak:
                runs.append(streak)
            streak = 0
            seen_fresh = True
            continue
        if seen_fresh:
            streak += 1
            if streak > max_gap:
                max_gap, max_at = streak, t.tick
    if seen_fresh and streak:
        runs.append(streak)
    return runs, max_gap, max_at


def _handovers(run: Run, ticks: Sequence[Tick]) -> List[Handover]:
    """Mandate grants/auto-switches, with delay since the last fresh order."""
    fresh_ticks = [t.tick for t in ticks if t.source in FRESH_SOURCES]
    out: List[Handover] = []
    for e in run.journal.events:
        if e.event not in ("mandate_granted", "mandate_auto_switch"):
            continue
        i = bisect_left(fresh_ticks, e.tick)
        delay = e.tick - fresh_ticks[i - 1] if i > 0 else 0
        out.append(Handover(e.tick, e.node, e.mandate, delay))
    return out


def analyze_run(run: Run, level: str = "socle") -> RunAnalysis:
    """Analyse one run against the mirrored invariant subset."""
    header = run.journal.header
    params = header.params if header else dict(DEFAULT_PARAMS)
    faults = (run.scenario or {}).get("faults") or []
    single_fault = len(faults) <= 1
    checker = _Checker(params, level == "complet", single_fault)
    mandate_ticks = {e.tick for e in run.journal.events
                     if e.event in MANDATE_EVENTS}
    twin = twin_values(run.twin)
    max_dev = [0.0]
    counts: Dict[str, int] = {}
    for r in run.journal.ticks:
        counts[r.source] = counts.get(r.source, 0) + 1
        checker.tick_checks(r, mandate_ticks)
        checker.gap_checks(r)
        checker.continuity(r, twin.get(r.tick), max_dev)
        checker.horizon(r)
    gaps, max_gap, max_at = _gap_runs(run.journal.ticks)
    return RunAnalysis(
        run=run, level=level, period_ms=float(params.get("T", 5)),
        ticks_total=len(run.journal.ticks), violations=checker.violations,
        max_gap=max_gap, max_gap_tick=max_at, gaps=gaps,
        max_deviation=max_dev[0], source_counts=counts,
        handovers=_handovers(run, run.journal.ticks), single_fault=single_fault,
        malformed=run.journal.malformed + (run.twin.malformed if run.twin else 0))


def analyze_campaign(runs: Sequence[Run], level: str = "socle") -> List[RunAnalysis]:
    """Analyse every run of a campaign, in discovery order."""
    return [analyze_run(r, level) for r in runs]
