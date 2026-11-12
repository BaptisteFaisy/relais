#!/usr/bin/env python3
"""test_wcet.py — unit tests of the M40 harness (host/wcet/).

What is pinned down here:
  * the percentiles published by host/wcet come from host/timing/wcet.py (one
    definition, no re-implementation);
  * the stage split of host/wcet/rta.py is exact: C_net + C_core + C_ctl equals
    the measured PATH, and no stage is rounded down to zero;
  * the margins are signed the way the report says (negative = exceeded);
  * the cross-check between the C summary and the Python recomputation detects a
    divergence instead of hiding it;
  * host/wcet/target-report.py classifies an evidence file by its own recorded
    identity and DERIVES its BLOCKED reason from the detection output, so the
    report can never contradict the evidence it prints;
  * no number is invented: an absent measurement yields a BLOCKED marker.

Run:  python3 -m unittest discover -s host/wcet -p 'test_*.py' -v
"""
import importlib.util
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import measure  # noqa: E402
import rta  # noqa: E402
import wcet_lib as W  # noqa: E402


def load_by_path(name, filename):
    """Import a module whose file name is not a Python identifier."""
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


TARGET_REPORT = load_by_path("wcet_target_report", "target-report.py")


def payload_stub(modes):
    """A minimal measure.json: {mode: {segment: descriptor}}."""
    return {"host": "linux-x86_64", "date": "2026-01-01T00:00:00+0100",
            "config": {"samples": 100, "runs": 1, "mac": True},
            "environment": {},
            "modes": {mode: {"runs": [{"status": "OK", "evt_bound_us": None}],
                             "aggregate": {"segments": segments}}
                      for mode, segments in modes.items()}}


def segments(decode=1.0, encode=2.0, node=4.0, law=0.5, path=7.0):
    def desc(value, n=100):
        return {"n": n, "min": value, "mean": value, "stdev": 0.1,
                "p50": value, "p90": value, "p99": value, "p99_9": value,
                "max": value}
    return {"decode": desc(decode), "encode": desc(encode), "node": desc(node),
            "law": desc(law), "path": desc(path)}


class TestReuseOfTiming(unittest.TestCase):
    def test_percentile_is_the_timing_one(self):
        self.assertIs(W.percentile, W.TIMING.percentile)
        self.assertIs(W.gumbel_pot_bound, W.TIMING.gumbel_pot_bound)
        ten = [float(x) for x in range(0, 100, 10)]
        self.assertAlmostEqual(W.percentile(ten, 0.5), 45.0)
        self.assertAlmostEqual(W.percentile(ten, 0.99), 89.1)

    def test_rta_engine_is_the_timing_one(self):
        stages = [W.TIMING_RTA.Stage("s", 10, 100)]
        analysis = W.TIMING_RTA.analyze(stages, 5000)
        self.assertTrue(analysis["schedulable"])
        self.assertEqual(analysis["chain_us"], 100)


class TestStageSplit(unittest.TestCase):
    def test_stages_sum_to_the_measured_path(self):
        costs = rta.stage_costs(segments(), "p99_9")
        self.assertAlmostEqual(costs["relais-net"], 3.0)   # decode + encode
        self.assertAlmostEqual(costs["relais-core"], 3.5)  # node - law
        self.assertAlmostEqual(costs["relais-ctl"], 0.5)   # law
        self.assertAlmostEqual(sum(costs.values()), 7.0)   # = path

    def test_no_stage_rounds_to_zero(self):
        costs = rta.stage_costs(segments(law=0.278), "p99_9")
        stages = rta.build_stages(costs)
        by_name = {s.name: s.wcet_us for s in stages}
        self.assertEqual(by_name["relais-ctl"], 1)  # ceil, never 0
        self.assertTrue(all(v >= 1 for v in by_name.values()))

    def test_worst_stage_over_modes(self):
        stub = payload_stub({"none": segments(node=4.0),
                             "cpu": segments(node=9.0, law=1.0)})
        worst, per_mode = rta.worst_stages(stub, "p99_9")
        self.assertAlmostEqual(worst["relais-core"], 8.0)  # 9 - 1, the worst
        self.assertAlmostEqual(per_mode["none"]["relais-core"], 3.5)
        self.assertEqual(sorted(per_mode), ["cpu", "none"])


