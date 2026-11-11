#!/usr/bin/env python3
"""Relais — tests of the worst-case adversary (INJ-12, spec 12 #1).

The searches are unit-tested against a STUBBED oracle, never against the real
simulator: the objective is a paraboloid whose optimum (the hilberic zero) is
known, so the tests can demand that the optimizer converges to it within an
epsilon, that it respects its evaluation budget, that a seed makes the run
reproducible, and that a counter-example (an invariant violation) is reported
in priority over a better plain score.

Run:
  python3 -m unittest discover -s host/optimize -p 'test_*.py' -v
"""
from __future__ import annotations

import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                       # the module under test lives here
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))  # repo root

import report_worst as report    # noqa: E402
import search as sx              # noqa: E402
import wcspace as wcs            # noqa: E402
import worstcase as wc           # noqa: E402

N = wcs.GENE_COUNT
TARGET = tuple(0.5 for _ in range(N))
SPACE = sx.Space(N)


def paraboloid(genome):
    """A known objective: a paraboloid whose maximum is 0 at TARGET."""
    g = tuple(genome)
    dist = sum((gi - ti) ** 2 for gi, ti in zip(g, TARGET)) / float(N)
    return sx.Candidate(g, fitness=-dist)


def ce_paraboloid(genome):
    """Nearly every genome is a counter-example; only the target is clean."""
    candidate = paraboloid(genome)
    if candidate.fitness > -0.05:
        return candidate
    return sx.Candidate(candidate.genome, fitness=candidate.fitness,
                        counter_example=True)


def run_evo(evaluate, budget=200, seed=7):
    """One evolutionary search against a stub, returning the result."""
    counter = {"n": 0}

    def counted(genome):
        counter["n"] += 1
        return evaluate(genome)

    rng = random.Random(seed)
    space = sx.Space(N)
    return sx.EvoSearch(space, counted, rng, budget=budget, population=16,
                        elite=2).run(), counter["n"]


class SearchTest(unittest.TestCase):
    """The optimizer finds a known optimum, within budget, reproducibly."""

    def test_finds_optimum_within_epsilon(self):
        result, n = run_evo(paraboloid, budget=300, seed=7)
        self.assertIsNotNone(result.best)
        self.assertGreater(result.best.fitness, -0.05,
                           "optimizer missed the known optimum: %s"
                           % result.best.fitness)
        self.assertLessEqual(n, 300)

    def test_budget_is_respected(self):
        _, n = run_evo(paraboloid, budget=123, seed=11)
        self.assertLessEqual(n, 123)

    def test_seed_makes_the_run_reproducible(self):
        a, na = run_evo(paraboloid, budget=150, seed=42)
        b, nb = run_evo(paraboloid, budget=150, seed=42)
        self.assertEqual(na, nb)
        self.assertEqual(a.best.genome, b.best.genome)
        self.assertEqual(a.best.fitness, b.best.fitness)

    def test_counter_example_outranks_better_score(self):
        clean = sx.Candidate(TARGET, fitness=10.0, counter_example=False)
        breach = sx.Candidate(tuple(0.0 for _ in range(N)), fitness=1.0,
                              counter_example=True)
        self.assertTrue(breach.outranks(clean),
                        "a counter-example must be reported before any score")
        self.assertFalse(clean.outranks(breach))

    def test_counter_example_is_kept_while_searching(self):
        result, _ = run_evo(ce_paraboloid, budget=200, seed=7)
        self.assertIsNotNone(result.best)
        self.assertTrue(result.best.counter_example,
                        "the search must keep a violation, not discard it")


class EncodingTest(unittest.TestCase):
    """The genome decodes into the simulator and CONTRACT vocabularies."""

    def test_one_fault_and_two_faults(self):
        gene = [0.2, 0.1, 0.25, 0.5, 0.4] + [0.6, 0.8, 0.75, 0.4, 0.9] + [0.0]
        solo = wcs.decode(gene, ticks=1000, seed=7, level="socle")
        self.assertEqual(len(solo.faults), 1)
        gene[-1] = 0.9
        nested = wcs.decode(gene, ticks=1000, seed=7, level="socle")
        self.assertEqual(len(nested.faults), 2)

    def test_kind_and_target_decode(self):
        gene = [0.5, 0.0, 0.49, 0.0, 0.0] + [0.0] * 5 + [0.0]
        sc = wcs.decode(gene, ticks=2000, seed=7, level="complet")
        fault = sc.faults[0]
        self.assertEqual(fault.kind, wcs.KINDS[0])
        self.assertEqual(fault.target, "A")

    def test_scenario_round_trips_to_contract(self):
        gene = [0.5, 0.1, 0.49, 0.6, 0.4] + [0.0] * 5 + [0.0]
        sc = wcs.decode(gene, ticks=2000, seed=7, level="complet")
        doc = sc.to_dict()
        self.assertIn("--fault", " ".join(sc.cli()))
        for f in doc["faults"]:
            self.assertIn("at_tick", f)
            self.assertIn("type", f)
        self.assertIn("seed", doc)


class ObjectiveTest(unittest.TestCase):
    """The objective turns a run into the right priority order."""

    def test_violation_is_a_counter_example_above_score(self):
        clean = wc.objective(wc.Measurement(rc=0, ticks=1000, max_gap=4,
                            max_deviation=0.5), 1000, 2.0)
        breach = wc.objective(wc.Measurement(rc=1, ticks=1000, violations=1),
                              1000, 2.0)
        self.assertFalse(clean.counter_example)
        self.assertTrue(breach.counter_example)
        self.assertGreater(breach.fitness, clean.fitness)

    def test_summary_line_is_parsed(self):
        line = ("ticks=2000 violations=0 max_gap=61 (bound 4294967295) "
                "max_deviation=0.548508 (bound 0.02) fallback_run=52 "
                "lamp_delay=1 level=complet")
        m = wc.parse_summary(line, 0, 1.0)
        self.assertEqual(m.ticks, 2000)
        self.assertEqual(m.max_gap, 61)
        self.assertAlmostEqual(m.max_deviation, 0.548508, places=6)
        self.assertEqual(m.level, "complet")
        self.assertFalse(m.broken)

    def test_broken_run_scores_worst(self):
        m = wc.Measurement(rc=2, stderr="nope")
        score = wc.objective(m, 2000, 2.0)
        self.assertAlmostEqual(score.fitness, wc.ERROR_FITNESS)


class ReportTest(unittest.TestCase):
    """The formatters turn a result into text and markdown."""

    def test_markdown_is_pasteable(self):
        fake = {"config": {"ticks": 2000, "sim_seed": 7, "levels": ["complet"],
                           "params": "T=5,p=1,N=8"},
                "budget": {"evaluations_per_search": 400, "seeds": 1,
                           "search_seed": 1, "algorithm": "evo", "population": 24,
                           "evaluations_total": 400, "wall_s": 1.2},
                "best": None, "counter_examples": [], "worst_by_kind": {}}
        markdown = report.markdown_report(fake)
        self.assertIn("INJ-12", markdown)
        self.assertIn("| Fault kind |", markdown)
        text = report.text_report(fake)
        self.assertIn("budget:", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)