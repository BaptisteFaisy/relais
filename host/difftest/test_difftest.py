"""Unit tests for the differential confrontation (M13).

Three properties are checked:
  1. C == Python on a deterministic nominal scenario.
  2. C == Python on 200 short random sequences.
  3. The comparator DETECTS a deliberately injected disagreement (the mandate
     withdrawal rule is broken in the Python mirror), because a comparator that
     never finds a divergence proves nothing.

Run with: python3 -m unittest discover -s host/difftest -p 'test_*.py' -v
"""

import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from difftest import (Bridge, Counterexample, diff_paths, floats_equal,  # noqa: E402
                      nominal_sequence, random_sequence, run_sequence)
from arbiter_py import Arbiter, default_params  # noqa: E402


class BrokenArbiter(Arbiter):
    """MUTANT for the test: never revokes a mandate, breaking MAN-04."""

    def _release_mandate(self, out, tick, kind):
        pass


class TestFloats(unittest.TestCase):
    def test_floats_equal_tolerates_one_ulp(self):
        self.assertTrue(floats_equal(1.0, 1.0 + 1e-9))

    def test_floats_equal_rejects_real_gap(self):
        self.assertFalse(floats_equal(1.0, 1.1))


class TestDiffPaths(unittest.TestCase):
    def test_reports_missing_key(self):
        d = diff_paths({"a": 1}, {"b": 1})
        self.assertTrue(any("missing" in x for x in d))

    def test_reports_length_mismatch(self):
        d = diff_paths([1, 2], [1, 2, 3])
        self.assertTrue(any("length" in x for x in d))

    def test_identical_gives_no_diff(self):
        self.assertEqual(diff_paths({"a": [1, 2]}, {"a": [1, 2]}), [])


class TestAgreement(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Fail early with a clear message if the bridge was not built.
        if not os.path.exists(BRIDGE_PATH):
            raise unittest.SkipTest("bridge binary missing: %s (run make)" % BRIDGE_PATH)

    def test_nominal_agrees(self):
        result = run_sequence(nominal_sequence())
        self.assertIsNone(result, msg=result.dump() if isinstance(result, Counterexample) else "")

    def test_random_agrees(self):
        rng = random.Random(1)
        for i in range(200):
            seq = random_sequence(rng, ticks=15)
            result = run_sequence(seq)
            self.assertIsNone(result,
                              msg="sequence %d diverged:\n%s" % (i, result.dump()
                                  if isinstance(result, Counterexample) else ""))


class TestDetectsInjectedMismatch(unittest.TestCase):
    """A comparator that cannot find a planted bug is worthless."""

    @classmethod
    def setUpClass(cls):
        if not os.path.exists(BRIDGE_PATH):
            raise unittest.SkipTest("bridge binary missing: %s (run make)" % BRIDGE_PATH)

    def test_detects_broken_revoke(self):
        bridge = Bridge(params=default_params())
        broken = BrokenArbiter(default_params())
        try:
            result = run_sequence(nominal_sequence(), bridge=bridge, py=broken)
            self.assertIsNotNone(result,
                                 "the comparator missed a broken mandate rule")
            self.assertIsInstance(result, Counterexample)
            # The divergence must be about the mandate/holder (MAN-04), which is
            # exactly what the broken mutant stops updating.
            self.assertTrue(any("mandate" in d or "holder" in d for d in result.diffs),
                            msg="\n".join(result.diffs))
        finally:
            bridge.close()


# Import after the path is set so Bridge's default path is resolved.
from difftest import BRIDGE as BRIDGE_PATH  # noqa: E402


if __name__ == "__main__":
    unittest.main()