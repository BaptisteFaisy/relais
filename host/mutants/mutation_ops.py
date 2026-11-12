#!/usr/bin/env python3
"""Relais — the mutation operators used to score the core (docs/MUTATION-SCORE.md).

A mutant is one line of C turned into a *plausible* faulty variant, by one of
the classic operators below. The operators are pure text: they know nothing of
the build, the clock, or the file system beyond the source text. `mutation_score.py`
drives the builds and the verdicts (killed / survived / equivalent / invalid).

Requirement ID: INJ-11 (mutants run the same campaign, which must flag them).
Here the "campaign" is the whole test suite plus reduced simulator/explorer
scenarios, and the point is the reverse of spec 7.6: instead of eight hand-made
design defects, tiny mechanical faults are injected and counted, so the strength
of the test suite is measured rather than asserted.
"""
import re
from typing import List, NamedTuple, Optional, Tuple

# One mutant: the smallest text edit that changes the behaviour, with its
# provenance so the report can point at (file, line) and the operator intent.
class Mutation(NamedTuple):
    file: str
    line: int          # 1-based line number
    col: int           # 0-based column at which `before` starts
    operator: str      # operator family ("cmp", "bound", "logic", "sign", "const")
    before: str        # original text slice
    after: str         # mutated text slice


_CHAR_CLASS = re.compile(r"[A-Za-z0-9_]")


def _is_unary_minus(line: str, pos: int) -> bool:
    """True when the '-' at pos is preceded by an operator, not an operand."""
    i = pos - 1
    while i >= 0 and line[i] in " \t":
        i -= 1
    if i < 0:
        return True
    prev = line[i]
    return prev in "(,=<>!&|+-*/%[{;:?~"


def _digits(token_source) -> list:
    """For 'const'/'bound': the literal and its +/-1 neighbours."""
    value = int(token_source)
    out = [(token_source, str(value + 1), "const+1")]
    if value > 0:
        out.append((token_source, str(value - 1), "const-1"))
    return out