class TestMargins(unittest.TestCase):
    def test_margin_sign_and_value(self):
        path = {"p99_9": 16.0, "max": 640.0, "evt_bound_us": 194.0}
        found = rta.margins(path, 8.0, 5000.0, 1000.0, 1000.0)
        self.assertAlmostEqual(found["perf01_p99_9"], 1000.0 - 16.0)
        self.assertAlmostEqual(found["perf01_worst_max"], 2000.0 - 640.0)
        self.assertAlmostEqual(found["perf01_worst_evt"], 2000.0 - 194.0)
        self.assertAlmostEqual(found["rta_chain_vs_period"], 5000.0 - 8.0)
        # 2*1000 + 8 + 1000 = 3008 <= 5000
        self.assertAlmostEqual(found["cycle_rule_2d_c_j"], 5000.0 - 3008.0)

    def test_exceeded_deadline_is_negative(self):
        path = {"p99_9": 1500.0, "max": 3000.0, "evt_bound_us": None}
        found = rta.margins(path, None, 5000.0, 1000.0, 1000.0)
        self.assertLess(found["perf01_p99_9"], 0.0)
        self.assertLess(found["perf01_worst_max"], 0.0)
        self.assertNotIn("perf01_worst_evt", found)
        self.assertNotIn("rta_chain_vs_period", found)

    def test_breaking_point_grows_with_the_cost(self):
        cheap = rta.smallest_period(rta.stage_costs(segments(), "p99_9"))
        dear = rta.smallest_period(
            rta.stage_costs(segments(decode=500.0, encode=500.0, node=900.0,
                                     law=100.0, path=2000.0), "p99_9"))
        self.assertIsNotNone(cheap)
        self.assertIsNotNone(dear)
        self.assertGreater(dear, cheap)


class TestCrossCheck(unittest.TestCase):
    def test_agreement_and_divergence(self):
        recomputed = W.describe([1.0, 2.0, 3.0, 4.0])
        same = dict(recomputed)
        agree, gap = W.cross_check(same, recomputed)
        self.assertTrue(agree)
        self.assertAlmostEqual(gap, 0.0)
        drifted = dict(recomputed)
        drifted["p99_9"] = recomputed["p99_9"] * 1.5
        agree, gap = W.cross_check(drifted, recomputed, tolerance_pct=1.0)
        self.assertFalse(agree)
        self.assertGreater(gap, 1.0)
        truncated = dict(recomputed)
        truncated["n"] = recomputed["n"] - 1
        self.assertFalse(W.cross_check(truncated, recomputed)[0])


class TestAggregation(unittest.TestCase):
    def test_merge_takes_the_worst_tail(self):
        runs = [{"n": 10, "min": 1.0, "mean": 2.0, "stdev": 0.5, "p50": 2.0,
                 "p90": 3.0, "p99": 4.0, "p99_9": 5.0, "max": 6.0},
                {"n": 10, "min": 1.0, "mean": 2.5, "stdev": 0.9, "p50": 2.5,
                 "p90": 3.5, "p99": 4.5, "p99_9": 9.0, "max": 20.0}]
        merged = W.merge_descriptors(runs)
        self.assertEqual(merged["n"], 20)
        self.assertEqual(merged["p99_9"], 9.0)   # worst tail
        self.assertEqual(merged["max"], 20.0)
        self.assertEqual(merged["p50"], 2.25)    # median body
        self.assertEqual(merged["runs"], 2)

    def test_dispersion_between_two_runs(self):
        entry = W.dispersion([10.0, 15.0])
        self.assertAlmostEqual(entry["spread"], 5.0)
        self.assertAlmostEqual(entry["spread_pct"], 40.0)  # 5 / 12.5
        self.assertIsNone(W.dispersion([10.0])["spread"])


class TestVerdict(unittest.TestCase):
    def analysis(self, period=5000.0):
        costs = rta.stage_costs(segments(), "p99_9")
        return W.TIMING_RTA.analyze(rta.build_stages(costs), period)

    def test_exceeded_budget_is_announced(self):
        found = rta.margins({"p99_9": 10.0, "max": 5000.0, "evt_bound_us": None},
                            self.analysis()["chain_us"], 5000.0, 1000.0, 1000.0)
        line = rta.verdict_line(self.analysis(), found)
        self.assertIn("EXCEEDED", line)
        self.assertIn("perf01_worst_max", line)

    def test_all_budgets_met(self):
        found = rta.margins({"p99_9": 10.0, "max": 20.0, "evt_bound_us": 30.0},
                            self.analysis()["chain_us"], 5000.0, 100.0, 100.0)
        self.assertTrue(all(v >= 0 for v in found.values()))
        self.assertIn("every published budget is met",
                      rta.verdict_line(self.analysis(), found))

    def test_unschedulable_chain(self):
        analysis = {"schedulable": False, "reasons": ["chain 9000us > T=5000us"]}
        self.assertIn("NOT TENABLE", rta.verdict_line(analysis, {}))

    def test_inflation_signs_are_never_double(self):
        stub = payload_stub({"none": segments(node=9.0), "cpu": segments(node=4.0)})
        _, per_mode = rta.worst_stages(stub, "p99_9")
        text = "\n".join(rta.inflation_table(per_mode))
        self.assertNotIn("+-", text)
        self.assertIn("core:-5.000 us", text)


