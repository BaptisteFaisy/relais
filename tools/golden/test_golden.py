#!/usr/bin/env python3
"""Relais — tests of the golden trace tool (mission M35).

Standard library only, no simulator needed: the tests cover the contract of
the normalisation (the volatile fields must be neutralised, and the
normalisation must be idempotent), the translation of the faults into the
simulator's vocabulary, the localisation of a divergence, and the coherence
between the frozen game and the versioned reference.

  python3 -m unittest discover -s tools/golden -p 'test_*.py'
"""

import os
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import golden_lib
import verify


class NormalizationTest(unittest.TestCase):
    """The volatile fields of the journal must not reach the hash."""

    def test_volatile_fields_are_neutralised(self):
        line = ('{"kind":"tick","tick":7,"ns":1717171717,"pid":4242,'
                '"path":"/tmp/run-42/journal.jsonl","value":0.5}')
        text, counts = golden_lib.normalize(line + "\n")
        self.assertNotIn("1717171717", text)
        self.assertNotIn("4242", text)
        self.assertNotIn("/tmp/run-42", text)
        self.assertIn('"ns":0', text)
        self.assertIn('"pid":0', text)
        self.assertIn('"<path>"', text)
        self.assertEqual(counts["ns"], 1)

    def test_normalization_is_idempotent(self):
        line = '{"kind":"tick","tick":7,"ns":42,"value":0.5}'
        once, _ = golden_lib.normalize(line)
        twice, _ = golden_lib.normalize(once)
        self.assertEqual(once, twice)

    def test_the_decision_fields_are_untouched(self):
        line = '{"kind":"tick","tick":7,"mandate":2,"holder":"B","value":-0.125}'
        text, _ = golden_lib.normalize(line)
        self.assertIn('"mandate":2', text)
        self.assertIn('"holder":"B"', text)
        self.assertIn('"value":-0.125', text)


class FaultTranslationTest(unittest.TestCase):
    """A scenario fault must reach the simulator in its own vocabulary."""

    def test_death_of_the_second_machine(self):
        spec = golden_lib.fault_spec({"at_tick": 800, "type": "death", "target": "B"})
        self.assertEqual(spec, "800:death:1:0:0")

    def test_cut_between_the_computers(self):
        spec = golden_lib.fault_spec({"at_tick": 700, "type": "cut",
                                      "link": "A-B", "direction": "both"})
        self.assertEqual(spec, "700:cut-peer:0:0:0")

    def test_update_carries_its_duration(self):
        spec = golden_lib.fault_spec({"at_tick": 30, "type": "update",
                                      "target": "B", "ticks": 20})
        self.assertEqual(spec, "30:update:1:20:0")

    def test_bias_carries_its_value(self):
        spec = golden_lib.fault_spec({"at_tick": 300, "type": "bias",
                                      "target": "B", "value": 0.02})
        self.assertEqual(spec, "300:bias:1:0:0.02")


class DivergenceTest(unittest.TestCase):
    """The guard must name the first divergent line."""

    def test_first_divergent_line(self):
        stored = golden_lib.line_digests('{"a":1}\n{"b":2}\n{"c":3}\n')
        same = golden_lib.line_digests('{"a":1}\n{"b":2}\n{"c":3}\n')
        other = golden_lib.line_digests('{"a":1}\n{"b":9}\n{"c":3}\n')
        self.assertIsNone(verify.first_divergent_line(stored, same))
        self.assertEqual(verify.first_divergent_line(stored, other), 1)

    def test_a_missing_line_is_a_divergence(self):
        stored = golden_lib.line_digests('{"a":1}\n{"b":2}\n')
        shorter = golden_lib.line_digests('{"a":1}\n')
        self.assertEqual(verify.first_divergent_line(stored, shorter), 1)


class GameTest(unittest.TestCase):
    """The frozen game and the versioned reference must stay coherent."""

    def setUp(self):
        self.game = golden_lib.resolve_game(golden_lib.load_manifest())

    def test_the_game_is_frozen_and_complete(self):
        self.assertGreaterEqual(len(self.game), 20)
        names = [entry["name"] for entry in self.game]
        self.assertEqual(len(names), len(set(names)))
        for entry in self.game:
            self.assertGreater(entry["ticks"], 0)
            self.assertIn(entry["level"], ("socle", "complet"))

    def test_every_core_behaviour_is_covered(self):
        specs = " ".join(spec for entry in self.game for spec in entry["specs"])
        for kind in ("death", "freeze", "cut-actuator", "cut-peer", "slow",
                     "update", "handover", "bias", "forge", "forge-stale",
                     "crash", "restart", "restore"):
            self.assertIn(kind, specs, "behaviour not covered: %s" % kind)

    def test_the_reference_holds_the_same_scenarios(self):
        traces = golden_lib.read_traces()
        names = [trace["name"] for trace in traces["scenarios"]]
        self.assertEqual(names, [entry["name"] for entry in self.game])

    def test_the_reference_records_no_violation(self):
        traces = golden_lib.read_traces()
        for trace in traces["scenarios"]:
            self.assertEqual(trace["exit"], 0, trace["name"])


if __name__ == "__main__":
    unittest.main()
