#!/usr/bin/env python3
"""Unit tests of the soak pilot and of the anomaly hunter (mission M42).

The hunter is only worth something if it really finds anomalies: these tests
feed it hand-made journals and series that carry one defect each, and check
that the right rule fires. They never launch the C binaries, so they stay
fast enough for the CI budget.
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import anomaly  # noqa: E402
import profiles  # noqa: E402
import soak      # noqa: E402

HEADER = {"kind": "header", "version": 1, "mutant": 0, "scenario": "soak",
          "scenario_id": 0, "params": {"T": 5, "p": 1, "N": 8, "certify": 3,
                                       "bail": 4, "eps": 0.005, "delta": 0.02,
                                       "degraded": 6000}}


def tick(number, mandate=1, holder="A", source="holder", misses=0, value=0.5):
    """One tick record of the journal format."""
    return {"kind": "tick", "tick": number, "ns": 0, "mandate": mandate,
            "holder": holder, "source": source, "value": value, "pos": 0.0,
            "vel": 0.0, "setpoint": 0.0, "misses": misses, "lamp": "off",
            "cause": "none", "certified": True, "certified_node": "B",
            "stop": False}


def event(number, name="mandate_granted", node="A", mandate=1):
    """One event record of the journal format."""
    return {"kind": "event", "tick": number, "ns": 0, "event": name,
            "node": node, "mandate": mandate, "detail": 0, "text": name}


def write_journal(folder, records):
    """Write records as a journal and return its path."""
    path = os.path.join(folder, "journal.jsonl")
    with open(path, "w", encoding="utf-8") as fh:
        for record in records:
            fh.write(json.dumps(record) + "\n")
    return path


def clean_profile():
    """The profile of a nominal run: no fault, lossless, verifiable."""
    return {"name": "nominal", "faults": [], "fault_count": 0, "lossy": False,
            "bias": False, "fault_free": True, "double_fault": False,
            "verify": False, "verify_bin": profiles.VERIFY}


class JournalRules(unittest.TestCase):
    """The structural rules of the hunter (A04 to A08, A12)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="relais-soak-test-")
        self.profile = clean_profile()

    def rules(self, records):
        """Scan a journal and return the set of rules that fired."""
        path = write_journal(self.tmp, records)
        findings, _stats = anomaly.scan_journal(path, self.profile, "socle")
        return {f["rule"] for f in findings}

    def test_clean_journal_has_no_finding(self):
        records = [HEADER, event(1)] + [tick(i) for i in range(1, 20)]
        self.assertEqual(self.rules(records), set())

    def test_two_values_on_one_tick_is_found(self):
        records = [HEADER] + [tick(i) for i in range(1, 10)]
        records.append(tick(9))
        self.assertIn("A04", self.rules(records))

    def test_stale_mandate_is_found(self):
        records = [HEADER] + [tick(i, mandate=2) for i in range(1, 5)]
        records.append(tick(5, mandate=1))
        self.assertIn("A05", self.rules(records))

    def test_silence_past_p_plus_one_is_found(self):
        records = [HEADER] + [tick(i) for i in range(1, 5)]
        records.append(tick(5, misses=3))
        self.assertIn("A06", self.rules(records))

    def test_fresh_order_counted_as_missing_is_found(self):
        records = [HEADER] + [tick(i) for i in range(1, 4)]
        records.append(tick(4, misses=1))
        self.assertIn("A07", self.rules(records))

    def test_non_monotonic_tick_is_found(self):
        records = [HEADER] + [tick(i) for i in range(1, 5)]
        records.append(tick(3))
        self.assertIn("A08", self.rules(records))

    def test_malformed_line_is_found(self):
        path = write_journal(self.tmp, [HEADER, tick(1)])
        with open(path, "a", encoding="utf-8") as fh:
            fh.write("{not json}\n")
        findings, _stats = anomaly.scan_journal(path, self.profile, "socle")
        self.assertIn("A12", {f["rule"] for f in findings})


class TrendRules(unittest.TestCase):
    """The trend rules of the hunter (A09, A10, A11)."""

    def test_latency_drift_is_found(self):
        self.assertIsNone(anomaly.check_latency(100.0, 100.0, 4.0))
        self.assertIsNotNone(anomaly.check_latency(500.0, 100.0, 4.0))

    def test_memory_growth_is_found(self):
        flat = [2048] * 12
        self.assertIsNone(anomaly.check_rss(flat, 12, 1024))
        growing = [2048 + 100 * i for i in range(12)]
        self.assertIsNotNone(anomaly.check_rss(growing, 12, 1024))
        noisy = [2048, 3000, 2048, 3000] + [2048] * 8
        self.assertIsNone(anomaly.check_rss(noisy, 12, 1024))

    def test_broken_digest_chain_is_found(self):
        with tempfile.TemporaryDirectory(prefix="relais-soak-test-") as tmp:
            path = os.path.join(tmp, "soak.jsonl")
            digest = ""
            for seq in range(3):
                payload = {"seq": seq, "ts": 1.0 + seq}
                digest = anomaly.chain_digest(digest, payload)
                with open(path, "a", encoding="utf-8") as fh:
                    fh.write(json.dumps(dict(payload, digest=digest)) + "\n")
            self.assertIsNone(anomaly.verify_digest_chain(path))
            with open(path, "a", encoding="utf-8") as fh:
                fh.write(json.dumps({"seq": 3, "ts": 9.0, "digest": "0" * 64}) + "\n")
            self.assertIsNotNone(anomaly.verify_digest_chain(path))


class PilotRules(unittest.TestCase):
    """The pilot: profile list, machine guard, command lines."""

    def test_every_profile_declares_a_level(self):
        for profile in profiles.PROFILES:
            self.assertTrue(profile["levels"])
            self.assertIn("socle", profile["levels"])

    def test_machine_guard_shortens_a_busy_machine(self):
        minutes, reason = soak.effective_minutes(30.0, 83.0, 13.0)
        self.assertEqual(minutes, 20.0)
        self.assertIn("busy", reason)

    def test_machine_guard_keeps_a_free_machine(self):
        minutes, reason = soak.effective_minutes(30.0, 2.0, 13.0)
        self.assertEqual(minutes, 30.0)
        self.assertIn("free", reason)

    def test_build_run_writes_a_replayable_command(self):
        profile = profiles.PROFILES[1]
        spec = profiles.build_run(profile, 42, 1000, "socle", "/tmp", 1, soak.SIM)
        self.assertIn("--ticks", spec["argv"])
        self.assertIn("--seed", spec["argv"])
        self.assertIn("--journal", spec["argv"])
        self.assertEqual(spec["flags"]["fault_count"], 1)

    def test_losty_profile_is_not_verified_offline(self):
        lossy = [p for p in profiles.PROFILES if p["name"] == "lossy"][0]
        spec = profiles.build_run(lossy, 7, 1000, "socle", "/tmp", 1, soak.SIM)
        self.assertFalse(spec["flags"]["verify"])


if __name__ == "__main__":
    unittest.main()