class TestMeasureCommand(unittest.TestCase):
    def test_mode_of_a_run_tag(self):
        self.assertEqual(measure.args_mode("journal-run2"), "journal")
        self.assertEqual(measure.args_mode("none-run1"), "none")

    def test_probe_command_carries_every_switch(self):
        args = measure.parse_args(["--samples", "10", "--warmup", "2", "--lock",
                                   "--prio", "61", "--params", "T=1,p=1,N=8",
                                   "--no-mac"])
        with tempfile.TemporaryDirectory() as tmp:
            cmd = measure.probe_command(args, "/bin/probe", "pc", tmp, "none-run1")
        joined = " ".join(cmd)
        for expected in ("--samples 10", "--warmup 2", "--lock", "--prio 61",
                         "--params T=1,p=1,N=8", "--no-mac", "--quiet",
                         "samples-none-run1.jsonl", "summary-none-run1.json"):
            self.assertIn(expected, joined)

    def test_pinning_goes_through_isolate_sh(self):
        cmd = measure.pinned_command(["/bin/probe", "--samples", "1"], 3)
        self.assertTrue(cmd[0].endswith("sh"))
        self.assertIn("isolate.sh", cmd[0 + 1])
        self.assertIn("--cpu", cmd)
        self.assertIn("3", cmd)
        self.assertEqual(cmd[-2], "--")
        self.assertEqual(measure.pinned_command(["/bin/probe"], None), ["/bin/probe"])


class TestTargetReport(unittest.TestCase):
    def test_classify_by_recorded_identity(self):
        self.assertEqual(TARGET_REPORT.classify({"host": "linux-x86_64"}), "pc")
        self.assertEqual(TARGET_REPORT.classify({"host": "qnx800-x86_64"}), "qnx")
        self.assertEqual(TARGET_REPORT.classify(
            {"host": "linux-aarch64",
             "environment": {"cpu_model": "Raspberry Pi 5"}}), "pi")
        self.assertEqual(TARGET_REPORT.classify(
            {"host": "linux-x86_64", "environment": {"os": "QNX"}}), "qnx")

    def test_blocked_reason_is_derived_from_the_evidence(self):
        reason = TARGET_REPORT.blocked_reason(
            "qnx", {"command -v qcc": "(empty) [rc=127]",
                    "ls -d ~/qnx800 /opt/qnx* 2>/dev/null": "/home/x/qnx800",
                    "ls ~/qnx800/host/usr/bin/qcc /opt/qnx*/host/usr/bin/qcc "
                    "2>/dev/null": "(empty) [rc=2]"})
        self.assertIn("no usable QNX toolchain", reason)
        self.assertIn("/home/x/qnx800", reason)   # the evidence is quoted
        self.assertIn("(empty)", reason)

    def test_no_evidence_no_number(self):
        with tempfile.TemporaryDirectory() as tmp:
            found = TARGET_REPORT.collect(tmp)
            self.assertEqual([found[k] for k in ("pc", "pi", "qnx")], [[], [], []])
            text = TARGET_REPORT.render(found, [("uname -srm", "Linux x86_64")], tmp)
            self.assertIn("BLOCKED(", text)
            self.assertIn("no number is published", text)

    def test_an_evidence_file_becomes_measured(self):
        with tempfile.TemporaryDirectory() as tmp:
            host_dir = os.path.join(tmp, "linux-x86_64")
            os.makedirs(host_dir)
            stub = payload_stub({"none": segments(path=7.0)})
            with open(os.path.join(host_dir, "measure.json"), "w") as fh:
                json.dump(stub, fh)
            found = TARGET_REPORT.collect(tmp)
            self.assertEqual(len(found["pc"]), 1)
            lines = TARGET_REPORT.measured_block(found["pc"])
            self.assertTrue(any("p99.9=7.000" in line for line in lines))
            text = TARGET_REPORT.render(found, [], tmp)
            self.assertIn("**MEASURED**", text)
            self.assertNotIn("BLOCKED(no measurement on this machine", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
