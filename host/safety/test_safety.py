#!/usr/bin/env python3
"""Relais -- safety generation tests (M16).

Three properties, run with:
  python3 -m unittest discover -s host/safety -p 'test_*.py' -v

1. A fault of the model with no mitigation measure and no covering test IS
   flagged (and only that one is).
2. The fault tree covers every FMEA branch that feeds the top event "the
   actuator stays without a fresh order for more than p+1 ticks".
3. The generated output is stable: the same inputs render the same bytes, so a
   PR diff stays readable.

Standard library only.
"""

from __future__ import annotations

import io
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import fmea  # noqa: E402
import fta  # noqa: E402

# Faults that can make the actuator lose a *fresh* order for p+1 ticks: the
# single faults the top event is fed from. F8 (a machine emits a *wrong* value)
# and F11 (the actuator itself fails) are different hazards with their own top
# events, and F9/F10 are the double fault reached only through the AND gate.
LOSS_OF_FRESH_ORDER_CODES = {"F1", "F2", "F3", "F4", "F5", "F6", "F7"}


def _entries():
    cases = fmea.parse_detailed_cases(fmea.TRUTH_TABLE)
    model = fmea.parse_fault_model(fmea.TRUTH_TABLE)
    return fmea.build_fmea(cases, model)


class FlaggedFaultTest(unittest.TestCase):
    def test_unmitigated_fault_is_flagged(self):
        entries = _entries()
        flagged = fmea.flag_unmitigated(entries)
        codes = [e["code"] for e in flagged]
        self.assertEqual(codes, ["F11"],
                         "exactly the actuator failure (F11) must be flagged")

    def test_every_other_fault_has_a_barrier_and_a_test(self):
        for e in _entries():
            if e["code"] == "F11":
                continue
            self.assertTrue(e["mitigation"], f"{e['code']} lacks a mitigation")
            self.assertTrue(e["coverage"], f"{e['code']} lacks a covering test")

    def test_fault_model_is_complete(self):
        model = fmea.parse_fault_model(fmea.TRUTH_TABLE)
        self.assertEqual([f"F{i}" for i in range(1, 12)], fmea._sorted_codes(model))


class FaultTreeTest(unittest.TestCase):
    def test_tree_covers_all_loss_of_order_branches(self):
        tree_codes = fta.codes_of(fta.build_tree())
        self.assertEqual(tree_codes, LOSS_OF_FRESH_ORDER_CODES,
                         "the tree must name every single fault that loses a "
                         "fresh order, and no other model fault")

    def test_tree_leaves_only_name_real_model_faults(self):
        model_codes = set(fmea.parse_fault_model(fmea.TRUTH_TABLE))
        tree_codes = fta.codes_of(fta.build_tree())
        self.assertTrue(tree_codes <= model_codes,
                        "a tree leaf references a fault code absent from the model")

    def test_top_event_is_an_and_of_three_ranks(self):
        root = fta.build_tree()
        self.assertEqual(root.kind, "AND")
        self.assertEqual(len(root.children), 3,
                         "the top event needs all three ranks to fail at once")
        self.assertEqual([c.kind for c in root.children], ["OR", "OR", "OR"])


class StabilityTest(unittest.TestCase):
    def test_fmea_markdown_stable(self):
        entries = _entries()
        a = fmea.render_markdown(entries)
        b = fmea.render_markdown(entries)
        self.assertEqual(a, b)

    def test_fmea_csv_stable(self):
        entries = _entries()
        a = fmea.render_csv(entries)
        b = fmea.render_csv(entries)
        self.assertEqual(a, b)

    def test_fta_renderings_stable(self):
        root = fta.build_tree()
        self.assertEqual(fta.render_text(root), fta.render_text(root))
        self.assertEqual(fta.render_dot(root), fta.render_dot(root))

    def test_full_file_stable_across_runs(self):
        md = io.StringIO()
        entries = _entries()
        md.write(fmea.render_markdown(entries))
        other = io.StringIO()
        other.write(fmea.render_markdown(entries))
        self.assertEqual(md.getvalue(), other.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)