def mask_line(line: str, in_block: bool) -> Tuple[List[bool], bool]:
    """Return mask[i] == True when column i is ordinary code (mutable), and
    the block-comment state for the next line."""
    mask = [True] * len(line)
    i = 0
    n = len(line)
    while i < n:
        ch = line[i]
        if in_block:
            end = line.find("*/", i)
            if end == -1:
                for j in range(i, n):
                    mask[j] = False
                return mask, True
            for j in range(i, end + 2):
                mask[j] = False
            i = end + 2
            in_block = False
            continue
        if ch == '"' or ch == "'":
            quote = ch
            mask[i] = False
            i += 1
            while i < n:
                mask[i] = False
                if line[i] == "\\":
                    mask[min(i + 1, n - 1)] = False
                    i += 2
                    continue
                if line[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if line.startswith("//", i):
            for j in range(i, n):
                mask[j] = False
            break
        if line.startswith("/*", i):
            for j in range(i, i + 2):
                mask[j] = False
            in_block = True
            i += 2
            continue
        i += 1
    return mask, in_block


def _cmp_swaps(token: str) -> Optional[str]:
    table = {"<=": "<", ">=": ">", "==": "!=", "!=": "==", "<": "<=", ">": ">="}
    return table.get(token)


def generate(lines_by_file) -> List[Mutation]:
    """Produce every mutation of every line, de-duplicated by (file,line,col,after).

    `lines_by_file`: dict {relative_path: [line, ...]} (with line endings
    already stripped). Lines that start with '#' (preprocessor) or that are
    inside a string/char/comment are left untouched."""
    cmp_re = re.compile(r"(<=|>=|==|!=|<|>)")
    bound_re = re.compile(r"(<=|>=|<|>)\s*(\d+)")
    logic_re = re.compile(r"(&&|\|\|)")
    sign_re = re.compile(r"-")
    const_re = re.compile(r"(?<![A-Za-z0-9_.])(\d+)(?![A-Za-z0-9_.])")

    found = {}  # key -> Mutation (first wins, so the more specific label wins)
    for path, lines in lines_by_file.items():
        in_block = False
        for idx, raw in enumerate(lines, start=1):
            if raw.lstrip().startswith("#"):
                continue
            mask, in_block = mask_line(raw, in_block)
            if in_block:  # block comment still open: nothing mutable here
                continue
            ops = []

            for m in cmp_re.finditer(raw):
                tok = m.group(0)
                lo, hi = m.start(), m.end()
                if not all(mask[c] for c in range(lo, hi)):
                    continue
                if tok == "<" and (lo > 0 and raw[lo - 1] == "<" or
                                   hi < len(raw) and raw[hi] in "<="):
                    continue
                if tok == ">" and (lo > 0 and raw[lo - 1] in ">-" or
                                   hi < len(raw) and raw[hi] in ">="):
                    continue
                after = _cmp_swaps(tok)
                ops.append(("cmp", lo, tok, after, tok + "->" + after))

            for m in bound_re.finditer(raw):
                cmp_tok, lit = m.group(1), m.group(2)
                c0 = m.start(2)
                if not all(mask[c] for c in range(c0, m.end(2))):
                    continue
                for before, after, label in _digits(lit):
                    ops.append(("bound", c0, before, after, label))

            for m in logic_re.finditer(raw):
                tok = m.group(0)
                lo, hi = m.start(), m.end()
                if not all(mask[c] for c in range(lo, hi)):
                    continue
                after = "||" if tok == "&&" else "&&"
                ops.append(("logic", lo, tok, after, tok + "->" + after))

            for m in sign_re.finditer(raw):
                pos = m.start()
                nxt = pos + 1
                while nxt < len(raw) and raw[nxt] in " \t":
                    nxt += 1
                if nxt >= len(raw) or not _CHAR_CLASS.match(raw[nxt]):
                    continue
                if raw[nxt:].lstrip().startswith("->"):
                    continue
                if not mask[pos] or not _is_unary_minus(raw, pos):
                    continue
                ops.append(("sign", pos, "-", "+", "- -> +"))

            for m in const_re.finditer(raw):
                lit = m.group(0)
                lo, hi = m.start(), m.end()
                if not all(mask[c] for c in range(lo, hi)):
                    continue
                for before, after, label in _digits(lit):
                    ops.append(("const", lo, before, after, label))

            # One text edit appears at most once; the first (most specific)
            # operator family wins the label, e.g. 'bound' before 'const'.
            for family, col, before, after, _label in ops:
                key = (path, idx, col, after)
                if key not in found:
                    found[key] = Mutation(path, idx, col, family, before, after)
    return list(found.values())


def select(mutations: List[Mutation], limit: int) -> List[Mutation]:
    """A deterministic, representative sample: round-robin over (file, operator)
    buckets so a capped run still covers every file and every operator family."""
    if limit <= 0 or limit >= len(mutations):
        return sorted(mutations, key=lambda m: (m.file, m.line, m.col, m.operator))
    buckets = {}
    order = []
    for m in sorted(mutations, key=lambda m: (m.file, m.operator, m.line, m.col)):
        key = (m.file, m.operator)
        if key not in buckets:
            buckets[key] = []
            order.append(key)
        buckets[key].append(m)
    chosen = []
    i = 0
    while len(chosen) < limit:
        progressed = False
        for key in order:
            if i < len(buckets[key]):
                chosen.append(buckets[key][i])
                progressed = True
                if len(chosen) >= limit:
                    break
        if not progressed:
            break
        i += 1
    return chosen


def apply_mutation(path: str, mutation: Mutation) -> None:
    """Rewrite `path` in place: replace columns [col, col+len(before)) with after."""
    with open(path, encoding="utf-8") as fh:
        lines = fh.readlines()
    line = lines[mutation.line - 1]
    lo = mutation.col
    hi = lo + len(mutation.before)
    if line[lo:hi] != mutation.before:
        raise ValueError("mutation anchor drifted in %s:%d: %r != %r" % (
            mutation.file, mutation.line, line[lo:lo + len(mutation.before)],
            mutation.before))
    lines[mutation.line - 1] = line[:lo] + mutation.after + line[hi:]
    with open(path, "w", encoding="utf-8") as fh:
        fh.writelines(lines)