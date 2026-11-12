#!/usr/bin/env python3
"""Relais -- fault tree for the top event (M16).

Top event: "the actuator stays without a fresh order for more than p+1 ticks".

The tree is assembled from the fault model of spec 3.3 and the switch-over
principle of docs/SAFETY.md / docs/PROTOCOL.md. It reads nothing from the file
system: the structure is encoded once, as data, so that the two renderers (text
and Graphviz DOT) share the same single source of truth.

The shape matters: the top event is an AND of three subtrees -- "no holder
order", "no certified shadow", "the will cannot bridge". Under the
single-fault hypothesis no single elementary fault sets the top event: it needs
a fault on the holder side AND one on the second side (a double fault, F9/F10).
Each elementary cause carries the barrier that stops it.

Output:
  * --text: the indented tree (default, printed to stdout),
  * --dot : the Graphviz DOT (printed to stdout).

Usage:
  python3 host/safety/fta.py
  python3 host/safety/fta.py --text
  python3 host/safety/fta.py --dot
Standard library only, deterministic (byte-identical on the same inputs).
"""

from __future__ import annotations

import argparse
import sys

TOP_EVENT = "the actuator stays without a fresh order for more than p+1 ticks"


class Gate:
    """An AND/OR gate over children (events or gates)."""

    def __init__(self, kind: str, label: str, children: list):
        assert kind in ("AND", "OR"), kind
        self.kind = kind
        self.label = label
        self.children = children


class Event:
    """An elementary cause, with the barrier that stops it."""

    def __init__(self, text: str, barrier: str):
        self.text = text
        self.barrier = barrier


def build_tree():
    """Return the fault tree rooted at the top AND gate."""
    return Gate("AND", TOP_EVENT, [
        Gate("OR", "no fresh holder order (rank 1 is lost)", [
            Event("holder dies (F1)", "revoke on p+1 silence, auto-switch "
                  "to the certified second (MAN-04, REL-01)"),
            Event("holder freezes (F2)", "stale orders dropped by mandate "
                  "number and tick label; revenant stands down (MAN-08, REL-07)"),
            Event("holder misses consecutive ticks (F3)", "miss counter in every "
                  "STATUT; deposed only on consecutive misses (DET-03)"),
            Event("holder-actuator link cut, one or both ways (F5)", "handover to "
                  "the second; the machine deposes itself (REL-01, MAN-09)"),
            Event("holder frames lost, duplicated, reordered or delayed (F6)",
                  "tick label and mandate number; absorbed within the parameters "
                  "(ROB-02)"),
            Event("holder frames corrupted or forged (F7)", "CRC-32C / SipHash MAC, "
                  "dropped and counted (DET-04, SEC-02)"),
        ]),
        Gate("OR", "no certified shadow (rank 2 is lost)", [
            Event("second dies (F1)", "no handover; yellow 'second absent'; "
                  "holder keeps commanding (REP-05, DEG-02)"),
            Event("second-actuator link cut (F5)", "certification lost after more "
                  "than p missing shadows (REP-05, DET-08)"),
            Event("inter-computer link cut (F4)", "second rebuilds a bump-free "
                  "state from the STATUT (REP-04, DET-06)"),
            Event("second never certified", "certification streak must be reached "
                  "before the shadow is applied (REP-05)"),
        ]),
        Gate("OR", "the will cannot keep covering (rank 3 is exhausted)", [
            Event("will horizon reached after N+1 ticks without a fresh order",
                  "local damped fallback loop; red lamp; stop requested "
                  "(ACT-02, ACT-03, DEG-05)"),
        ]),
    ])


def _preorder(root):
    """Yield every node (gates and events) once, in a deterministic order."""
    yield root
    for child in getattr(root, "children", []):
        yield from _preorder(child)


def _events(root):
    """Yield the elementary causes (leaves) only, in order."""
    for node in _preorder(root):
        if isinstance(node, Event):
            yield node


def codes_of(root):
    """Return the set of fault codes (F1..F11) named by the tree leaves."""
    codes: set[str] = set()
    for ev in _events(root):
        for token in ev.text.replace("(", " ").replace(")", " ").split():
            token = token.strip(",")
            if token.startswith("F") and token[1:].isdigit():
                codes.add(token)
    return codes


def render_text(root) -> str:
    """Render the tree as an indented text listing, barriers included."""
    lines: list[str] = []
    pad = "    "

    def emit(node, depth):
        prefix = pad * depth
        if isinstance(node, Event):
            lines.append(f"{prefix}- {node.text}")
            lines.append(f"{prefix}  barrier: {node.barrier}")
        elif node is root:
            lines.append(f"{node.kind}: {node.label}")
        else:
            lines.append(f"{prefix}{node.kind}: {node.label}")

    def walk(node, depth):
        emit(node, depth)
        for child in getattr(node, "children", []):
            walk(child, depth + 1)

    walk(root, 0)
    return "\n".join(lines) + "\n"


def render_dot(root) -> str:
    """Render the tree as a Graphviz digraph with AND/OR gate shapes."""
    ids = {id(node): f"n{i}" for i, node in enumerate(_preorder(root))}
    lines = [
        "digraph fault_tree {",
        "  rankdir=TB;",
        '  node [shape=box, fontname="Helvetica", fontsize=10];',
    ]

    def esc(s):
        return s.replace('"', "'").replace("\n", " ")

    for node in _preorder(root):
        nid = ids[id(node)]
        if isinstance(node, Gate):
            if node is root:
                lines.append(f'  {nid} [label="{esc(node.label)}", shape=box, '
                             f'style=filled, fillcolor="#fde0dc"];')
            else:
                shape = "invhouse" if node.kind == "AND" else "diamond"
                lines.append(f'  {nid} [label="{node.kind}: {esc(node.label)}", '
                             f'shape={shape}];')
        else:
            label = f"{esc(node.text)}\\nbarrier: {esc(node.barrier)}"
            lines.append(f'  {nid} [label="{label}", shape=ellipse];')

    for node in _preorder(root):
        if not isinstance(node, Gate):
            continue
        for child in node.children:
            lines.append(f"  {ids[id(node)]} -> {ids[id(child)]};")

    lines.append("}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Render the Relais fault tree.")
    parser.add_argument("--text", action="store_true", help="print the text tree")
    parser.add_argument("--dot", action="store_true", help="print the Graphviz DOT")
    args = parser.parse_args(argv)

    root = build_tree()
    if args.dot and not args.text:
        sys.stdout.write(render_dot(root))
    else:
        sys.stdout.write(render_text(root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())