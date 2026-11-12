"""Relais — the report side of the chained journal (idea 34, OBS-07).

The C core owns the chain: core/src/journal.c computes the fingerprints and
host/verify/relais-verify recomputes them. This module proves the two things the
report needs: a chained journal is read exactly like the older ones, and a
journal without any chain is still read (non-regression). It also drives
relais-verify --check-chain over a journal the simulator produced, so the Python
side never becomes the authority on a verdict it only reports.

Run: python3 -m unittest discover -s host/report -p 'test_*.py'
Standard library only.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import journal_io  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
SIMULATE = os.path.join(ROOT, "build", "relais-simulate")
VERIFY = os.path.join(ROOT, "build", "relais-verify")
BUILT = os.path.exists(SIMULATE) and os.path.exists(VERIFY)
CHAIN_RE = re.compile(r"^[0-9a-f]{16}$")
ANCHOR_RE = re.compile(r"journal chain anchor: ([0-9a-f]{16}) \((\d+) records\)")


def legacy_line(tick, source="holder"):
    """One record of a journal written before the chain existed: no fingerprint."""
    return json.dumps({"kind": "tick", "tick": tick, "ns": 0, "mandate": 1,
                       "holder": "A", "source": source, "value": 0.1, "pos": 0.1,
                       "vel": 0.0, "setpoint": 0.1, "misses": 0, "lamp": "off",
                       "cause": "none", "certified": True, "certified_node": "B",
                       "stop": False})


def simulate(root, ticks=60):
    """Run the seeded simulator and return (journal path, anchor, records)."""
    journal = os.path.join(root, "actuator.jsonl")
    proc = subprocess.run([SIMULATE, "--ticks", str(ticks), "--seed", "11",
                           "--level", "complet", "--journal", journal, "--quiet"],
                          cwd=ROOT, capture_output=True, text=True, timeout=300)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    match = ANCHOR_RE.search(proc.stdout)
    assert match is not None, proc.stdout
    return journal, match.group(1), int(match.group(2))


def verify(journal, *args):
    """Run relais-verify --check-chain and return (code, stdout)."""
    proc = subprocess.run([VERIFY, journal, "--check-chain"] + list(args),
                          cwd=ROOT, capture_output=True, text=True, timeout=300)
    return proc.returncode, proc.stdout


class TestReportReadsAChainedJournal(unittest.TestCase):
    """The reader of the report must not care whether a chain is present."""

    def test_chained_journal_is_read_with_its_fingerprints(self):
        if not BUILT:
            self.skipTest("build/ is missing: run make all")
        root = tempfile.mkdtemp()
        try:
            journal, _, records = simulate(root)
            parsed = journal_io.read_journal(journal)
            self.assertEqual(parsed.malformed, 0)
            self.assertIsNotNone(parsed.header)
            self.assertEqual(parsed.chained, records)
            self.assertEqual(parsed.chained,
                             len(parsed.ticks) + len(parsed.events) + 1)
            self.assertTrue(CHAIN_RE.match(parsed.header.chain), parsed.header.chain)
            for tick in parsed.ticks:
                self.assertTrue(CHAIN_RE.match(tick.chain), tick.chain)
            self.assertTrue(all(CHAIN_RE.match(e.chain) for e in parsed.events))
            # The fingerprints are all different: a chain is not a constant.
            values = [t.chain for t in parsed.ticks]
            self.assertEqual(len(set(values)), len(values))
            self.assertEqual(parsed.ticks[0].tick, 1)
            self.assertEqual(len(parsed.ticks), 60)
        finally:
            shutil.rmtree(root)

    def test_journal_without_chain_still_reads(self):
        """Non-regression: an older journal has no fingerprint and stays valid."""
        root = tempfile.mkdtemp()
        try:
            path = os.path.join(root, "actuator.jsonl")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("\n".join(legacy_line(t) for t in range(1, 11)) + "\n")
            parsed = journal_io.read_journal(path)
            self.assertEqual(parsed.malformed, 0)
            self.assertEqual(parsed.chained, 0)
            self.assertEqual(len(parsed.ticks), 10)
            self.assertEqual(parsed.ticks[4].tick, 5)
            self.assertEqual(parsed.ticks[4].chain, "")
            self.assertEqual(parsed.ticks[4].source, "holder")
        finally:
            shutil.rmtree(root)


@unittest.skipUnless(BUILT, "build/ is missing: run make all")
class TestCheckChain(unittest.TestCase):
    """relais-verify --check-chain: what it accepts, and what it names."""

    def test_intact_journal_passes(self):
        root = tempfile.mkdtemp()
        try:
            journal, anchor, records = simulate(root)
            code, out = verify(journal, "--expect-chain", anchor)
            self.assertEqual(code, 0, out)
            self.assertIn("chain: OK", out)
            self.assertIn("anchor matches", out)
            self.assertGreater(records, 0)
        finally:
            shutil.rmtree(root)

    def test_edited_deleted_inserted_and_truncated_are_reported(self):
        root = tempfile.mkdtemp()
        try:
            journal, anchor, _ = simulate(root, ticks=80)
            with open(journal, encoding="utf-8") as handle:
                lines = handle.readlines()
            self.assertGreater(len(lines), 30)
            # One tick record in the middle of the file: the record the reader
            # names is its line number, since every line of the file is chained.
            target = next(i for i, line in enumerate(lines)
                          if '"value":' in line and i > 10)
            named = "CHAIN-BREAK record %d" % (target + 1)

            def write(name, content):
                path = os.path.join(root, name)
                with open(path, "w", encoding="utf-8") as handle:
                    handle.writelines(content)
                return path

            def bump_digit(line):
                """Change one digit of the applied value, same line length."""
                at = line.index('"value":') + len('"value":')
                while not line[at].isdigit():
                    at += 1
                return line[:at] + str((int(line[at]) + 1) % 10) + line[at + 1:]

            edited = list(lines)
            edited[target] = bump_digit(edited[target])
            self.assertNotEqual(edited[target], lines[target])
            code, out = verify(write("edited.jsonl", edited))
            self.assertEqual(code, 1, out)
            self.assertIn(named, out)

            deleted = lines[:target] + lines[target + 1:]
            code, out = verify(write("deleted.jsonl", deleted))
            self.assertEqual(code, 1, out)
            self.assertIn(named, out)

            inserted = lines[:target] + [lines[target - 1]] + lines[target:]
            code, out = verify(write("inserted.jsonl", inserted))
            self.assertEqual(code, 1, out)
            self.assertIn(named, out)

            # A cut tail is a valid chain: only the anchor reveals it.
            truncated = write("truncated.jsonl", lines[:target])
            code, out = verify(truncated)
            self.assertEqual(code, 0, out)
            self.assertIn("chain: OK", out)
            code, out = verify(truncated, "--expect-chain", anchor)
            self.assertEqual(code, 1, out)
            self.assertIn("CHAIN-SHORT", out)
        finally:
            shutil.rmtree(root)

    def test_legacy_journal_has_nothing_to_verify(self):
        root = tempfile.mkdtemp()
        try:
            path = os.path.join(root, "legacy.jsonl")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("\n".join(legacy_line(t) for t in range(1, 11)) + "\n")
            code, out = verify(path)
            self.assertEqual(code, 0, out)
            self.assertIn("chain: absent", out)
        finally:
            shutil.rmtree(root)


if __name__ == "__main__":
    unittest.main()
