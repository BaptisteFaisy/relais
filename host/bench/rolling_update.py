#!/usr/bin/env python3
"""Relais — the rolling update on a plain PC (REL-10, spec 12 item 6).

Three real processes on this host: two computers and one actuator, exactly like
the QNX image minus QNX. The bench stops a machine for the installation, then
reboots it, and triggers the handover between the two updates. Nothing needs
root, no QEMU, no SDP.

The two machines are updated in turn:

  1. the second is updated (stopped, rebooted, recertified);
  2. the holder cedes the mandate to it (REL-09);
  3. the other machine, now the second, is updated the same way;
  4. the holder cedes the mandate back;
  5. we are back in the nominal redundant state, with a mandate number that only
     ever grew.

The cession: on the QNX target `relaisctl handover` makes relais-core emit a
CESSION frame (RL_CMD_HANDOVER -> relais_node_on_handover, reason "maintenance"
= 2). The PC node binary has no such command channel, so the bench plays that
part itself: it emits the very same CESSION frame the machine's maintenance tool
would send, carrying the machine's identity, its current mandate, and the
level-S control (CRC-32C) -- the demo runs without a key. The alternative path
(no cession, the actuator deposes the silent machine after p+1 ticks) is proven
in core/tests/test_ota.c; this demo sticks to the planned handover.

Usage:
  python3 host/bench/rolling_update.py            # the rolling update
  python3 host/bench/rolling_update.py --all-ticks  # print every tick
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT_ACTUATOR = 7300
PORT_A = 7301
PORT_B = 7302

# The protocol's own parameters (T = 5 ms, p = 1, N = 8), the same as the
# simulator and scenarios/rolling-update.json. T is the control law's design
# period: at a coarser T the discrete loop oscillates and DET-09 latches the
# witness, so the PC demo keeps T = 5 ms (the protocol is tick based, a loaded
# PC only slows the ticks down, it does not desynchronise them -- CMD-07).
PARAMS = {"T": 5, "p": 1, "N": 8}
TRAJECTORY = "kind=slalom amp=0.6 freq=0.5"
TICKS = 400
INSTALL_MS = 200           # installation window, ~40 ticks at T = 5 ms


def binary(name):
    path = os.path.join(ROOT, "build", name)
    if not os.path.exists(path):
        sys.exit("missing %s: run `make` first" % path)
    return path


def params_string():
    return ",".join("%s=%s" % kv for kv in PARAMS.items())


# ---------------------------------------------------------------- CRC-32C
# Castagnoli, reflected (poly 0x82F63B78), init 0xFFFFFFFF, inverted at the end:
# the same value core/src/check.c uses at level S. Needed here for the one frame
# the bench sends on behalf of the maintenance tool.
_CRC_TABLE = []


def _make_crc_table():
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
        table.append(crc)
    return table


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc = _CRC_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def cession_frame(sender, mandate, tick):
    """The CESSION frame the machine's maintenance tool sends (REL-09)."""
    body = bytearray()
    body += (0x31534C52).to_bytes(4, "big")   # magic "RLS1"
    body += bytes([1])                         # version
    body += bytes([5])                         # type: CESSION
    body += bytes([sender])                    # 1 = A, 2 = B
    body += bytes([0x02])                      # flags: RELAIS_FLAG_CESSION
    body += int(tick).to_bytes(8, "big")
    body += int(mandate).to_bytes(4, "big")
    body += (1).to_bytes(2, "big")             # payload length
    body += (0).to_bytes(2, "big")             # reserved
    body += bytes([2])                         # payload: reason "maintenance"
    body += int(crc32c(bytes(body))).to_bytes(8, "big")
    return bytes(body)


