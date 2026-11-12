#!/usr/bin/env python3
"""Relais -- generate the FMEA from the fault truth table (M16).

Reads the two tables already maintained in the repository:

  * docs/TRUTH-TABLE.md   -- the detailed fault cases (13 rows) and the
                             fault-model summary (F1..F11, spec 3.3);
  * the fault model of spec 3.3 is that same summary, so the script parses it
    once and reuses it (nothing is typed twice).

The script never hard-codes the failure modes, the causes, the effects, the
requirements or the coverage: those are parsed out of the markdown table. Each
FMEA entry is one fault code of the model (F1..F11); the detailed cases that
reference a code contribute its local effect, system effect, requirements and
coverage. A fault code with no detailed case (F11, "out of scope") becomes an
entry with no coverage and is flagged.

What the script *adds* is the AMDEC ratings -- severity, occurrence,
detectability and their product (criticality) -- which are engineering
judgement keyed by fault code, plus the mitigation measure keyed by fault code.
Both additions live in a single small table, clearly marked as judgement, not
as a measurement.

Output:
  * a Markdown FMEA table (to --out, or stdout),
  * a CSV version (to --csv, or next to --out),
  * on stderr, the faults of the model that carry NO mitigation measure and NO
    covering test.

Usage:
  python3 host/safety/fmea.py [--out fmea.md] [--csv fmea.csv]

Everything is deterministic: the same inputs produce the same bytes, so a PR
diff stays readable. Standard library only.
"""

from __future__ import annotations

import argparse
import csv
import io
import os
import sys

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
TRUTH_TABLE = os.path.join(REPO_ROOT, "docs", "TRUTH-TABLE.md")

# ---------------------------------------------------------------------------
# Deterministic, judgement-based AMDEC ratings, keyed by fault code (spec 3.3).
# These are engineering judgment for a demonstrator, NOT measurements. Severity
# S is the effect on safety if the barrier were absent; occurrence O is how
# likely the fault is before mitigation; detectability D is how likely the
# system is to detect it (1 = certain, 10 = undetectable). Criticality is the
# RPN S*O*D, on the usual 1..1000 scale.
# ---------------------------------------------------------------------------
RATINGS = {
    # code: (S, O, D, detection method, mitigation measure)
    "F1": (9, 4, 2,
           "p+1 consecutive ticks without a valid order, counted by the actuator",
           "revoke and auto-switch to the certified second, or grant on demande; "
           "handover within p+2 ticks (REL-01)"),
    "F2": (9, 4, 3,
           "stale orders are dropped by mandate number and tick label (MAN-08)",
           "handover; the revenant reads the STATUT, stops emitting, rejoins as second"),
    "F3": (8, 5, 4,
           "miss counter published in every STATUT (DET-03)",
           "deposed only when the misses are consecutive; intermittent misses absorbed"),
    "F4": (6, 4, 3,
           "loss of ETAT detected after two ticks (DET-06)",
           "no handover; the second rebuilds a bump-free state from the STATUT; "
           "yellow \"link lost\""),
    "F5": (8, 5, 2,
           "the actuator sees no order; the machine sees no STATUT (DET-07)",
           "handover if it was the holder; the machine deposes itself after "
           "bail_ticks of silence (MAN-09)"),
    "F6": (7, 5, 3,
           "tick label, mandate number and CRC reject a reordered or late frame",
           "dropped and counted; absorbed within the parameter bounds (ROB-02)"),
    "F7": (8, 3, 1,
           "CRC-32C (level S) or SipHash-2-4 MAC (level C) (DET-04)",
           "dropped, counted and journaled; no effect on the service (SEC-02)"),
    "F8": (10, 3, 2,
           "order/shadow comparison over 3 ticks (DET-09)",
           "disagreement alert, decertification, red lamp; holder kept "
           "(detected, not arbitrated: no majority with two machines)"),
    "F9": (10, 2, 7,
           "not detected directly; reaches the safe state",
           "will to its horizon, then the local fallback loop (safe state)"),
    "F10": (10, 1, 5,
           "not detected directly; reaches the safe state",
           "will for N ticks, then fallback; red lamp; stop requested (DEG-05)"),
    "F11": (10, 1, 9,
           "not detected (actuator is a declared single point of failure)",
           ""),  # out of scope: no mitigation, no test -> flagged.
}


