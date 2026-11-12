"""Tests for host/verify/invariant_mining.py (M12).

Three guarantees the mission asks for, on synthetic journals:
  * a relation that truly holds is reported confirmed,
  * a relation that is contradicted is reported candidate (not confirmed),
  * a degenerate corpus (empty, header-only, malformed) never crashes.

Run with:  python3 -m unittest discover -s host/verify -p 'test_invariant*.py'
"""
from __future__ import annotations

import json
import os
import tempfile
import unittest

import invariant_mining as im

PARAMS = {"T": 5, "p": 1, "N": 8, "certify": 3, "bail": 4,
          "eps": 0.005, "delta": 0.02, "degraded": 6000}


def _tick(n, **kw):
    base = {"tick": n, "mandate": kw.pop("mandate", 1), "holder": kw.pop("holder", "A"),
            "source": kw.pop("source", "holder"), "value": kw.pop("value", 0.1),
            "misses": kw.pop("misses", 0), "lamp": kw.pop("lamp", "off"),
            "cause": kw.pop("cause", "none"), "certified": kw.pop("certified", False),
            "certified_node": kw.pop("certified_node", ""), "stop": False}
    base.update(kw)
    return base


def _event(kind, tick, node="", mandate=1, detail=0):
    return {"event": kind, "tick": tick, "node": node, "mandate": mandate,
            "detail": detail, "text": kind}


def _write_journal(dirpath, name, ticks=(), events=(), params=PARAMS, raw_lines=None):
    path = os.path.join(dirpath, name)
    with open(path, "w", encoding="utf-8") as fh:
        if raw_lines is not None:
            fh.write("\n".join(raw_lines) + "\n")
            return path
        fh.write(json.dumps({"kind": "header", "version": 1, "mutant": 0,
                             "scenario": name, "scenario_id": 0, "params": params}) + "\n")
        for e in events:
            fh.write(json.dumps({"kind": "event", **e}) + "\n")
        for t in ticks:
            fh.write(json.dumps({"kind": "tick", **t}) + "\n")
    return path


def _mine_paths(paths):
    journals = im.load_corpus(paths)
    return im.mine(journals), journals


def _by_id(results, inv_id):
    return next(r for r in results if r.id == inv_id)


class TestTrueRelationConfirmed(unittest.TestCase):
    def test_amplitude_and_holder_fresh_confirmed(self):
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(i, value=0.5, source="holder", misses=0, lamp="off")
                     for i in range(1, 11)]
            p = _write_journal(d, "good.json", ticks=ticks)
            results, journals = _mine_paths([p])
            self.assertEqual(len(journals), 1)
            amp = _by_id(results, "AMP-BOUND")
            self.assertEqual(amp.status, "confirmed")
            self.assertEqual(amp.confidence, 1.0)
            self.assertEqual(amp.confirming, 1)
            self.assertEqual(amp.contradicting, 0)
            self.assertGreater(amp.support, 0)
            fresh = _by_id(results, "HOLDER-FRESH")
            self.assertEqual(fresh.status, "confirmed")

    def test_fallback_red_confirmed(self):
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(1, source="fallback", lamp="red", value=0.0, misses=2,
                           holder="")]
            p = _write_journal(d, "fb.json", ticks=ticks)
            results, _ = _mine_paths([p])
            self.assertEqual(_by_id(results, "FALLBACK-RED").status, "confirmed")


class TestFalseRelationRejected(unittest.TestCase):
    def test_fallback_not_red_is_candidate(self):
        # A fallback tick whose lamp is NOT red contradicts FALLBACK-RED.
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(1, source="fallback", lamp="off", value=0.0, holder="")]
            p = _write_journal(d, "bad.json", ticks=ticks)
            results, _ = _mine_paths([p])
            bad = _by_id(results, "FALLBACK-RED")
            self.assertEqual(bad.status, "candidate")
            self.assertLess(bad.confidence, 1.0)
            self.assertEqual(bad.contradicting, 1)
            self.assertEqual(bad.first_counterexample, p)

    def test_amplitude_violation_is_candidate(self):
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(1, value=5.0), _tick(2, value=0.1)]
            p = _write_journal(d, "over.json", ticks=ticks)
            results, _ = _mine_paths([p])
            amp = _by_id(results, "AMP-BOUND")
            self.assertEqual(amp.status, "candidate")
            self.assertEqual(amp.contradicting, 1)


