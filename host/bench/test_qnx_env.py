"""Unit tests for host/bench/qnx_env.py (mission M01).

Two scenarios the mission explicitly requires:
  * a machine WITHOUT an SDP -- the case here -- must report verdict 2 with
    actionable messages (a person can proceed from the text alone);
  * a fabricated SDP tree in a tmpdir must report verdict 0.

The host-dependent probes (QEMU, KVM) are injected so every test is
deterministic on any machine. Standard library only (rule 6).
"""
import os
import stat
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import qnx_env
from qnx_env import SDP_ENV_SCRIPT, detect, find_sdp, parse_version

QEMU_OK = "QEMU emulator version 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18)"
QEMU_OLD = "QEMU emulator version 7.2.0"
KVM_OK = {"available": True, "accel": "kvm", "reason": "kvm usable"}
KVM_NO = {"available": False, "accel": "tcg",
          "reason": "/dev/kvm missing: falling back to TCG"}


def _executable(path: str) -> None:
    """Touch `path` with the execute bit set (a stand-in tool, not runnable)."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("#!/bin/sh\nexit 0\n")
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def make_fake_sdp(root, with_env=True, with_qcc=True, with_mkqnximage=True):
    """Build a minimal but plausible SDP 8.0 layout inside `root`."""
    sdp = os.path.join(root, "qnx800")
    os.makedirs(sdp, exist_ok=True)
    host = os.path.join(sdp, "host", "linux", "x86_64")
    os.makedirs(host, exist_ok=True)
    if with_env:
        with open(os.path.join(sdp, SDP_ENV_SCRIPT), "w", encoding="utf-8") as fh:
            fh.write("#!/bin/sh\nQNX_SDP=%s\nexport QNX_SDP\n" % sdp)
    if with_qcc:
        _executable(os.path.join(host, "usr", "bin", "qcc"))
    if with_mkqnximage:
        _executable(os.path.join(host, "usr", "bin", "mkqnximage"))
    os.makedirs(os.path.join(sdp, "target", "qnx8", "x86_64le"), exist_ok=True)
    return sdp


class TestParseVersion(unittest.TestCase):

    def test_qemu_8_2(self):
        self.assertEqual(parse_version(QEMU_OK), (8, 2))

    def test_qemu_7_2(self):
        self.assertEqual(parse_version(QEMU_OLD), (7, 2))

    def test_garbage(self):
        self.assertEqual(parse_version("no version here at all"), (0, 0))


class TestAbsent(unittest.TestCase):
    """The reference machine here: no SDP, so verdict 2 and actionable text."""

    def test_no_sdp_is_verdict_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = detect(search_roots=(tmp,), environ={},
                            version_text=QEMU_OK, kvm_info=KVM_OK)
        self.assertEqual(report.verdict, 2)
        self.assertEqual(report.label, "ABSENT")
        self.assertIsNone(report.sdp)

    def test_no_sdp_message_is_actionable(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = detect(search_roots=(tmp,), environ={},
                            version_text=QEMU_OK, kvm_info=KVM_OK)
        text = qnx_env.format_report(report)
        self.assertIn("QNX Everywhere", text)
        self.assertIn(SDP_ENV_SCRIPT, text)
        self.assertTrue(any("make -C target syntax" in fix for fix in report.fixes))

    def test_this_machine_reports_absent(self):
        report = detect()
        if report.sdp is not None:
            self.skipTest("this machine happens to have an SDP")
        self.assertEqual(report.verdict, 2)


class TestFakeSdp(unittest.TestCase):
    """A fabricated SDP tree in a tmpdir, with a healthy host underneath."""

    def test_verdict_0_ready(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdp = make_fake_sdp(tmp)
            report = detect(search_roots=(tmp,), environ={},
                            version_text=QEMU_OK, kvm_info=KVM_OK)
        self.assertEqual(report.verdict, 0)
        self.assertEqual(report.label, "READY")
        self.assertEqual(report.sdp, sdp)
        statuses = {c.name: c.status() for c in report.checks}
        self.assertEqual(statuses["env-script"], "OK")
        self.assertEqual(statuses["qcc"], "OK")
        self.assertEqual(statuses["mkqnximage"], "OK")
        self.assertEqual(statuses["qemu"], "OK")

    def test_missing_mkqnximage_is_partial(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_fake_sdp(tmp, with_mkqnximage=False)
            report = detect(search_roots=(tmp,), environ={},
                            version_text=QEMU_OK, kvm_info=KVM_OK)
        self.assertEqual(report.verdict, 1)
        self.assertEqual(report.label, "PARTIAL")
        self.assertTrue(any("mkqnximage" in fix for fix in report.fixes))

    def test_qemu_too_old_is_partial(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_fake_sdp(tmp)
            report = detect(search_roots=(tmp,), environ={},
                            version_text=QEMU_OLD, kvm_info=KVM_OK)
        self.assertEqual(report.verdict, 1)

    def test_qemu_missing_is_partial(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_fake_sdp(tmp)
            report = detect(search_roots=(tmp,), environ={},
                            version_text="", kvm_info=KVM_OK)
        self.assertEqual(report.verdict, 1)

    def test_kvm_missing_is_advisory_not_blocking(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_fake_sdp(tmp)
            report = detect(search_roots=(tmp,), environ={},
                            version_text=QEMU_OK, kvm_info=KVM_NO)
        self.assertEqual(report.verdict, 0)
        kvm = next(c for c in report.checks if c.name == "kvm")
        self.assertEqual(kvm.status(), "WARN")

    def test_env_var_wins_over_roots(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdp = make_fake_sdp(tmp)
            environ = {"RELAIS_QNX_SDP": sdp}
            self.assertEqual(find_sdp((tmp,), environ=environ), sdp)

    def test_cli_print_env_script(self):
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as tmp:
            sdp = make_fake_sdp(tmp)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = qnx_env.main(["--root", tmp, "--print", "env-script"])
        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().strip(),
                         os.path.join(sdp, SDP_ENV_SCRIPT))


if __name__ == "__main__":
    unittest.main()