def _strip(text: str) -> str:
    """Strip a markdown cell of its surrounding whitespace only."""
    return text.strip()


def _split_row(line: str) -> list[str]:
    """Split a markdown table row into cells, ignoring leading/trailing pipes."""
    if line.count("|") < 2:
        return []
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [_strip(c) for c in line.split("|")]


def _is_separator(line: str) -> bool:
    return line.replace("-", "").replace(":", "").replace("|", "").strip() == ""


def parse_detailed_cases(path: str) -> list[dict]:
    """Parse the 'Detailed cases' markdown table into a list of dicts."""
    rows: list[dict] = []
    header: list[str] | None = None
    with io.open(path, "r", encoding="utf-8") as fh:
        lines = fh.readlines()
    in_table = False
    for line in lines:
        if line.startswith("## Detailed cases"):
            in_table = True
            continue
        if in_table and line.startswith("## "):
            break
        if not in_table:
            continue
        if _is_separator(line):
            continue
        cells = _split_row(line)
        if not cells:
            continue
        if header is None:
            header = cells
            continue
        if len(cells) < len(header):
            continue
        rows.append(dict(zip(header, cells)))
    return rows


def parse_fault_model(path: str) -> dict[str, str]:
    """Parse the 'Fault-model summary' table: code -> fault description."""
    model: dict[str, str] = {}
    header: list[str] | None = None
    with io.open(path, "r", encoding="utf-8") as fh:
        lines = fh.readlines()
    in_table = False
    for line in lines:
        if line.startswith("## Fault-model summary"):
            in_table = True
            continue
        if in_table and line.startswith("## "):
            break
        if not in_table:
            continue
        if _is_separator(line):
            continue
        cells = _split_row(line)
        if not cells:
            continue
        if header is None:
            header = cells
            continue
        if len(cells) < 2:
            continue
        model[cells[0]] = cells[1]
    return model


def _fault_codes(text: str) -> list[str]:
    """Extract the F-codes mentioned in a cell, in order."""
    codes: list[str] = []
    i = 0
    while i < len(text):
        if text[i] == "F" and i + 1 < len(text) and text[i + 1].isdigit():
            j = i + 1
            while j < len(text) and text[j].isdigit():
                j += 1
            code = text[i:j]
            if code not in codes:
                codes.append(code)
            i = j
        else:
            i += 1
    return codes


def _sorted_codes(model: dict[str, str]) -> list[str]:
    """Return model codes in ascending numeric order (F1, F2, ..., F11)."""
    def key(code: str) -> int:
        digits = "".join(ch for ch in code if ch.isdigit())
        return int(digits) if digits else 0
    return sorted(model.keys(), key=key)


def _union(cells: list[str]) -> str:
    """Join distinct non-empty cells with '; ' preserving first-seen order."""
    seen: list[str] = []
    for c in cells:
        c = c.strip()
        if c in ("", "-", "—"):
            continue
        for part in c.split("; "):
            part = part.strip()
            if part and part not in seen:
                seen.append(part)
    return "; ".join(seen)


def _first_nonempty(cells: list[str]) -> str:
    for c in cells:
        c = c.strip()
        if c not in ("", "-", "—"):
            return c
    return "—"


def build_fmea(cases: list[dict], model: dict[str, str]) -> list[dict]:
    """Assemble one FMEA entry per fault code of the model."""
    by_code: dict[str, list[dict]] = {code: [] for code in model}
    for case in cases:
        for code in _fault_codes(case.get("Fault (model code)", "")):
            if code in by_code:
                by_code[code].append(case)

    entries: list[dict] = []
    for code in _sorted_codes(model):
        rows = by_code[code]
        rating = RATINGS.get(code, (0, 0, 0, "", ""))
        severity, occurrence, detectability, detection, mitigation = rating
        criticality = severity * occurrence * detectability

        # A fault is flagged when it has neither a mitigation measure nor a
        # covering test: the barrier is missing on both sides.
        coverage = _union([r.get("Covered by", "") for r in rows])
        flagged = (mitigation == "") and (coverage == "")

        entries.append({
            "code": code,
            "mode": model.get(code, code),
            "cause": _first_nonempty([r.get("Fault (model code)", "") for r in rows]),
            "local_effect": _union([r.get("What the actuator sees", "") for r in rows]),
            "system_effect": _union([r.get("Expected outcome", "") for r in rows]),
            "detection": detection,
            "severity": severity,
            "occurrence": occurrence,
            "detectability": detectability,
            "criticality": criticality,
            "mitigation": mitigation,
            "requirements": _union([r.get("Requirements", "") for r in rows]),
            "coverage": coverage,
            "flagged": flagged,
        })
    return entries


