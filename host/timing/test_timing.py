#!/usr/bin/env python3
"""test_timing.py — unit tests for wcet.py and rta.py.

Run:  python3 -m unittest discover -s host/timing -p 'test_*.py' -v
"""
import math
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rta
import wcet
from wcet import (describe, gumbel_pot_bound, percentile, samples_for_quantile_bound)
from rta import (Stage, analyze, response_time)


class TestPercentiles(unittest.TestCase):
    def test_linear_interpolation_known_set(self):
        ten = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90]
        self.assertEqual(percentile(ten, 0.0), 0.0)
        self.assertEqual(percentile(ten, 1.0), 90.0)
        self.assertEqual(percentile(ten, 0.5), 45.0)
        self.assertAlmostEqual(percentile(ten, 0.9), 81.0)

    def test_describe_exact(self):
        d = describe([1.0, 2.0, 2.0, 3.0])
        self.assertEqual(d["n"], 4)
        self.assertEqual(d["min"], 1.0)
        self.assertEqual(d["max"], 3.0)
        self.assertAlmostEqual(d["mean"], 2.0)
        self.assertEqual(d["p50"], 2.0)

    def test_single_sample(self):
        d = describe([7.0])
        self.assertEqual(d["min"], 7.0)
        self.assertEqual(d["max"], 7.0)
        self.assertEqual(d["mean"], 7.0)
        self.assertEqual(d["stdev"], 0.0)


class TestWcetExtrapolation(unittest.TestCase):
    def test_extrapolation_monotone_in_tail(self):
        # Deterministic heavy tail via the synthetic generator.
        import synth_samples
        values = sorted(o["latency_us"] for o in synth_samples.generate(20000, seed=1))
        loose = gumbel_pot_bound(values, 0.99, 1e-4, 20)
        strict = gumbel_pot_bound(values, 0.99, 1e-8, 20)
        self.assertIsNotNone(loose)
        self.assertIsNotNone(strict)
        self.assertGreater(strict["bound"], loose["bound"])
        self.assertGreaterEqual(strict["hi"], strict["bound"])

    def test_honest_when_too_few_total_samples(self):
        self.assertIsNone(gumbel_pot_bound([1.0, 2.0, 3.0], 0.99, 1e-6, 20))

    def test_honest_when_no_exceedances(self):
        values = [5.0] * 1000
        self.assertIsNone(gumbel_pot_bound(values, 0.99, 1e-6, 20))

    def test_samples_for_quantile_bound(self):
        q, conf = 0.9999, 0.95
        expected = math.ceil(math.log(1 - conf) / math.log(q))
        self.assertEqual(samples_for_quantile_bound(q, conf), expected)


class TestRta(unittest.TestCase):
    def test_3task_hand_computed_schedulable(self):
        stages = [Stage("A", 3, 1), Stage("B", 2, 2), Stage("C", 1, 2)]
        a = analyze(stages, period=10)
        r_by_name = {s.name: r for s, r in a["results"]}
        self.assertEqual(r_by_name["A"], 1.0)
        self.assertEqual(r_by_name["B"], 3.0)
        self.assertEqual(r_by_name["C"], 5.0)
        self.assertEqual(a["chain_us"], 9.0)
        self.assertTrue(a["schedulable"])

    def test_fixed_point_reached(self):
        stage = Stage("B", 1, 2)
        r = response_time(stage, [1.0, 2.0], 10)
        self.assertIsNotNone(r)
        # A fixed point satisfies r == C + sum ceil(r/T)*C_h exactly.
        rhs = 2.0 + math.ceil(r / 10.0) * 1.0 + math.ceil(r / 10.0) * 2.0
        self.assertEqual(r, rhs)

    def test_not_schedulable_by_chain(self):
        stages = [Stage("A", 3, 2), Stage("B", 2, 3), Stage("C", 1, 1)]
        a = analyze(stages, period=10)
        r_by_name = {s.name: r for s, r in a["results"]}
        self.assertEqual(r_by_name["A"], 2.0)
        self.assertEqual(r_by_name["B"], 5.0)
        self.assertEqual(r_by_name["C"], 6.0)
        self.assertEqual(a["chain_us"], 13.0)
        self.assertFalse(a["schedulable"])
        self.assertTrue(any("chain response" in r for r in a["reasons"]))

    def test_not_schedulable_by_divergence(self):
        stages = [Stage("A", 2, 10), Stage("B", 1, 1)]
        a = analyze(stages, period=10)
        r_by_name = {s.name: r for s, r in a["results"]}
        self.assertEqual(r_by_name["A"], 10.0)
        self.assertIsNone(r_by_name["B"])
        self.assertFalse(a["schedulable"])
        self.assertTrue(any("diverges" in r for r in a["reasons"]))

    def test_builtin_example(self):
        stages, period = rta.builtin_example()
        a = analyze(stages, period)
        self.assertTrue(a["schedulable"])


if __name__ == "__main__":
    unittest.main()