#!/usr/bin/env python3
"""Tests of the QNX two-machine demo (mission M49), WITHOUT any QEMU.

What is checked here is everything that can be checked before a guest is
booted: the boot line that starts machine B inside the guest, the QEMU
command line that carries the user-net, the decoding of the STATUT frames the
tap observes, the analysis of a console transcript given as a fixture, and the
analysis of a journal given as a fixture.

The real run (a real QNX 8.0 guest, a real QMP freeze) is driven by
host/bench/qnx_demo.py; see docs/QNX-DEMO.md.

Requirement IDs: QNX-09, INJ-07, PERF-02, OBS-04.
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qnx_demo  # noqa: E402  (the path is set above)
import qnx_vm  # noqa: E402

ROOT = qnx_demo.ROOT

# A console transcript of a real QNX 8.0 boot (trimmed): the fixture the
# console analysis is checked against.
CONSOLE_FIXTURE = """
---> Starting slogger2
---> Starting Networking
---> Starting sshd
Process count:24
Startup complete
QNX relais-m46 8.0.0 2026/06/05-16:26:03EDT x86pc x86_64
# echo "__RL_S""HELL_READY__"
__RL_SHELL_READY__
"""

# A journal of a real run, trimmed: the actuator granted the mandate to B,
# certified A as the second, then revoked B and switched to A.
JOURNAL_FIXTURE = [
    '{"kind":"header","version":1,"params":{"T":10,"p":3,"N":8}}',
    '{"kind":"event","tick":10,"event":"mandate_granted","node":"B","mandate":1}',
    '{"kind":"event","tick":20,"event":"certified","node":"A","mandate":1}',
    '{"kind":"tick","tick":30,"holder":"B","source":"holder","value":0.1,'
    '"misses":0,"mandate":1,"certified":true,"certified_node":"A"}',
    '{"kind":"tick","tick":31,"holder":"B","source":"shadow","value":0.1,'
    '"misses":1,"mandate":1,"certified":true,"certified_node":"A"}',
    '{"kind":"event","tick":32,"event":"mandate_revoked","node":"B",'
    '"mandate":1,"detail":2}',
    '{"kind":"event","tick":32,"event":"mandate_auto_switch","node":"A",'
    '"mandate":2,"detail":2}',
    '{"kind":"tick","tick":32,"holder":"A","source":"shadow","value":0.1,'
    '"misses":0,"mandate":2,"certified":true,"certified_node":"A"}',
]


def statut_frame(tick, mandate, holder, source, misses, certified, cert_node):
    """Build a STATUT frame the way the actuator puts it on the wire."""
    frame = bytearray(56)
    frame[0:4] = bytes([0x31, 0x53, 0x4C, 0x52])   # the magic of spec 6.1
    frame[5] = 1                                    # RELAIS_MSG_STATUT
    frame[6] = 4                                    # RELAIS_NODE_ACTUATOR
    struct.pack_into(">QI", frame, 8, tick, mandate)
    frame[24] = holder
    frame[25] = source
    frame[26] = misses
    frame[27] = 1 if certified else 0
    frame[28] = cert_node
    return bytes(frame)


class BootLineTest(unittest.TestCase):
    """The line that starts machine B inside the guest (QNX-09)."""

    def test_line_starts_the_node_in_the_background(self):
        line = qnx_demo.boot_line("T=10,p=3,N=8", 40000)
        self.assertIn("/proc/boot/rl-node-boot.sh", line)
        self.assertIn("T=10,p=3,N=8", line)
        self.assertTrue(line.rstrip().endswith("&"))

    def test_line_carries_the_ports_of_the_bench(self):
        line = qnx_demo.boot_line("T=10,p=3,N=8", 40000)
        self.assertIn(" %d " % qnx_demo.ACT_PORT, line)
        self.assertIn(" %d " % qnx_demo.NODE_A_PORT, line)
        self.assertIn("40000", line)

    def test_script_points_at_the_host_actuator(self):
        with open(qnx_demo.BOOT_SCRIPT, encoding="utf-8") as fh:
            script = fh.read()
        self.assertIn("10.0.2.2", script)        # the user-net gateway
        self.assertIn("--self B", script)
        self.assertIn("--actuator", script)
        self.assertIn("rl-node", script)

    def test_script_is_launched_by_the_guest_boot(self):
        with open(qnx_demo.BOOT_SCRIPT, encoding="utf-8") as fh:
            script = fh.read()
        self.assertIn("#!/proc/boot/ksh", script)
        self.assertIn("ifconfig vtnet0", script)   # waits for the DHCP lease


class QemuCommandTest(unittest.TestCase):
    """The command line of the guest: user-net, hostfwd, no root."""

    def test_command_forwards_the_udp_port_of_the_guest(self):
        with tempfile.TemporaryDirectory() as vm_dir:
            os.makedirs(os.path.join(vm_dir, "output"))
            for name in ("ifs.bin", "disk-qemu.vmdk"):
                open(os.path.join(vm_dir, "output", name), "wb").close()
            with tempfile.TemporaryDirectory() as run_dir:
                # binary= passe un NOM sans le résoudre : le test construit la
                # ligne de commande mais ne doit pas exiger qemu sur la machine
                # (le runner CI n'a pas qemu dans son PATH, et n'en a pas besoin
                # pour construire une image).
                args, accel = qnx_vm.build_qnx_command(
                    vm_dir, run_dir, binary="qemu-system-x86_64",
                    hostfwd=["udp:127.0.0.1:%d-:%d" % (qnx_demo.GUEST_FWD_PORT,
                                                       qnx_demo.GUEST_NODE_PORT)])
        joined = " ".join(args)
        self.assertIn("hostfwd=udp:127.0.0.1:7302-:7101", joined)
        self.assertIn("netdev", joined)
        self.assertIn("user", joined)
        self.assertIn(accel, ("kvm", "tcg"))
        self.assertNotIn("tap", joined)
        self.assertNotIn("sudo", joined)


class StatutDecodeTest(unittest.TestCase):
    """The tap reads the documented header and nothing else."""

    def test_decodes_the_documented_fields(self):
        frame = statut_frame(4242, 2, qnx_demo.NODE_B, 1, 0, True,
                              qnx_demo.NODE_A)
        decoded = qnx_demo.decode_statut(frame)
        self.assertEqual(decoded["tick"], 4242)
        self.assertEqual(decoded["mandate"], 2)
        self.assertEqual(decoded["holder"], qnx_demo.NODE_B)
        self.assertEqual(decoded["certified_node"], qnx_demo.NODE_A)
        self.assertTrue(decoded["certified"])

    def test_ignores_anything_that_is_not_a_statut(self):
        self.assertIsNone(qnx_demo.decode_statut(b"too short"))
        other = bytearray(statut_frame(1, 1, 1, 1, 0, False, 0))
        other[5] = 2                                   # an ORDRE, not a STATUT
        self.assertIsNone(qnx_demo.decode_statut(bytes(other)))


class ConsoleFixtureTest(unittest.TestCase):
    """The console transcript of the guest, given as a fixture (OBS-04)."""

    def test_finds_the_qnx_version_and_the_boot_marker(self):
        facts = qnx_demo.parse_guest_console(CONSOLE_FIXTURE)
        self.assertIn("8.0.0", facts["version"])
        self.assertTrue(facts["version"].startswith("QNX "))
        self.assertTrue(facts["startup_complete"])

    def test_a_console_without_the_marker_says_so(self):
        facts = qnx_demo.parse_guest_console("---> Starting slogger2\n")
        self.assertEqual(facts["version"], "")
        self.assertFalse(facts["startup_complete"])


class JournalFixtureTest(unittest.TestCase):
    """The journal of the actuator, given as a fixture (ACT-07)."""

    def setUp(self):
        handle, self.path = tempfile.mkstemp(suffix=".jsonl")
        with os.fdopen(handle, "w", encoding="utf-8") as fh:
            fh.write("\n".join(JOURNAL_FIXTURE) + "\n")

    def tearDown(self):
        os.unlink(self.path)

    def test_finds_the_takeover_and_the_silence(self):
        facts = qnx_demo.analyse_journal(self.path)
        self.assertEqual(facts["auto_switch_tick"], 32)
        self.assertEqual(facts["last_holder_b_tick"], 31)
        self.assertEqual(facts["last_fresh_order_b_tick"], 30)
        self.assertEqual(facts["takeover_ticks"], 2)
        self.assertEqual(facts["misses_at_takeover"], 2)
        self.assertEqual(facts["sources"]["holder"], 1)
        self.assertEqual(facts["sources"]["shadow"], 2)

    def test_keeps_the_evidence_of_the_revoke(self):
        facts = qnx_demo.analyse_journal(self.path)
        revoked = [e for e in facts["events"] if e[1] == "mandate_revoked"]
        self.assertEqual(revoked[0][4], 2)     # the miss count the actuator kept


class DefaultsTest(unittest.TestCase):
    """The defaults of the demo stay inside the protocol's bounds."""

    def test_period_and_patience_are_legal(self):
        values = dict(pair.split("=") for pair in qnx_demo.DEFAULT_PARAMS.split(","))
        self.assertLessEqual(int(values["T"]), 50)     # CMD-01
        self.assertLessEqual(int(values["p"]), 3)      # N <= 8 would not cover it
        self.assertEqual(int(values["N"]), 8)


if __name__ == "__main__":
    unittest.main()
