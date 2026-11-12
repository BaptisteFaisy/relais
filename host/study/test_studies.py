#!/usr/bin/env python3
"""Relais — tests of the M31 study tools (ideas 61..71).

Three families, per the mission:
  1. the time-series reader (CSV load, resampling, synthetic generators);
  2. the physics mirrors and the dataset world, validated TICK BY TICK against
     the C simulator and the C arbiter (the authority);
  3. the study grids on known cases: the sensitivity point of the defaults is
     the published one, and the testament ranking is reproducible at fixed
     seed.

Run:
  python3 -m unittest discover -s host/study -p 'test_*.py' -v
"""
from __future__ import annotations

import math
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for _inc in (HERE, os.path.join(ROOT, "host", "physics"),
             os.path.join(ROOT, "host", "report")):
    if _inc not in sys.path:
        sys.path.insert(0, _inc)

import dataset_replay as dr        # noqa: E402
import plant_law as pl             # noqa: E402
import sensitivity as sens         # noqa: E402
import study_common as sc          # noqa: E402
import testament_opt as to         # noqa: E402
from dataset_world import DatasetWorld          # noqa: E402
from journal_io import read_journal             # noqa: E402

SIM = os.path.join(ROOT, "build", "relais-simulate")
BRIDGE = os.path.join(ROOT, "build", "relais-bridge")
HAVE_SIM = os.path.isfile(SIM)
HAVE_BRIDGE = os.path.isfile(BRIDGE)


class TestDatasetReader(unittest.TestCase):
    """The time-series reader: load, validate, resample, generate."""

    def test_load_valid(self):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as fh:
            fh.write("time_s,angle\n0.0,0.1\n0.005,0.2\n0.010,-0.3\n")
            path = fh.name
        try:
            times, angles = dr.load_csv(path)
            self.assertEqual(times, [0.0, 0.005, 0.010])
            self.assertEqual(angles, [0.1, 0.2, -0.3])
        finally:
            os.unlink(path)

    def test_load_rejects_disorder(self):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as fh:
            fh.write("0.0,0.1\n0.010,0.2\n0.005,-0.3\n")
            path = fh.name
        try:
            with self.assertRaises(ValueError):
                dr.load_csv(path)
        finally:
            os.unlink(path)

    def test_load_rejects_short(self):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as fh:
            fh.write("time_s,angle\n0.0,0.1\n")
            path = fh.name
        try:
            with self.assertRaises(ValueError):
                dr.load_csv(path)
        finally:
            os.unlink(path)

    def test_resample_interpolates_and_holds(self):
        times = [0.0, 1.0]
        angles = [0.0, 1.0]
        # ticks at t = (tick-1)*0.5 -> 0, 0.5, 1.0, then hold past the end
        got = dr.resample(times, angles, 4, 0.5)
        self.assertAlmostEqual(got[0], 0.0)
        self.assertAlmostEqual(got[1], 0.5)
        self.assertAlmostEqual(got[2], 1.0)
        self.assertAlmostEqual(got[3], 1.0)  # held past the end

    def test_generators_deterministic_and_bounded(self):
        import random
        n = int(dr.DURATION_S / dr.SAMPLE_DT) + 1
        for name, gen in dr.GENERATORS.items():
            a = gen(random.Random(dr.SEED), n)
            b = gen(random.Random(dr.SEED), n)
            self.assertEqual(a, b, "%s is not reproducible" % name)
            self.assertEqual(len(a), n)
            self.assertLess(max(abs(v) for v in a), 1.05,
                            "%s leaves the normalised range" % name)