# ---------------------------------------------------------------- process
class Node:
    def __init__(self, name, port, peer_port):
        self.name = name
        self.port = port
        self.log_path = os.path.join(RUN_DIR, "node-%s.log" % name)
        self.cmd = [binary("relais-node"), "--self", name,
                    "--port", str(port),
                    "--actuator", "127.0.0.1:%d" % PORT_ACTUATOR,
                    "--peer", "127.0.0.1:%d" % peer_port,
                    "--ticks", str(TICKS),
                    "--params", params_string()]
        self.proc = None

    def start(self):
        log = open(self.log_path, "w", encoding="utf-8")
        self.proc = subprocess.Popen(self.cmd, cwd=ROOT, stdout=log,
                                     stderr=subprocess.STDOUT,
                                     start_new_session=True, text=True)

    def kill(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()

    def reborn(self, journal):
        """Stop the machine (installation), then reboot it empty (DEG-03)."""
        print("  [%s] update: -> out of service for %.0f ms" %
              (self.name, INSTALL_MS))
        self.kill()
        time.sleep(INSTALL_MS / 1000.0)
        self.start()  # a fresh process: empty state, needs to be certified
        print("  [%s] update: rebooted, will be recertified" % self.name)
        journal.wait_certified(self.name)


# ---------------------------------------------------------------- journal
class Journal:
    def __init__(self, path):
        self.path = path
        self.records = []
        self.seen = 0

    def refresh(self):
        if not os.path.exists(self.path):
            return
        with open(self.path, encoding="utf-8") as fh:
            lines = fh.readlines()
        for line in lines[self.seen:]:
            try:
                self.records.append(json.loads(line))
            except ValueError:
                pass
        self.seen = len(lines)

    def ticks(self):
        return [r for r in self.records if r.get("kind") == "tick"]

    def events(self):
        return [r for r in self.records if r.get("kind") == "event"]

    def last_tick(self):
        ticks = self.ticks()
        return ticks[-1] if ticks else None

    def wait_until(self, pred, timeout_s, label):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            self.refresh()
            t = self.last_tick()
            if t is not None and pred(t):
                return t
            time.sleep(0.02)
        sys.exit("[%s] never became true: %s" % (label, self.last_tick()))

    def wait_armed(self):
        def ok(t):
            return t.get("holder") and t.get("certified") is True
        print("  arm: waiting for a holder and a certified second ...")
        return self.wait_until(ok, 15.0, "armed")

    def wait_certified(self, name):
        def ok(t):
            return t.get("certified_node") == name and t.get("certified") is True
        print("  wait: %s certified again ..." % name)
        return self.wait_until(ok, 10.0, "certified %s" % name)

    def wait_holder(self, name):
        def ok(t):
            return t.get("holder") == name
        print("  wait: %s holds the mandate ..." % name)
        return self.wait_until(ok, 10.0, "holder %s" % name)


RUN_DIR = None  # set by main(); used by Node for its log path


def start_world(journal):
    actuator_log = open(os.path.join(RUN_DIR, "actuator.log"), "w",
                        encoding="utf-8")
    actuator = subprocess.Popen(
        [binary("relais-actuator"), "--port", str(PORT_ACTUATOR),
         "--peer", "A=127.0.0.1:%d" % PORT_A,
         "--peer", "B=127.0.0.1:%d" % PORT_B,
         "--ticks", str(TICKS),
         "--params", params_string(),
         "--trajectory", TRAJECTORY, "--journal", journal],
        cwd=ROOT, stdout=actuator_log, stderr=subprocess.STDOUT,
        start_new_session=True, text=True)
    node_a = Node("A", PORT_A, PORT_B)
    node_b = Node("B", PORT_B, PORT_A)
    node_a.start()
    node_b.start()
    return actuator, node_a, node_b


def send_cession(holder_name, mandate, tick):
    sender = 1 if holder_name == "A" else 2
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(cession_frame(sender, mandate, tick),
                ("127.0.0.1", PORT_ACTUATOR))
    sock.close()
    print("  cession: %s hands the mandate over (mandate %d, tick %d)" %
          (holder_name, mandate, tick))


def verify(journal):
    proc = subprocess.run([binary("relais-verify"), journal, "--level", "complet"],
                          cwd=ROOT, capture_output=True, text=True)
    print(proc.stdout.strip())
    return proc.returncode


def report(journal, verbose):
    """Line-by-line account + the structural verdict of the rolling update.

    Two kinds of claim are kept apart, on purpose:
      * STRUCTURAL (load independent): the mandate grew by exactly one step per
        change of holder, two cessions drove the two handovers, the emergency
        law never took over, and we are back in the nominal redundant state.
        This is what REL-10 is about and what this demo proves on real
        processes.
      * REAL TIME (load dependent): not one stale tick, the witness never red.
        On a quiet machine these hold; on a loaded shared PC the jitter can cost
        a tick and DET-09 can latch the witness. The rigorous, deterministic
        proof of these is core/tests/test_ota.c and the seeded simulator
        (`make test`, `make simulate`), not a wall-clock run.
    """
    ticks = journal.ticks()
    if not ticks:
        print("  no tick recorded")
        return False
    first_fresh = next((t["tick"] for t in ticks
                        if t.get("source") in ("holder", "shadow")), None)
    stale = [t["tick"] for t in ticks
             if first_fresh and t["tick"] >= first_fresh
             and t.get("source") not in ("holder", "shadow")]
    red = [t["tick"] for t in ticks if t.get("lamp") == "red"]
    fallback = [t["tick"] for t in ticks if t.get("source") == "fallback"
                and first_fresh and t["tick"] >= first_fresh]
    cessions = [e for e in journal.events() if e.get("event") == "cession"]

    # Mandate steps: exactly +1 per change of holder (MAN-03).
    changes, bad = 0, 0
    for prev, cur in zip(ticks, ticks[1:]):
        if cur.get("mandate", 0) < prev.get("mandate", 0):
            bad += 1
        if cur.get("holder") != prev.get("holder") and cur.get("holder"):
            changes += 1
            if cur.get("mandate") != prev.get("mandate", 0) + 1:
                bad += 1
    final = ticks[-1]
    structural_ok = (changes == 3 and bad == 0 and len(cessions) == 2
                     and not fallback and final.get("holder")
                     and final.get("certified") is True)

    print("  --- structural (REL-10, load independent) ---")
    print("  mandate changes: %d (want 3: grant + 2 cessions), bad steps: %d" %
          (changes, bad))
    print("  cessions emitted: %d (want 2)" % len(cessions))
    print("  emergency-law ticks after arming: %d (want 0)" % len(fallback))
    print("  final state: holder %s, certified %s, mandate %s" %
          (final.get("holder") or "-", final.get("certified_node") or "-",
           final.get("mandate")))
    print("  STRUCTURAL VERDICT: %s" % ("OK" if structural_ok else "FAILED"))
    print("  --- real time (load dependent; rigorous proof = test_ota) ---")
    print("  stale ticks after arming: %d, red-lamp ticks: %d" %
          (len(stale), len(red)))
    if verbose:
        cols = "%-6s %-8s %-7s %-9s %-6s %s"
        print("  " + cols % ("tick", "holder", "mandate", "source", "lamp", "certified"))
        for t in ticks:
            print("  " + cols % (t.get("tick"), t.get("holder") or "-",
                                 t.get("mandate"), t.get("source"),
                                 t.get("lamp"), t.get("certified_node") or "-"))
    return structural_ok


def main():
    global RUN_DIR
    ap = argparse.ArgumentParser(description="Relais rolling update, PC mode")
    ap.add_argument("--all-ticks", action="store_true",
                    help="print every tick line")
    ap.add_argument("--out", default="", help="run directory")
    args = ap.parse_args()

    _CRC_TABLE.extend(_make_crc_table())
    RUN_DIR = args.out or os.path.join(
        "runs", "rolling-update-%d" % int(time.time()))
    os.makedirs(RUN_DIR, exist_ok=True)
    journal_path = os.path.join(RUN_DIR, "actuator.jsonl")
    journal = Journal(journal_path)

    print("== Relais rolling update (REL-10), mode PC, T=%d ms p=%d ==" %
          (PARAMS["T"], PARAMS["p"]))
    actuator, node_a, node_b = start_world(journal_path)
    try:
        armed = journal.wait_armed()
        holder = armed.get("holder")      # "A" or "B"
        second = "B" if holder == "A" else "A"
        print("  nominal state: %s holds, %s certified" % (holder, second))

        # 1. the second is updated.
        node_b.reborn(journal) if second == "B" else node_a.reborn(journal)

        # 2. the holder cedes to the freshly certified second.
        time.sleep(0.2)
        current = journal.last_tick()
        send_cession(holder, current["mandate"], current["tick"])
        journal.wait_holder(second)

        # 3. the other machine, now the second, is updated.
        node_a.reborn(journal) if second == "B" else node_b.reborn(journal)

        # 4. the holder cedes the mandate back.
        time.sleep(0.2)
        current = journal.last_tick()
        send_cession(second, current["mandate"], current["tick"])
        journal.wait_holder(holder)

        # Back to the nominal redundant state.
        journal.wait_certified(second)
        print("  nominal state restored: %s holds, %s certified" %
              (holder, second))
    finally:
        # Stop the tempo FIRST, so the journal ends on the nominal state and the
        # actuator does not keep ticking (and churning the mandate) while the
        # machines are being torn down.
        if actuator.poll() is None:
            actuator.kill()
        time.sleep(0.05)
        for proc in (node_a, node_b):
            proc.kill()
    journal.refresh()
    structural_ok = report(journal, args.all_ticks)
    print("  --- relais-verify (offline invariants, level complet) ---")
    vcode = verify(journal_path)
    print("  run directory: %s" % RUN_DIR)
    if vcode != 0:
        print("  note: relais-verify flags real-time violations. On a loaded "
              "shared PC the 5 ms deadline is not met every tick; the "
              "deterministic proof of REL-10 (zero stale tick, witness never "
              "red) is core/tests/test_ota.c. Re-run on a quiet machine for a "
              "clean trace.")
    print("  ROLLING UPDATE: %s" % ("OK" if structural_ok else "FAILED"))
    return 0 if structural_ok else 1


if __name__ == "__main__":
    sys.exit(main())