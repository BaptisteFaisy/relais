"""Tests for the deterministic replay (idea 42, OBS-07).

The replay of a nominal run and of a run with a handover must be identical; a
journal deliberately amputated of one field must be flagged divergent (otherwise
the test proves nothing). The simulator engine is exercised only when
build/relais-simulate is present.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
for _inc in (HERE, os.path.join(HERE, "..", "report")):
    if _inc not in sys.path:
        sys.path.insert(0, _inc)

from journal_io import read_journal  # noqa: E402
import replay  # noqa: E402
from replay import (intrinsic_verdict, derive_misses_and_mandate, run_simulator,
                    compare_journals, _ROOT)  # noqa: E402

SIM = os.path.join(_ROOT, "build", "relais-simulate")
HAVE_SIM = os.path.isfile(SIM)


def _tick(tick, holder="", mandate=0, source="none", misses=0, lamp="off",
          cause="none", value=0.0, pos=0.0):
    return ('{"kind":"tick","tick":%d,"ns":0,"mandate":%d,"holder":"%s",'
            '"source":"%s","value":%g,"pos":%g,"vel":0,"setpoint":0,'
            '"misses":%d,"lamp":"%s","cause":"%s","certified":false,'
            '"certified_node":"","stop":false}\n' % (
                tick, mandate, holder, source, value, pos, misses, lamp, cause))


def _header():
    return ('{"kind":"header","version":1,"mutant":0,"scenario":"t","scenario_id":0,'
            '"params":{"T":5,"p":1,"N":8,"certify":3,"bail":4,"eps":0.005,'
            '"delta":0.02,"degraded":6000}}\n')


def _nominal_lines(n=50):
    lines = [_header(), _tick(1, holder="", mandate=0, source="fallback",
                              misses=1, lamp="red", cause="fallback")]
    for t in range(2, n + 1):
        lines.append(_tick(t, holder="A", mandate=1, source="holder"))
    return lines


def _death_lines(n=110):
    lines = [_header(), _tick(1, holder="", mandate=0, source="fallback",
                              misses=1, lamp="red", cause="fallback")]
    for t in range(2, 101):
        lines.append(_tick(t, holder="A", mandate=1, source="holder"))
    lines.append(_tick(101, holder="A", mandate=1, source="testament", misses=1,
                       lamp="yellow", cause="node_missing"))
    for t in range(102, n + 1):
        lines.append(_tick(t, holder="B", mandate=2, source="holder",
                           lamp="yellow", cause="second_uncertified"))
    return lines


def _write(lines):
    fd, path = tempfile.mkstemp(suffix=".jsonl")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.writelines(lines)
    return path


def _amputate(lines, field):
    pat = {
        "misses": re.compile(r'"misses":\d+,?'),
        "mandate": re.compile(r'"mandate":\d+,?'),
        "source": re.compile(r'"source":"[a-z]+",?'),
    }[field]
    return [pat.sub("", ln) if ln.lstrip().startswith('{"kind":"tick"') else ln
            for ln in lines]


def _fix_commas(lines):
    # after a field is removed, tidy any ",," or trailing "," before "}"
    return [re.sub(r',+(?=\s*})', '', ln).replace(",,", ",") for ln in lines]


class TestIntrinsicReplay(unittest.TestCase):
    def test_nominal_identical(self):
        p = _write(_nominal_lines())
        act = read_journal(p)
        verdict, where = intrinsic_verdict(act.ticks)
        self.assertEqual(verdict, "identical", where)
        os.unlink(p)

    def test_death_handover_identical(self):
        p = _write(_death_lines())
        act = read_journal(p)
        verdict, where = intrinsic_verdict(act.ticks)
        self.assertEqual(verdict, "identical", where)
        # sanity: the derived mandate starts vacant (0), steps to A (1), then B (2).
        derived = derive_misses_and_mandate(act.ticks)
        mandates = sorted({m for _, m in derived})
        self.assertEqual(mandates, [0, 1, 2])
        os.unlink(p)

    def test_amputated_misses_divergent(self):
        lines = _fix_commas(_amputate(_death_lines(), "misses"))
        p = _write(lines)
        act = read_journal(p)
        verdict, where = intrinsic_verdict(act.ticks)
        self.assertEqual(verdict, "divergent")
        self.assertIn("misses", where)
        os.unlink(p)

    def test_amputated_mandate_divergent(self):
        lines = _fix_commas(_amputate(_nominal_lines(), "mandate"))
        p = _write(lines)
        act = read_journal(p)
        verdict, where = intrinsic_verdict(act.ticks)
        self.assertEqual(verdict, "divergent")
        self.assertIn("mandate", where)
        os.unlink(p)


@unittest.skipUnless(HAVE_SIM, "build/relais-simulate not built")
class TestSimulatorReplay(unittest.TestCase):
    def _gen(self, ticks, seed, level="socle", faults=()):
        fd, path = tempfile.mkstemp(suffix=".jsonl")
        os.close(fd)
        cmd = [SIM, "--ticks", str(ticks), "--seed", str(seed), "--level", level,
               "--quiet", "--journal", path]
        for f in faults:
            cmd += ["--fault", f]
        subprocess.run(cmd, check=True, cwd=_ROOT,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        return path

    def test_nominal_simulator_identical(self):
        path = self._gen(200, 11)
        act = read_journal(path)
        rep = read_journal(run_simulator(act, 11, "socle", []))
        verdict, where = compare_journals(act, rep)
        self.assertEqual(verdict, "identical", where)
        os.unlink(path)

    def test_death_simulator_identical(self):
        path = self._gen(600, 3, "complet", ["500:death:0"])
        act = read_journal(path)
        rep = read_journal(run_simulator(act, 3, "complet", ["500:death:0"]))
        verdict, where = compare_journals(act, rep)
        self.assertEqual(verdict, "identical", where)
        os.unlink(path)

    def test_amputated_source_divergent(self):
        path = self._gen(200, 5)
        act = read_journal(path)
        # amputate the recorded `source` field, then replay against a fresh run.
        with open(path, encoding="utf-8") as f:
            raw = f.readlines()
        amputated = _fix_commas(_amputate(raw, "source"))
        apath = path + ".amputated.jsonl"
        with open(apath, "w", encoding="utf-8") as f:
            f.writelines(amputated)
        amp = read_journal(apath)
        rep = read_journal(run_simulator(act, 5, "socle", []))
        verdict, where = compare_journals(amp, rep)
        self.assertEqual(verdict, "divergent")
        self.assertIn("source", where)
        os.unlink(path)
        os.unlink(apath)


if __name__ == "__main__":
    unittest.main(verbosity=2)