def flag_unmitigated(entries: list[dict]) -> list[dict]:
    """Return the entries with no mitigation measure and no covering test."""
    return [e for e in entries if e["flagged"]]


FMEA_COLUMNS = [
    "ID", "Mode", "Cause", "Local effect", "System effect", "Detection",
    "Severity (S)", "Occurrence (O)", "Detectability (D)",
    "Criticality (S*O*D)", "Mitigation", "Requirements", "Coverage",
]


def render_markdown(entries: list[dict]) -> str:
    """Render the FMEA as a Markdown table plus the flag list."""
    out: list[str] = []
    out.append("# Relais -- generated FMEA (from docs/TRUTH-TABLE.md)")
    out.append("")
    out.append("Generated by `host/safety/fmea.py`; do not edit by hand. The "
               "modes, causes, effects, requirements and coverage are parsed from "
               "the truth table and the spec-3.3 fault model. The S/O/D ratings and "
               "the criticality are engineering judgement, **not** measurements.")
    out.append("")
    out.append("| " + " | ".join(FMEA_COLUMNS) + " |")
    out.append("|" + "---|" * len(FMEA_COLUMNS))
    for e in entries:
        row = [
            e["code"], e["mode"], e["cause"], e["local_effect"],
            e["system_effect"], e["detection"], str(e["severity"]),
            str(e["occurrence"]), str(e["detectability"]),
            str(e["criticality"]), e["mitigation"], e["requirements"],
            e["coverage"],
        ]
        out.append("| " + " | ".join(row) + " |")
    out.append("")
    out.append("## Faults of the model without a mitigation measure nor a test")
    out.append("")
    flagged = flag_unmitigated(entries)
    if flagged:
        for e in flagged:
            out.append(f"- **{e['code']} — {e['mode']}**: no mitigation measure and "
                       f"no covering test in the current scope "
                       f"(extension: two actuator channels).")
    else:
        out.append("- none.")
    out.append("")
    return "\n".join(out)


def render_csv(entries: list[dict]) -> str:
    """Render the FMEA as CSV (RFC-4180, deterministic)."""
    buf = io.StringIO()
    writer = csv.writer(buf, lineterminator="\n")
    writer.writerow(FMEA_COLUMNS)
    for e in entries:
        writer.writerow([
            e["code"], e["mode"], e["cause"], e["local_effect"],
            e["system_effect"], e["detection"], e["severity"],
            e["occurrence"], e["detectability"], e["criticality"],
            e["mitigation"], e["requirements"], e["coverage"],
        ])
    return buf.getvalue()


def _csv_default(out_md: str | None) -> str | None:
    if out_md is None:
        return None
    base, _ = os.path.splitext(out_md)
    return base + ".csv"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate the Relais FMEA.")
    parser.add_argument("--out", help="write the Markdown table to this file")
    parser.add_argument("--csv", help="write the CSV table to this file")
    parser.add_argument("--table", default=TRUTH_TABLE,
                        help="path to docs/TRUTH-TABLE.md")
    args = parser.parse_args(argv)

    cases = parse_detailed_cases(args.table)
    model = parse_fault_model(args.table)
    entries = build_fmea(cases, model)

    md = render_markdown(entries)
    csv_text = render_csv(entries)

    if args.out:
        with io.open(args.out, "w", encoding="utf-8") as fh:
            fh.write(md)
    else:
        sys.stdout.write(md)

    csv_path = args.csv if args.csv else _csv_default(args.out)
    if csv_path:
        with io.open(csv_path, "w", encoding="utf-8") as fh:
            fh.write(csv_text)

    for e in flag_unmitigated(entries):
        sys.stderr.write(f"fmea: unmitigated fault {e['code']} ({e['mode']}) "
                         f"has no mitigation measure nor covering test\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())