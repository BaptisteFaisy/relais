"""Unit tests for the M46 QNX VM layer: qnx_vm.py, qnx_image.py,
qnx_failover.py.

Everything here runs WITHOUT a guest: buildfile snippet generation, QEMU
command-line construction (KVM and TCG paths), sentinel splitting, mapping
files, and the parsing of a real console transcript captured from a QNX 8.0
guest booted under KVM on this host (runs/qnx-vm/console.log, 2026-11-12).
Host-dependent probes are monkeypatched so the tests are deterministic
anywhere. Standard library only (rule 6).
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import qnx_failover
import qnx_image
import qnx_vm
from qnx_image import QnxVmError, write_ifs_files, write_ifs_start

# Real excerpt of runs/qnx-vm/console.log (QNX 8.0.0 guest, QEMU/KVM), with
# the ksh echo of the quote-split sentinel and CRLF line endings intact.
TRANSCRIPT_FIXTURE = (
    "Startup complete\r\n"
    'echo "__RL""_SHELL_READY__"\r\n'
    "__RL_SHELL_READY__\r\n"
    "# rl-proof ; echo \"__RL_DONE\"\"__\"$?\r\n"
    "rl-proof: uname sys=QNX rel=8.0.0 ver=2026/06/05-16:26:03EDT "
    "machine=x86pc node=relais-m46\r\n"
    "rl-proof: gettime_us calls=10000 min_us=0.008 mean_us=0.019 "
    "max_us=0.444\r\n"
    "rl-proof: sleep_overshoot_us calls=200 min_us=549.541 "
    "mean_us=996.117 max_us=1660.936\r\n"
    "rl-proof: done rc=0\r\n"
    "__RL_DONE__0\r\n"
)

KVM_OK = {"available": True, "accel": "kvm", "reason": "kvm usable"}
KVM_NO = {"available": False, "accel": "tcg", "reason": "/dev/kvm missing"}


def make_vm_dir(root):
    """A vm_dir whose mkqnximage artifacts exist (empty stand-ins)."""
    out = os.path.join(root, "output")
    os.makedirs(out)
    for name in ("ifs.bin", "disk-qemu.vmdk"):
        with open(os.path.join(out, name), "wb") as handle:
            handle.write(b"x")
    return root


class TestSplitSentinel(unittest.TestCase):
    def test_echo_cannot_match(self):
        sent = qnx_vm.split_sentinel(qnx_vm.SENTINEL, 9)
        self.assertNotIn(qnx_vm.SENTINEL, sent)
        self.assertIn(qnx_vm.SENTINEL, sent.replace('"', ""))

    def test_ready_sentinel(self):
        sent = qnx_vm.split_sentinel(qnx_vm.READY_SENTINEL)
        self.assertNotIn(qnx_vm.READY_SENTINEL, sent)
        self.assertIn(qnx_vm.READY_SENTINEL, sent.replace('"', ""))


class TestParseProof(unittest.TestCase):
    def test_parses_real_transcript(self):
        results = qnx_vm.parse_proof(TRANSCRIPT_FIXTURE)
        self.assertEqual(results["uname"]["sys"], "QNX")
        self.assertEqual(results["uname"]["rel"], "8.0.0")
        self.assertEqual(results["gettime_us"]["calls"], "10000")
        self.assertEqual(float(results["gettime_us"]["mean_us"]), 0.019)
        self.assertEqual(results["sleep_overshoot_us"]["calls"], "200")
        self.assertEqual(results["done"]["rc"], "0")

    def test_ignores_noise(self):
        self.assertEqual(qnx_vm.parse_proof("no proof here\r\n# "), {})


class TestParseFailover(unittest.TestCase):
    def test_takeover_line(self):
        text = ("rl-failover: second_up port=41234 silence_ms=40\r\n"
                "rl-failover: holder_up pid=41 port=41234 interval_ms=10\r\n"
                "rl-failover: killing holder_pid=41\r\n"
                "rl-failover: takeover takeover_us=43210 heartbeats=99\r\n"
                "rl-failover: done rc=0\r\n")
        out = qnx_failover.parse_failover(text)
        self.assertEqual(out["takeover"]["takeover_us"], "43210")
        self.assertEqual(out["done"]["rc"], "0")


class TestBuildQnxCommand(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.vm_dir = make_vm_dir(os.path.join(self._tmp.name, "vm"))
        self.run_dir = os.path.join(self._tmp.name, "run")
        os.makedirs(self.run_dir)
        self._real_kvm = qnx_vm.detect_kvm
        self._real_bin = qnx_vm.find_qemu_binary
        qnx_vm.find_qemu_binary = lambda name=None: "/usr/bin/qemu-system-x86_64"

    def tearDown(self):
        qnx_vm.detect_kvm = self._real_kvm
        qnx_vm.find_qemu_binary = self._real_bin
        self._tmp.cleanup()

    def test_kvm_path(self):
        qnx_vm.detect_kvm = lambda path=None: dict(KVM_OK)
        args, accel = qnx_vm.build_qnx_command(self.vm_dir, self.run_dir)
        self.assertEqual(accel, "kvm")
        self.assertIn("-enable-kvm", args)
        self.assertIn("host", args[args.index("-cpu") + 1])
        self.assertIn(os.path.join(self.vm_dir, "output", "ifs.bin"), args)

    def test_tcg_fallback_is_reported(self):
        qnx_vm.detect_kvm = lambda path=None: dict(KVM_NO)
        args, accel = qnx_vm.build_qnx_command(self.vm_dir, self.run_dir)
        self.assertEqual(accel, "tcg")
        self.assertNotIn("-enable-kvm", args)
        self.assertIn("max", args[args.index("-cpu") + 1])

    def test_kvm_requested_but_unavailable_raises(self):
        qnx_vm.detect_kvm = lambda path=None: dict(KVM_NO)
        with self.assertRaises(QnxVmError):
            qnx_vm.build_qnx_command(self.vm_dir, self.run_dir, accel="kvm")

    def test_console_and_control_sockets(self):
        qnx_vm.detect_kvm = lambda path=None: dict(KVM_OK)
        args, _ = qnx_vm.build_qnx_command(self.vm_dir, self.run_dir)
        joined = " ".join(args)
        self.assertIn(qnx_vm.serial_socket(self.run_dir), joined)
        self.assertIn(qnx_vm.qmp_socket(self.run_dir), joined)
        self.assertIn(qnx_vm.pid_file(self.run_dir), joined)
        self.assertIn("-display none", joined)
        self.assertEqual(qnx_vm.check_no_root_needed(args), [])

    def test_missing_artifacts_raise(self):
        qnx_vm.detect_kvm = lambda path=None: dict(KVM_OK)
        with self.assertRaises(QnxVmError):
            qnx_vm.build_qnx_command(self._tmp.name, self.run_dir)

    def test_low_hostfwd_port_raises(self):
        qnx_vm.detect_kvm = lambda path=None: dict(KVM_OK)
        with self.assertRaises(QnxVmError):
            qnx_vm.build_qnx_command(self.vm_dir, self.run_dir,
                                     hostfwd=["tcp:127.0.0.1:22-:22"])


class TestSnippets(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.vm_dir = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def _snippet(self, name):
        path = os.path.join(self.vm_dir, "local", "snippets", name)
        with open(path, encoding="utf-8") as handle:
            return handle.read()

    def test_ifs_files_respects_mkifs_rules(self):
        binary = os.path.join(self.vm_dir, "rl-proof")
        with open(binary, "wb") as handle:
            handle.write(b"\x7fELF")
        write_ifs_files(self.vm_dir, {"rl-proof": binary})
        text = self._snippet("ifs_files.custom")
        self.assertIn("[type=file]", text)               # sticky-attr reset
        self.assertNotIn("[+script]", text)              # corrupts binaries
        self.assertIn(f"[perms=0755] rl-proof={binary}", text)
        self.assertTrue(text.rstrip().endswith("[perms=0444]"))

    def test_ifs_files_missing_host_raises(self):
        with self.assertRaises(QnxVmError):
            write_ifs_files(self.vm_dir, {"x": "/nonexistent/x"})

    def test_ifs_start(self):
        write_ifs_start(self.vm_dir, ["echo hi", "  ", "# comment"])
        text = self._snippet("ifs_start.custom")
        self.assertIn("echo hi", text)
        self.assertIn("# comment", text)


class TestVmFilesMapping(unittest.TestCase):
    def test_parses_and_resolves(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "vm_files")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("# comment\n\nrl-proof=build/qnx-vm-bin/rl-proof\n")
            entries = qnx_failover.load_vm_files(path)
        self.assertEqual(entries,
                         {"rl-proof": os.path.join(qnx_vm.ROOT, "build",
                                                   "qnx-vm-bin", "rl-proof")})


if __name__ == "__main__":
    unittest.main(verbosity=2)
