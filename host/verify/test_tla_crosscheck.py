"""Unit tests for host/verify/tla_crosscheck.py.

The tests build small models by hand: an agreement that must be reported as
such, a disagreement that must be caught, and a degenerate model that must not
crash.  Nothing here needs TLC, java or the C binaries.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import tla_crosscheck as tc  # noqa: E402


def tiny_config():
    return tc.Config(max_tick=1, max_mandate=1, n_horizon=1, patience=1,
                     certify_ticks=1)


class TestInvariants(unittest.TestCase):
    """The invariants are evaluated on one state at a time."""

    def test_initial_state_is_clean(self):
        state = tc.init_state(tiny_config())
        self.assertEqual(tc.check_invariants(state, tiny_config()), [])

    def test_i1_catches_an_order_with_no_holder(self):
        cfg = tiny_config()
        state = list(tc.init_state(cfg))
        state[tc.TICK] = 1
        state[tc.SOURCE] = "holder"
        state[tc.HOLDER] = tc.NONE
        state[tc.APPFROM] = tc.NONE
        state[tc.APPTICK] = 1
        self.assertIn("I1", tc.check_invariants(tuple(state), cfg))

    def test_i3_catches_an_order_from_a_deposed_machine(self):
        cfg = tiny_config()
        state = list(tc.init_state(cfg))
        state[tc.TICK] = 1
        state[tc.SOURCE] = "holder"
        state[tc.HOLDER] = "A"
        state[tc.APPFROM] = "B"          # applied from somebody else
        state[tc.APPMAND] = 1
        self.assertIn("I3", tc.check_invariants(tuple(state), cfg))

    def test_i6_catches_a_will_played_past_its_horizon(self):
        cfg = tiny_config()
        state = list(tc.init_state(cfg))
        state[tc.TICK] = 5
        state[tc.SOURCE] = "testament"
        state[tc.TLEN] = 1
        state[tc.TBASE] = 1
        state[tc.TRUN] = 4
        found = tc.check_invariants(tuple(state), cfg)
        self.assertIn("I6", found)

    def test_safety_catches_two_machines_applied_at_once(self):
        cfg = tiny_config()
        state = list(tc.init_state(cfg))
        state[tc.APPSET] = frozenset({"A", "B"})
        self.assertIn("Safety", tc.check_invariants(tuple(state), cfg))

    def test_i2_catches_two_holders_of_one_mandate(self):
        cfg = tiny_config()
        state = list(tc.init_state(cfg))
        holders = list(state[tc.MANDHOLDERS])
        holders[1] = frozenset({"A", "B"})
        state[tc.MANDHOLDERS] = tuple(holders)
        self.assertIn("I2", tc.check_invariants(tuple(state), cfg))


class TestExplore(unittest.TestCase):
    """The reduced machine must explore and stay clean."""

    def test_explore_finds_states_and_no_violation(self):
        result = tc.explore(tiny_config())
        self.assertGreater(result["states"], 1)
        self.assertEqual(result["violations"], [])
        self.assertIn((0, tc.NONE), result["pairs"])

    def test_degenerate_model_does_not_crash(self):
        cfg = tc.Config(max_tick=0, max_mandate=0, n_horizon=0, patience=0,
                        certify_ticks=1)
        result = tc.explore(cfg)
        self.assertEqual(result["violations"], [])
        self.assertGreaterEqual(result["states"], 1)

    def test_successors_are_bounded(self):
        cfg = tiny_config()
        state = tc.init_state(cfg)
        self.assertLessEqual(len(tc.successors(state, cfg)), 40)


class TestCompare(unittest.TestCase):
    """The comparison table is the point of the tool."""

    MODEL = {"states": 100, "violations": [], "sources": {"holder", "fallback"},
             "pairs": {(0, "none"), (1, "A")}}
    TLC = {"states": 100, "ok": True, "violation": None}
    EXPLORE = {"states": 900000, "ok": True, "violation": None}
    SIM = {"pairs": {(0, "none"), (1, "A")}, "sources": {"holder", "fallback"},
           "violations": []}

    def test_agreement_reports_no_disagreement(self):
        rows = tc.compare(self.MODEL, self.TLC, self.EXPLORE, self.SIM)
        self.assertTrue(all(row[3] for row in rows), rows)

    def test_a_violation_in_the_model_is_caught(self):
        model = dict(self.MODEL, violations=[("I3", 2, 1, "B", "holder")])
        rows = tc.compare(model, self.TLC, self.EXPLORE, self.SIM)
        self.assertFalse(all(row[3] for row in rows))

    def test_a_state_count_mismatch_is_caught(self):
        rows = tc.compare(self.MODEL, dict(self.TLC, states=101),
                          self.EXPLORE, self.SIM)
        bad = [row for row in rows if not row[3]]
        self.assertTrue(any("states" in row[0] for row in bad), rows)

    def test_a_c_pair_outside_the_model_is_caught(self):
        sim = dict(self.SIM, pairs=self.SIM["pairs"] | {(7, "B")})
        rows = tc.compare(self.MODEL, self.TLC, self.EXPLORE, sim)
        bad = [row for row in rows if not row[3]]
        self.assertTrue(any("mandate/holder" in row[0] for row in bad), rows)

    def test_a_source_mismatch_is_caught(self):
        sim = dict(self.SIM, sources={"holder"})
        rows = tc.compare(self.MODEL, self.TLC, self.EXPLORE, sim)
        self.assertFalse(all(row[3] for row in rows))

    def test_missing_tlc_is_reported_not_faked(self):
        rows = tc.compare(self.MODEL, None, {"error": "x"}, {"error": "y"})
        self.assertTrue(all(row[3] for row in rows))
        self.assertTrue(any(row[1] == "not run" for row in rows))


class TestHelpers(unittest.TestCase):
    def test_release_goes_to_the_certified_second(self):
        cfg = tiny_config()
        self.assertEqual(tc.release(True, "B", "A", 3, cfg), ("B", 4, False, tc.NONE))

    def test_release_goes_vacant_without_a_certified_second(self):
        cfg = tiny_config()
        self.assertEqual(tc.release(False, tc.NONE, "A", 3, cfg),
                         (tc.NONE, 3, False, tc.NONE))

    def test_cap_saturates(self):
        self.assertEqual(tc.cap(9, 2), 2)
        self.assertEqual(tc.cap(1, 2), 1)

    def test_config_round_trip(self):
        cfg = tiny_config()
        self.assertEqual(set(cfg.as_dict()),
                         {"MaxTick", "MaxMandate", "N", "P", "CertifyTicks"})


if __name__ == "__main__":
    unittest.main()