@unittest.skipUnless(HAVE_SIM, "build/relais-simulate missing")
class TestPhysicsMirror(unittest.TestCase):
    """The float32 mirror reproduces the C journal tick by tick."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="m31-mirror-")
        cls.journal_path = os.path.join(cls.tmp, "actuator.jsonl")
        subprocess.run([SIM, "--ticks", "400", "--seed", "5",
                        "--trajectory", "kind=slalom amp=0.6 freq=0.5",
                        "--journal", cls.journal_path, "--quiet"], check=True)
        cls.journal = read_journal(cls.journal_path)
        cls.params = pl.default_params()
        cls.dt = pl.step_s(cls.params)

    def test_plant_chain(self):
        worst = 0.0
        for a, b in zip(self.journal.ticks, self.journal.ticks[1:]):
            p = pl.Plant(a.pos)
            p.vel = a.vel
            dist = pl.traj_disturbance(0.05, 2.0, 7, b.tick * self.dt)
            p.step(b.value, dist, self.dt)
            worst = max(worst, abs(p.pos - b.pos), abs(p.vel - b.vel))
        self.assertLess(worst, 5e-4, "plant mirror drifts from the C journal")

    def test_law_chain(self):
        state = pl.CtlState()
        checked = 0
        worst = 0.0
        for t in self.journal.ticks:
            v = state.step({"pos": t.pos, "vel": t.vel,
                            "setpoint": t.setpoint, "applied": t.value},
                           self.params)
            nxt = t.tick + 1
            if nxt <= len(self.journal.ticks):
                row = self.journal.ticks[nxt - 1]
                if row.source == "holder":
                    checked += 1
                    worst = max(worst, abs(v - row.value))
        self.assertGreater(checked, 300)
        self.assertLess(worst, 2e-3, "law mirror drifts from the C orders")


@unittest.skipUnless(HAVE_SIM and HAVE_BRIDGE, "needs simulate + bridge")
class TestDatasetWorld(unittest.TestCase):
    """The Python world equals the C simulator, tick by tick, event by event."""

    TICKS = 250

    def _compare(self, level, fault=None):
        dt = 0.005
        sp = lambda t: 0.6 * math.sin(2 * math.pi * 0.5 * (t - 1) * dt)
        dist = lambda t: pl.traj_disturbance(0.05, 2.0, 7, t * dt)
        tmp = tempfile.mkdtemp(prefix="m31-world-")
        c_journal = os.path.join(tmp, "c.jsonl")
        cmd = [SIM, "--ticks", str(self.TICKS), "--seed", "9",
               "--trajectory", "kind=slalom amp=0.6 freq=0.5",
               "--journal", c_journal, "--quiet"]
        if level == "complet":
            cmd += ["--level", "complet"]
        if fault:
            cmd += ["--fault", fault]
        subprocess.run(cmd, check=True)
        cj = read_journal(c_journal)
        world = DatasetWorld(complete=(level == "complet"))
        if fault:
            at, kind, node = fault.split(":")
            world.add_fault(int(at), kind, int(node))
        world.run(self.TICKS, sp, dist)
        world.close()
        pj = read_journal_rows(world.rows)
        self.assertEqual(len(cj.ticks), len(pj.ticks))
        for a, b in zip(cj.ticks, pj.ticks):
            self.assertEqual(a.source, b.source, "source at tick %d" % a.tick)
            self.assertEqual(a.holder, b.holder, "holder at tick %d" % a.tick)
            self.assertEqual(a.lamp, b.lamp, "lamp at tick %d" % a.tick)
            self.assertLess(abs(a.value - b.value), 1e-3)
            self.assertLess(abs(a.pos - b.pos), 1e-3)
        ce = [(e.tick, e.event, e.node) for e in cj.events]
        pe = [(e.tick, e.event, e.node) for e in pj.events]
        self.assertEqual(ce, pe, "event streams diverge")

    def test_socle_nominal(self):
        self._compare("socle")

    def test_complet_nominal(self):
        self._compare("complet")

    def test_socle_death(self):
        self._compare("socle", "150:death:0")

    def test_complet_death(self):
        self._compare("complet", "150:death:0")


def read_journal_rows(rows):
    """Wrap world rows into a Journal-shaped object (reuse of journal_io)."""
    import io
    import json
    buf = io.StringIO("".join(json.dumps(r) + "\n" for r in rows))
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as fh:
        fh.write(buf.getvalue())
        path = fh.name
    try:
        return read_journal(path)
    finally:
        os.unlink(path)


@unittest.skipUnless(HAVE_SIM, "build/relais-simulate missing")
class TestSensitivityKnownCase(unittest.TestCase):
    """The sensitivity grid reproduces the published default point."""

    def test_default_death_socle(self):
        tmp = tempfile.mkdtemp(prefix="m31-sens-")
        row = sens.measure(tmp, "test-base", dict(sens.BASE), "death_socle")
        self.assertEqual(row["rc"], 0)
        self.assertEqual(row["violations"], 0)
        self.assertEqual(row["max_gap"], 1)          # p=1: one stale tick max
        self.assertEqual(row["handover_delay"], 2)   # two ticks to the grant
        self.assertEqual(row["handover_delay_ms"], 10.0)

    def test_patience_three_widens_the_gap(self):
        tmp = tempfile.mkdtemp(prefix="m31-sens-")
        row = sens.measure(tmp, "test-p3", dict(sens.BASE, p=3), "death_socle")
        self.assertEqual(row["rc"], 0)
        self.assertEqual(row["max_gap"], 3)          # p=3: three stale ticks
        self.assertEqual(row["handover_delay"], 4)


@unittest.skipUnless(HAVE_SIM, "build/relais-simulate missing")
class TestTestamentRanking(unittest.TestCase):
    """The ranking of the will strategies is reproducible at fixed seed."""

    def _one_window(self, tag):
        tmp = tempfile.mkdtemp(prefix="m31-test-")
        return to.study_run(tmp, "slalom", 300, 6901, "socle")

    def test_ranking_reproducible(self):
        r1 = self._one_window("a")
        r2 = self._one_window("b")
        self.assertEqual(r1["t0"], r2["t0"])
        for name in pl.STRATEGIES:
            self.assertEqual(r1[name]["dev_max"], r2[name]["dev_max"])
            self.assertEqual(r1[name]["dev_rms"], r2[name]["dev_rms"])
        rank1 = to.ranking([r1])
        rank2 = to.ranking([r2])
        self.assertEqual([x["strategy"] for x in rank1],
                         [x["strategy"] for x in rank2])

    def test_mirror_fidelity_full_horizon(self):
        """The double-death run plays the will over its full N ticks, and the
        mirror of the current strategy must reproduce every one of them."""
        tmp = tempfile.mkdtemp(prefix="m31-fid-")
        fr = to.fidelity_run(tmp, 6901)
        self.assertEqual(fr["rc"], 0)
        self.assertEqual(fr["horizon_played"], to.PARAMS["N"])
        self.assertLessEqual(fr["fidelity_max"], to.FIDELITY_TOL,
                             "the mirror does not reproduce the will the C "
                             "arbiter played over the full horizon")

    def test_five_strategies_scored(self):
        r = self._one_window("all")
        for name in pl.STRATEGIES:
            self.assertIsNotNone(r[name]["dev_max"])
            self.assertEqual(r[name]["n"], 8)  # N = 8 ticks per window


if __name__ == "__main__":
    unittest.main(verbosity=2)
