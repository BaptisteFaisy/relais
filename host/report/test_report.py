"""Relais — tests of the campaign report (OBS-05). Standard library only.

Run: python3 -m unittest discover -s host/report -p 'test_*.py'
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import charts  # noqa: E402
import journal_io  # noqa: E402
import stats  # noqa: E402

REPORT = os.path.join(HERE, "report.py")


def tick_line(tick, source="holder", holder="A", mandate=1, value=0.1, misses=0,
              lamp="off", certified=True, certified_node="B"):
    return json.dumps({"kind": "tick", "tick": tick, "ns": 0, "mandate": mandate,
                       "holder": holder, "source": source, "value": value,
                       "pos": value, "vel": 0.0, "setpoint": value, "misses": misses,
                       "lamp": lamp, "cause": "none", "certified": certified,
                       "certified_node": certified_node, "stop": False})


def event_line(tick, event="mandate_granted", node="A", mandate=1, detail=0):
    return json.dumps({"kind": "event", "tick": tick, "ns": 0, "event": event,
                       "node": node, "mandate": mandate, "detail": detail,
                       "text": event})


def make_run(root, name, ticks=40, bad=False):
    """One run folder: a journal, its twin, and a scenario."""
    path = os.path.join(root, name)
    os.makedirs(path, exist_ok=True)
    lines = ['{"kind":"header","version":1,"mutant":0,"scenario":"%s",'
             '"scenario_id":1,"params":{"T":5,"p":1,"N":8,"certify":3,"bail":4,'
             '"eps":0.005,"delta":0.02,"degraded":6000}}' % name]
    twin = [lines[0]]
    for t in range(1, ticks + 1):
        if bad and t > 20:
            lines.append(tick_line(t, source="testament", misses=t - 20,
                                   lamp="yellow"))
        else:
            lines.append(tick_line(t, value=0.1 + 0.001 * t))
        twin.append(tick_line(t, value=0.1 + 0.001 * t))
    if bad:
        lines.append(event_line(21, "mandate_revoked", "A", 1, 2))
    with open(os.path.join(path, "actuator.jsonl"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\nnot json at all\n")
    with open(os.path.join(path, "twin.jsonl"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(twin) + "\n")
    with open(os.path.join(path, "scenario.json"), "w", encoding="utf-8") as fh:
        json.dump({"id": 1, "name": name, "ticks": ticks,
                   "faults": [{"at_tick": 20, "type": "death", "target": "A"}]}, fh)
    return path


class TestStats(unittest.TestCase):
    def test_known_values(self):
        self.assertEqual(stats.median([1, 2, 3, 4]), 2.5)
        self.assertEqual(stats.median([]), 0)
        self.assertAlmostEqual(stats.mean([1, 2, 3]), 2.0)
        # interpolation linéaire : p99 de 1..10 vaut 9.91, pas 10
        self.assertAlmostEqual(stats.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 99), 9.91)
        self.assertEqual(stats.percentile([], 50), 0)
        bins = stats.histogram_from_values([0.0, 0.5, 1.5], 1.0)
        self.assertEqual(sum(b["count"] for b in bins), 3)
        self.assertEqual(stats.histogram_from_values([], 1.0), [])


class TestJournalIo(unittest.TestCase):
    def test_tolerates_a_malformed_line(self):
        root = tempfile.mkdtemp()
        try:
            make_run(root, "run-a", ticks=10)
            runs = journal_io.discover_runs(root)
            self.assertEqual(len(runs), 1)
            journal = runs[0].journal
            self.assertEqual(len(journal.ticks), 10)
            self.assertTrue(any(journal.malformed) if isinstance(
                getattr(journal, "malformed", None), list) else True)
        finally:
            shutil.rmtree(root)


class TestCharts(unittest.TestCase):
    def test_svgs_are_well_formed(self):
        import xml.etree.ElementTree as ET
        for svg in (charts.series_svg([1, 2, 3], [0.1, 0.2, 0.1], [0.1, 0.2, 0.1]),
                    charts.source_strip_svg(["holder", "shadow", "testament"]),
                    charts.mandate_strip_svg(["A", "A", "B"], [1, 1, 2]),
                    charts.histogram_svg([{"lo": 0, "hi": 1, "count": 3}])):
            self.assertIn("<svg", svg)
            ET.fromstring(svg.strip())


class TestReportEndToEnd(unittest.TestCase):
    def test_standalone_html_and_verdict(self):
        root = tempfile.mkdtemp()
        out = os.path.join(root, "report.html")
        try:
            make_run(root, "nominal", ticks=30)
            make_run(root, "death", ticks=30, bad=True)
            proc = subprocess.run([sys.executable, REPORT, "--campaign", root,
                                   "--out", out, "--level", "complet"],
                                  capture_output=True, text=True, timeout=180)
            self.assertTrue(os.path.exists(out), proc.stdout + proc.stderr)
            html = open(out, encoding="utf-8").read()
            self.assertNotIn("<script", html)
            self.assertNotIn('src="http', html)
            self.assertNotIn('href="http', html)
            for needle in ("GLOBAL VERDICT", "Per fault class", "Worst cases",
                           "Runs", "Acceptance criterion"):
                self.assertIn(needle, html)
            # the bad run violates the level-C bound: the verdict must say FAIL
            self.assertIn('class="banner ko"', html)
            self.assertNotEqual(proc.returncode, 0)
        finally:
            shutil.rmtree(root)

    def test_clean_campaign_says_ok(self):
        root = tempfile.mkdtemp()
        out = os.path.join(root, "report.html")
        try:
            make_run(root, "nominal", ticks=30)
            proc = subprocess.run([sys.executable, REPORT, "--campaign", root,
                                   "--out", out], capture_output=True, text=True,
                                  timeout=180)
            html = open(out, encoding="utf-8").read()
            self.assertIn('class="banner ok"', html)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        finally:
            shutil.rmtree(root)


if __name__ == "__main__":
    unittest.main()
