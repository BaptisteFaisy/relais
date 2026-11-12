#!/usr/bin/env python3
"""Unit tests for the nightly probe generators and the report assembler.

These tests are fast and deterministic: they only enumerate the run lists and
render the markdown, they never launch the C binaries.
"""
import argparse
import unittest

import nightly_probe
import report


class ProbeTests(unittest.TestCase):
    def _args(self, **kw):
        d = dict(sweep_from=100, sweep_step=7, sweep_count=1986, durations=2500,
                 shard=0, shards=4)
        d.update(kw)
        return argparse.Namespace(**d)

    def test_death_run_count(self):
        runs = nightly_probe.death_runs(self._args())
        self.assertEqual(len(runs), 2 * 1986)

    def test_freeze_run_count(self):
        runs = nightly_probe.freeze_runs(self._args())
        self.assertEqual(len(runs), 4 * 2500)

    def test_cut_run_count(self):
        ns = self._args(sweep_count=420)
        runs = nightly_probe.cut_runs(ns)
        self.assertEqual(len(runs), 6 * 2 * 420)

    def test_slice_partitions(self):
        runs = nightly_probe.death_runs(self._args())
        parts = [nightly_probe.slice_runs(runs, i, 4) for i in range(4)]
        self.assertEqual(sum(len(p) for p in parts), len(runs))

    def test_campaign_fault_spec_is_parseable(self):
        # every generated fault spec must carry at:kind:node[:arg] fields
        for argv in nightly_probe.cut_runs(self._args(sweep_count=5)):
            spec = argv[argv.index("--fault") + 1]
            parts = spec.split(":")
            self.assertGreaterEqual(len(parts), 3)


class ReportTests(unittest.TestCase):
    def test_render_pass_and_fail(self):
        artifacts = [
            {"scale": "unit", "checks": 5651, "violations": 0, "duration": 5.0},
            {"scale": "mutation-score", "violations": 0, "score_percent": 0.85,
             "survived": 3},
        ]
        text, verdict = report.render(artifacts, "(test)", "relais")
        self.assertEqual(verdict, "PASS")
        self.assertIn("**Verdict: PASS**", text)
        artifacts.append({"scale": "explore", "violations": 1, "duration": 1.0})
        _text, verdict = report.render(artifacts, "(test)", "relais")
        self.assertEqual(verdict, "FAIL")


if __name__ == "__main__":
    unittest.main()