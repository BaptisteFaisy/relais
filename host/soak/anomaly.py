#!/usr/bin/env python3
"""Relais -- the anomaly hunter of the long soak (mission M42, idea 15).

This module owns the rulebook: every rule has an identifier, a threshold and
the reason why that threshold is the right one. It never hides anything: as
soon as a rule fires the pilot freezes the scene and publishes the context.

Layers of judgement, from the strongest to the weakest:
  - the C core, which checks the invariants inside the running simulator;
  - the offline C verifier (OBS-07), which re-checks the published journal;
  - the Python mirror (host/report/analysis.py), which re-checks the same
    invariants from the journal alone;
  - the structural rules of this module, on the journal as written.

Requirement IDs: OBS-07 (offline invariants), PERF-05 (long runs), ROB-06
(replayable campaigns), INJ-08 (fault sweeps), ACT-01 (one value per tick),
MAN-04 (revocation on the actuator's own evidence), SEC-04 (forged orders),
ACT-09 (stale orders), DET-03/DET-09 (defect must be unmasked).
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from typing import Any, Dict, List, Optional, Sequence, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
_SHARED = os.path.join(_ROOT, "host", "report")
if _SHARED not in sys.path:
    sys.path.append(_SHARED)

import analysis    # noqa: E402  (repo-local module, path set above)
import journal_io  # noqa: E402
import stats       # noqa: E402

RULES_VERSION = "1.0.0"

RULES: List[Dict[str, str]] = [
    {
        "id": "A01",
        "title": "invariant violation reported by the core",
        "threshold": "the simulator exits 0 and reports violations=0",
        "why": "the core checks I1..I11 while it runs; any breach of the "
               "safety invariants is a defect (SPEC 7.2, DET-03, DET-09)",
    },
    {
        "id": "A02",
        "title": "invariant violation reported by the offline verifier",
        "threshold": "relais-verify exits 0 on the published journal",
        "why": "the journal is the authority a third party can replay; the "
               "verifier must judge it exactly like the run (OBS-07)",
    },
    {
        "id": "A03",
        "title": "invariant violation reported by the python mirror",
        "threshold": "0 violations after the profile preconditions are applied",
        "why": "a third reading of the same invariants, from the journal "
               "alone; it catches a journal that does not tell the same "
               "story as the run (OBS-07)",
    },
    {
        "id": "A04",
        "title": "two values applied on one tick",
        "threshold": "0 duplicated tick numbers in the journal",
        "why": "the actuator applies exactly ONE value per tick (ACT-01); a "
               "repeated tick number means two commands for one period",
    },
    {
        "id": "A05",
        "title": "stale order accepted",
        "threshold": "the mandate number never decreases",
        "why": "a mandate is a monotonic number; an order carrying an older "
               "mandate is stale and must be dropped (MAN-04, ACT-09)",
    },
    {
        "id": "A06",
        "title": "tick without a fresh order outside a handover",
        "threshold": "misses <= p+1 while a mandate is live",
        "why": "the actuator revokes on its own evidence after p+1 ticks "
               "without a valid order (MAN-04); more misses than that means "
               "it kept a mandate it had already proved empty",
    },
    {
        "id": "A07",
        "title": "fresh order counted as missing",
        "threshold": "misses == 0 on every tick whose source is the holder",
        "why": "a tick served by the holder's own fresh order cannot also be "
               "a missed tick; the record would be self-contradictory",
    },
    {
        "id": "A08",
        "title": "non-monotonic timestamp",
        "threshold": "tick numbers strictly increase, ns never decreases",
        "why": "the journal is an ordered trace of time; a stamp that goes "
               "backwards breaks every replay and every latency measurement",
    },
    {
        "id": "A09",
        "title": "latency drift above the reference p99.9",
        "threshold": "cpu microseconds per 1000 ticks <= p99.9(reference) * 4",
        "why": "the calibration phase gives the reference; the observed "
               "spread of the metric is under 20 percent, so a factor of 4 "
               "leaves headroom for a busy machine while a real drift (a "
               "leak, a slower loop) crosses any fixed factor (PERF-05)",
    },
    {
        "id": "A10",
        "title": "monotonic growth of the process memory",
        "threshold": "0 runs in a row whose peak RSS grows by >= 1024 kB",
        "why": "core/ never allocates; the measured peak RSS is constant, so "
               "a sustained rise of one megabyte cannot be jitter",
    },
    {
        "id": "A11",
        "title": "broken journal fingerprint",
        "threshold": "the digest chain of the log and the journal sha256 "
                     "match",
        "why": "the soak log is chained by sha256; a broken chain means the "
               "evidence was altered, truncated or reordered (OBS-07)",
    },
    {
        "id": "A12",
        "title": "unreadable journal line",
        "threshold": "0 malformed lines in the journal",
        "why": "a journal that cannot be parsed cannot be replayed; the "
               "verifier would silently check less than the run did",
    },
]

RULE_BY_ID: Dict[str, Dict[str, str]] = {r["id"]: r for r in RULES}


def rulebook() -> List[Dict[str, str]]:
    """Return the versioned rulebook (a copy, so callers cannot mutate it)."""
    return [dict(rule) for rule in RULES]


def finding(rule_id: str, **context: Any) -> Dict[str, Any]:
    """Build one finding: the rule, its threshold and the full context."""
    rule = RULE_BY_ID[rule_id]
    return {
        "rule": rule_id,
        "title": rule["title"],
        "threshold": rule["threshold"],
        "severity": "fatal",
        "context": context,
    }


def sha256_file(path: str) -> str:
    """Return the sha256 of a file, read in 1 MiB blocks."""
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def chain_digest(previous: str, payload: Dict[str, Any]) -> str:
    """Chain one record: sha256 over the previous digest and the payload."""
    blob = json.dumps(payload, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False)
    return hashlib.sha256((previous + blob).encode("utf-8")).hexdigest()


def _check_sequence(journal: journal_io.Journal, out: List[Dict[str, Any]]) -> None:
    """A04 and A08: the tick sequence of the journal must be strict."""
    seen: Dict[int, int] = {}
    last_tick = 0
    last_ns = 0
    for t in journal.ticks:
        if t.tick in seen:
            out.append(finding("A04", tick=t.tick, occurrences=seen[t.tick] + 1,
                               source=t.source, holder=t.holder,
                               detail="two tick records carry the same tick number"))
        else:
            seen[t.tick] = 1
        if t.tick <= last_tick:
            out.append(finding("A08", tick=t.tick, previous_tick=last_tick,
                               detail="tick numbers do not strictly increase"))
        last_tick = t.tick
        if t.ns and last_ns and t.ns < last_ns:
            out.append(finding("A08", tick=t.tick, ns=t.ns, previous_ns=last_ns,
                               detail="host timestamp goes backwards"))
        last_ns = max(last_ns, t.ns)


def _check_mandate(journal: journal_io.Journal, out: List[Dict[str, Any]]) -> None:
    """A05: the mandate number never decreases."""
    highest = 0
    for t in journal.ticks:
        if t.mandate < highest:
            out.append(finding("A05", tick=t.tick, mandate=t.mandate,
                               highest_mandate=highest, source=t.source,
                               holder=t.holder,
                               detail="a tick carries a mandate older than the one in force"))
        highest = max(highest, t.mandate)


def _check_freshness(journal: journal_io.Journal,
                     out: List[Dict[str, Any]]) -> None:
    """A06 and A07: the miss counter and the source must agree."""
    params = journal.header.params if journal.header else {}
    patience = int(params.get("p", 1))
    for t in journal.ticks:
        if t.holder and t.mandate > 0 and t.misses > patience + 1:
            out.append(finding("A06", tick=t.tick, misses=t.misses, patience=patience,
                               holder=t.holder, mandate=t.mandate, source=t.source,
                               cause=t.cause,
                               detail="the holder stayed silent past p+1 ticks "
                                      "without the mandate being revoked"))
        if t.source == "holder" and t.misses != 0:
            out.append(finding("A07", tick=t.tick, misses=t.misses,
                               holder=t.holder, mandate=t.mandate,
                               detail="a tick served by the holder is counted as missed"))


def _mirror_excluded(profile: Dict[str, Any]) -> List[str]:
    """Invariants whose preconditions the profile does not guarantee."""
    excluded: List[str] = []
    if profile.get("lossy") or profile.get("fault_count", 0) != 1:
        # I4 (the zero-gap bound) is claimed inside the timing hypothesis on
        # a lossless channel and under the single-fault rule (SPEC 7.3,
        # ROB-03); the core relaxes it when frames are lost or when two
        # faults overlap, and the pilot cannot prove those preconditions.
        excluded.append("I4")
    return excluded


def _check_mirror(journal: journal_io.Journal, profile: Dict[str, Any],
                  level: str, path: str,
                  out: List[Dict[str, Any]]) -> None:
    """A03: the python mirror of the invariants, from the journal alone."""
    run = journal_io.Run(name=os.path.basename(path), path=path, journal=journal,
                         scenario={"faults": profile.get("faults", [])})
    result = analysis.analyze_run(run, level)
    excluded = _mirror_excluded(profile)
    kept = [v for v in result.violations if v.inv not in excluded]
    if not kept:
        return
    out.append(finding("A03", level=level, excluded=excluded,
                       violations=[{"inv": v.inv, "tick": v.tick, "detail": v.detail}
                                   for v in kept[:5]],
                       violation_count=len(kept)))


def scan_journal(path: str, profile: Dict[str, Any],
                 level: str) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    """Apply every structural rule to one journal; returns (findings, stats)."""
    out: List[Dict[str, Any]] = []
    journal = journal_io.read_journal(path)
    _check_sequence(journal, out)
    _check_mandate(journal, out)
    _check_freshness(journal, out)
    if journal.malformed:
        out.append(finding("A12", malformed=journal.malformed,
                           samples=journal.malformed_samples[:3],
                           detail="the journal holds lines the reader cannot parse"))
    _check_mirror(journal, profile, level, path, out)
    stats = {
        "ticks": len(journal.ticks),
        "events": len(journal.events),
        "malformed": journal.malformed,
        "handovers": sum(1 for e in journal.events
                         if e.event in ("mandate_granted", "mandate_auto_switch")),
        "mandates": sum(1 for e in journal.events if e.event == "mandate_granted"),
        "revokes": sum(1 for e in journal.events if e.event == "mandate_revoked"),
        "max_misses": max((t.misses for t in journal.ticks), default=0),
    }
    return out, stats


def verify_journal(path: str, profile: Dict[str, Any],
                   level: str) -> Optional[Dict[str, Any]]:
    """A02: re-check the published journal with the offline C verifier."""
    if not profile.get("verify", True):
        return None
    argv = [profile["verify_bin"], path, "--level", level]
    if profile.get("fault_free"):
        argv.append("--fault-free")
    if profile.get("double_fault"):
        argv.append("--double-fault")
    proc = subprocess.run(argv, capture_output=True, text=True, timeout=300)
    if proc.returncode == 0:
        return None
    return finding("A02", argv=argv, returncode=proc.returncode,
                   output=(proc.stdout + proc.stderr)[-400:])


def check_latency(metric: float, reference: float, factor: float) -> Optional[Dict[str, Any]]:
    """A09: the run latency must stay under the reference p99.9 times the factor."""
    if reference <= 0.0:
        return None
    threshold = reference * factor
    if metric <= threshold:
        return None
    return finding("A09", metric=metric, reference=reference, factor=factor,
                   threshold=threshold, ratio=metric / reference,
                   detail="the run latency drifted above the reference p99.9")


def check_rss(series: Sequence[int], window: int,
              min_growth_kb: int) -> Optional[Dict[str, Any]]:
    """A10: the peak RSS must not grow monotonically over the last runs."""
    if len(series) < window or window < 2:
        return None
    tail = list(series)[-window:]
    if any(tail[i + 1] <= tail[i] for i in range(len(tail) - 1)):
        return None
    growth = tail[-1] - tail[0]
    if growth < min_growth_kb:
        return None
    return finding("A10", window=window, first_kb=tail[0], last_kb=tail[-1],
                   growth_kb=growth, series=tail,
                   detail="the peak resident memory grew on every one of the "
                          "last runs")


def verify_digest_chain(path: str) -> Optional[Dict[str, Any]]:
    """A11: the soak log must be a unbroken sha256 chain."""
    previous = ""
    expected_seq = 0
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            payload = {k: v for k, v in record.items() if k != "digest"}
            recomputed = chain_digest(previous, payload)
            if record.get("digest") != recomputed or record.get("seq") != expected_seq:
                return finding("A11", line=lineno, seq=record.get("seq"),
                               expected_seq=expected_seq,
                               stored=record.get("digest"), recomputed=recomputed,
                               detail="the digest chain of the soak log is broken")
            previous = recomputed
            expected_seq += 1
    return None


def latency_reference(samples: Sequence[float]) -> float:
    """Return the p99.9 of the calibration samples (0.0 when there is none)."""
    return stats.percentile(list(samples), 99.9)


def rule_table_text() -> str:
    """Return the rulebook as plain text, for the report and the docs."""
    lines = ["Relais soak rulebook, version %s" % RULES_VERSION]
    for rule in RULES:
        lines.append("%s  %s" % (rule["id"], rule["title"]))
        lines.append("    threshold: %s" % rule["threshold"])
        lines.append("    why: %s" % rule["why"])
    return "\n".join(lines)
