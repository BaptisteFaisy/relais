#!/usr/bin/env python3
"""Tests ciblés de quota.py — bibliothèque standard uniquement.

Lance : python3 -m unittest test_quota -v
(depuis .github/fireworks/).
"""
import os
import shutil
import tempfile
import time
import unittest
from unittest import mock

import quota


class SaisinesParPrTest(unittest.TestCase):
    """Une PR ne peut pas consommer tout le plafond horaire à elle seule."""

    def test_compte_par_pr_et_par_fenetre(self):
        home = tempfile.mkdtemp(prefix="fw-home-")
        self.addCleanup(shutil.rmtree, home, True)
        with mock.patch.dict(os.environ, {"HOME": home}):
            cache = quota.cache_dir()
            os.makedirs(cache, exist_ok=True)
            for pr, sha in (("215", "a" * 12), ("215", "b" * 12),
                            ("215", "c" * 12), ("250", "d" * 12)):
                with open(os.path.join(cache, "pr-%s-%s.stamp" % (pr, sha)),
                          "w") as fh:
                    fh.write("0")
            vieux = os.path.join(cache, "pr-215-vieux.stamp")
            with open(vieux, "w") as fh:
                fh.write("0")
            os.utime(vieux, (time.time() - 7_200,) * 2)
            maintenant = time.time()
            self.assertEqual(quota.saisines_pr("215", maintenant), 3)
            self.assertEqual(quota.saisines_pr("250", maintenant), 1)

    def test_plafond_par_pr_leve_skip(self):
        home = tempfile.mkdtemp(prefix="fw-home-")
        self.addCleanup(shutil.rmtree, home, True)
        with mock.patch.dict(os.environ, {"HOME": home}):
            cache = quota.cache_dir()
            os.makedirs(cache, exist_ok=True)
            for i in range(quota.SAISINES_PR_MAX):
                with open(os.path.join(cache, "pr-215-%012d.stamp" % i),
                          "w") as fh:
                    fh.write("0")
            with self.assertRaises(quota.Saisine):
                quota.autoriser("215", "e" * 40)


if __name__ == "__main__":
    unittest.main()