class TestDegenerateCorpus(unittest.TestCase):
    def test_empty_corpus_does_not_crash(self):
        with tempfile.TemporaryDirectory() as d:
            results, journals = _mine_paths([d])  # a directory with no *.json
            self.assertEqual(journals, [])
            self.assertTrue(all(r.status == "vacuous" for r in results))

    def test_header_only_journal_is_vacuous(self):
        with tempfile.TemporaryDirectory() as d:
            p = _write_journal(d, "empty.json", ticks=(), events=())
            results, journals = _mine_paths([p])
            self.assertEqual(len(journals), 1)
            self.assertEqual(journals[0].ticks, [])
            for r in results:
                self.assertEqual(r.status, "vacuous")
                self.assertEqual(r.support, 0)

    def test_malformed_lines_are_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            raw = [json.dumps({"kind": "header", "params": PARAMS}),
                   "{not json at all", "",
                   json.dumps({"kind": "tick", **_tick(1, value=0.2)}),
                   "trailing garbage"]
            p = _write_journal(d, "dirty.json", raw_lines=raw)
            results, journals = _mine_paths([p])
            self.assertEqual(len(journals), 1)
            self.assertGreaterEqual(journals[0].malformed, 2)
            self.assertEqual(len(journals[0].ticks), 1)
            self.assertEqual(_by_id(results, "AMP-BOUND").status, "confirmed")

    def test_no_events_makes_implications_vacuous(self):
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(i) for i in range(1, 5)]
            p = _write_journal(d, "noev.json", ticks=ticks, events=())
            results, _ = _mine_paths([p])
            self.assertEqual(_by_id(results, "AUTOSW-CERT").status, "vacuous")
            self.assertEqual(_by_id(results, "GRANT-VACANT").status, "vacuous")


class TestEventImplications(unittest.TestCase):
    def test_grant_after_vacant_confirmed(self):
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(1, holder="", mandate=0, source="fallback", lamp="red"),
                     _tick(2, holder="A", mandate=1)]
            events = [_event("mandate_granted", 2, node="A", mandate=1)]
            p = _write_journal(d, "grant.json", ticks=ticks, events=events)
            results, _ = _mine_paths([p])
            self.assertEqual(_by_id(results, "GRANT-VACANT").status, "confirmed")

    def test_grant_after_same_tick_revoke_confirmed(self):
        # MAN-05: the mandate was freed by a revoke earlier in the same tick.
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(1, holder="A", mandate=1), _tick(2, holder="B", mandate=2)]
            events = [_event("mandate_revoked", 2, node="A", mandate=1),
                      _event("mandate_granted", 2, node="B", mandate=2)]
            p = _write_journal(d, "sameswitch.json", ticks=ticks, events=events)
            results, _ = _mine_paths([p])
            self.assertEqual(_by_id(results, "GRANT-VACANT").status, "confirmed")

    def test_grant_without_vacancy_is_candidate(self):
        # A grant with no prior vacancy and no same-tick revoke is illegal.
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(1, holder="A", mandate=1), _tick(2, holder="B", mandate=2)]
            events = [_event("mandate_granted", 2, node="B", mandate=2)]
            p = _write_journal(d, "badgrant.json", ticks=ticks, events=events)
            results, _ = _mine_paths([p])
            self.assertEqual(_by_id(results, "GRANT-VACANT").status, "candidate")

    def test_autoswitch_requires_certified(self):
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(1, holder="A", certified=True, certified_node="B"),
                     _tick(2, holder="B", mandate=2)]
            events = [_event("mandate_auto_switch", 2, node="B", mandate=2)]
            p = _write_journal(d, "as.json", ticks=ticks, events=events)
            results, _ = _mine_paths([p])
            self.assertEqual(_by_id(results, "AUTOSW-CERT").status, "confirmed")


class TestReporting(unittest.TestCase):
    def test_json_and_text_reports_do_not_crash(self):
        with tempfile.TemporaryDirectory() as d:
            ticks = [_tick(i, value=0.3) for i in range(1, 6)]
            p = _write_journal(d, "rep.json", ticks=ticks)
            results, journals = _mine_paths([p])
            text = im.format_report(results, len(journals), 0.0)
            self.assertIn("AMP-BOUND", text)
            payload = json.loads(im.to_json(results, len(journals)))
            self.assertEqual(payload["journals"], 1)
            self.assertTrue(any(i["id"] == "AMP-BOUND" for i in payload["invariants"]))


if __name__ == "__main__":
    unittest.main()
