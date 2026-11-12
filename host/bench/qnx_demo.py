#!/usr/bin/env python3
"""Relais M49 — the two-machine demo, with a REAL QNX 8.0 as machine B.

    python3 host/bench/qnx_demo.py [--out runs/qnx-demo] [--ticks 12000]

The actuator and machine A run on the HOST; machine B is an INVITED QNX 8.0
guest (QEMU 8.2 + KVM, image built by the SDP 8.0 mkqnximage) running
host/node/relais-node.c cross-compiled with qcc.  The fault is a REAL freeze
of the whole guest: the QMP `stop` halts its CPUs, timers and network, like a
hung computer (spec 4.11).  The guest reaches the host actuator through the
QEMU user-net: 10.0.2.2:7100 for the ORDRE, hostfwd udp 127.0.0.1:7302 ->
guest 7101 for the STATUT.  The boot line that starts it is
image/fragments/rl-node-boot.sh, launched by ifs_start.custom.

The actuator revokes the mandate of the frozen machine on its OWN evidence
(p+1 ticks without a valid order: MAN-04, DET-03) and gives it to the
certified second, machine A (MAN-10).  The takeover is measured on the host
clock and counted in ticks.  Evidence in --out: the journal, the console
transcript, the screendumps, qmp.json, qnx-demo.json and proof.svg.

Honesty: the times are the times of the EMULATOR, not hard real time; the
safety is in TICKS.  See docs/QNX-DEMO.md.Requirement IDs: QNX-09, INJ-07, PERF-02, OBS-04, MAN-04, MAN-10, DET-03.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from typing import Optional

try:  # package layout
    from . import qnx_image, qnx_relais, qnx_vm
except ImportError:  # flat script layout
    import qnx_image
    import qnx_relais
    import qnx_vm

QnxVmError = qnx_image.QnxVmError
create_vm, rebuild_vm = qnx_image.create_vm, qnx_image.rebuild_vm
write_ifs_files = qnx_image.write_ifs_files
write_ifs_start = qnx_image.write_ifs_start
qcc_build, CORE_FILES = qnx_relais.qcc_build, qnx_relais.CORE_FILES
QnxVm, ROOT, transcript_path = qnx_vm.QnxVm, qnx_vm.ROOT, qnx_vm.transcript_path

ACT_PORT = 7100          # the actuator, on the host loopback
NODE_A_PORT = 7101       # machine A, on the host
TAP_PORT = 7104          # the observer interposed between actuator and A
GUEST_NODE_PORT = 7101   # machine B, inside the guest
GUEST_FWD_PORT = 7302    # host udp -> guest udp (QEMU user-net hostfwd)
NODE_A, NODE_B, ACTUATOR_ID = 1, 2, 4
DEFAULT_PARAMS, DEFAULT_TRAJ = "T=10,p=3,N=8", "kind=slalom amp=0.6 freq=0.5"
BOOT_SCRIPT = os.path.join(ROOT, "image", "fragments", "rl-node-boot.sh")
GUEST_BIN_DIR = os.path.join(ROOT, "build", "qnx-demo-bin")
NODE_SOURCES = (["host/node/relais-node.c", "host/measure/latency.c",
                 "host/measure/latency_json.c"]
                + ["core/src/%s" % name for name in CORE_FILES])


def now_us() -> int:
    """The host monotonic clock, in microseconds."""
    return time.monotonic_ns() // 1000


def decode_statut(data: bytes) -> Optional[dict]:
    """Decode a STATUT frame (spec 6.1: big-endian, 24-byte header). Read-only."""
    if len(data) < 56 or data[5] != 1 or data[6] != ACTUATOR_ID:
        return None
    tick, mandate = struct.unpack_from(">QI", data, 8)
    return {"tick": tick, "mandate": mandate, "holder": data[24],
            "source": data[25], "misses": data[26],
            "certified": bool(data[27]), "certified_node": data[28]}


def parse_guest_console(text: str) -> dict:
    """What the guest console says: the QNX version and the boot markers."""
    facts: dict = {"version": "", "startup_complete": False}
    for line in text.splitlines():
        if line.startswith("QNX "):
            facts["version"] = line.strip()
        if "Startup complete" in line:
            facts["startup_complete"] = True
    return facts


class StatutTap:
    """A live observer of the actuator's STATUT, between the actuator and A.
    It forwards every datagram verbatim (no protocol of its own) and decodes
    the documented header, so the run is seen while it happens.
    """
    def __init__(self, bind_port: int, forward_port: int) -> None:
        self.forward = ("127.0.0.1", forward_port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", bind_port))
        self.sock.settimeout(0.2)
        self.frames: list[dict] = []
        self.lock = threading.Lock()
        self.stopped = False
        self.thread = threading.Thread(target=self._loop, daemon=True)
    def start(self) -> "StatutTap":
        self.thread.start()
        return self
    def close(self) -> None:
        self.stopped = True
        self.thread.join(timeout=2.0)
        self.sock.close()
    def _loop(self) -> None:
        while not self.stopped:
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                self.sock.sendto(data, self.forward)
            except OSError:
                pass
            frame = decode_statut(data)
            if frame is None:
                continue
            frame["t_us"] = now_us()
            with self.lock:
                self.frames.append(frame)
    def last(self) -> Optional[dict]:
        with self.lock:
            return dict(self.frames[-1]) if self.frames else None
    def mark(self) -> int:
        """The index of the next frame: the beginning of a window."""
        with self.lock:
            return len(self.frames)
    def first_after(self, index: int, key: str, value: int) -> Optional[dict]:
        """The first frame after *index* whose *key* equals *value*."""
        with self.lock:
            for frame in self.frames[index:]:
                if frame.get(key) == value:
                    return dict(frame)
        return None
    def wait_for(self, key: str, value: int, timeout: float,
                 since_tick: int = 0, poll: float = 0.002,
                 alive: Optional[callable] = None) -> Optional[dict]:
        """Wait for a frame whose *key* equals *value*; *alive* stops it."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.last()
            if frame is not None and frame["tick"] > since_tick \
                    and frame.get(key) == value:
                return frame
            if alive is not None and not alive():
                return None
            time.sleep(poll)
        return None


def wait_stable(tap: StatutTap, ticks: int, timeout: float,
                holder: int = NODE_B, certified: int = NODE_A) -> Optional[dict]:
    """Wait until the pair has been (holder, certified second) for *ticks*."""
    deadline = time.monotonic() + timeout
    anchor = None
    while time.monotonic() < deadline:
        frame = tap.last()
        if frame is not None and frame["holder"] == holder \
                and frame["certified_node"] == certified:
            anchor = frame["tick"] if anchor is None else anchor
            if frame["tick"] - anchor >= ticks:
                return frame
        else:
            anchor = None
        time.sleep(0.002)
    return None


def build_guest_node() -> str:
    """The QNX node: host/node/relais-node.c plus core/, built by the qcc."""
    out = os.path.join(GUEST_BIN_DIR, "rl-node")
    return qcc_build(NODE_SOURCES, out, ["host/measure"], "-lsocket -lm")


def boot_line(params: str, ticks: int) -> str:
    """The guest's boot line: start machine B, pointing at the host actuator."""
    return ("/proc/boot/ksh /proc/boot/rl-node-boot.sh %s %d %d %d &"
            % (params, ticks, ACT_PORT, NODE_A_PORT))


def build_image(vm_dir: str, params: str, ticks: int) -> None:
    """The QNX 8.0 image: the node, its boot line, and the mkqnximage build."""
    binary = build_guest_node()
    create_vm(vm_dir, "relais-m49")
    write_ifs_files(vm_dir, {"rl-node": binary, "rl-node-boot.sh": BOOT_SCRIPT})
    write_ifs_start(vm_dir, [boot_line(params, ticks)])
    rebuild_vm(vm_dir)


def actuator_command(journal: str, ticks: int, params: str, traj: str) -> list[str]:
    """The host actuator: the tempo, the mandate, the journal (the authority)."""
    return [os.path.join(ROOT, "build", "relais-actuator"),
            "--port", str(ACT_PORT),
            "--peer", "A=127.0.0.1:%d" % TAP_PORT,
            "--peer", "B=127.0.0.1:%d" % GUEST_FWD_PORT,
            "--ticks", str(ticks), "--params", params, "--trajectory", traj,
            "--journal", journal]


def node_a_command(ticks: int, params: str) -> list[str]:
    """Machine A, on the host: the same core/ state machine as the guest."""
    return [os.path.join(ROOT, "build", "relais-node"),
            "--self", "A", "--port", str(NODE_A_PORT),
            "--actuator", "127.0.0.1:%d" % ACT_PORT,
            "--peer", "127.0.0.1:%d" % GUEST_FWD_PORT,
            "--ticks", str(ticks), "--params", params]


def spawn(cmd: list[str], log_path: str) -> tuple[subprocess.Popen, object]:
    """Start a bench process with its output in a file."""
    handle = open(log_path, "w", encoding="utf-8")
    return subprocess.Popen(cmd, cwd=ROOT, stdout=handle,
                            stderr=subprocess.STDOUT), handle

def terminate(proc: Optional[subprocess.Popen]) -> None:
    """Stop a bench process, gently then firmly. Never leaves an orphan."""
    if proc is None or proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def arm_and_freeze(vm: QnxVm, tap: StatutTap, args: argparse.Namespace,
                   out: str, run_dir: str, summary: dict,
                   actuator: subprocess.Popen) -> Optional[subprocess.Popen]:
    """Arm the pair, freeze the guest, wait for the takeover."""
    alive = lambda: actuator.poll() is None  # noqa: E731 (one line predicate)
    granted = tap.wait_for("holder", NODE_B, args.arm_timeout, alive=alive)
    if granted is None:
        raise QnxVmError("machine B (the QNX guest) never took the mandate; "
                         "suspects: boot line, DHCP lease, user-net, actuator "
                         "out of ticks. Transcript: %s" % transcript_path(run_dir))
    summary["mandate_granted_tick"] = granted["tick"]
    print("machine B (QNX guest) holds the mandate at tick %d" % granted["tick"], file=sys.stderr)
    node_a, _ = spawn(node_a_command(args.ticks, args.params),
                      os.path.join(out, "node-A.log"))
    certified = tap.wait_for("certified_node", NODE_A, args.arm_timeout,
                             since_tick=granted["tick"], alive=alive)
    if certified is None:
        raise QnxVmError("machine A was never certified as the second (REP-05); "
                         "nothing to hand over to. Transcript: %s"
                         % transcript_path(run_dir))
    summary["certified_tick"] = certified["tick"]
    print("machine A certified as the second at tick %d"
          % certified["tick"], file=sys.stderr)
    stable = wait_stable(tap, args.stable_ticks, args.arm_timeout)
    if stable is None:
        raise QnxVmError("the pair never stayed (B holder, A certified) for %d "
                         "ticks; the bench is not stable enough to freeze."
                         % args.stable_ticks)
    summary["stable_tick"] = stable["tick"]
    print("== QMP stop: the whole guest freezes ==", file=sys.stderr)
    summary.update(freeze_and_handover(vm, tap, out, args.handover_timeout))
    if not summary.get("handover"):
        raise QnxVmError("no takeover after the freeze: the actuator never moved "
                         "the mandate. QMP status: %s / %s" % (
                             summary.get("status_before"),
                             summary.get("status_frozen")))
    print("takeover at tick %d, %d us after the freeze"
          % (summary["handover_tick"], summary["takeover_us"]), file=sys.stderr)
    return node_a


def freeze_and_handover(vm: QnxVm, tap: StatutTap, out: str,
                        handover_timeout: float) -> dict:
    """The fault: QMP `stop` freezes the whole guest, `cont` wakes it up.
    The takeover is timed on the tap's clock (the host monotonic clock)."""
    result: dict = {"status_before": vm.qmp.execute("query-status")}
    vm.qmp.execute("screendump", filename=os.path.join(out, "guest-before.png"))
    mark, pre = tap.mark(), tap.last()
    pre_t_us, freeze_tick = (pre["t_us"], pre["tick"]) if pre else (0, 0)
    vm.qmp.execute("stop")
    result.update(frozen_at_us=now_us(), freeze_tick=freeze_tick,
                  pre_freeze_t_us=pre_t_us)  # the tap clock, not the QMP one
    result["status_frozen"] = vm.qmp.execute("query-status")
    vm.qmp.execute("screendump", filename=os.path.join(out, "guest-frozen.png"))
    handover = tap.wait_for("holder", NODE_A, handover_timeout, since_tick=freeze_tick)
    if handover is None:
        result["handover"] = None
        vm.qmp.execute("cont")
        return result
    exact = tap.first_after(mark, "holder", NODE_A) or handover
    result["handover"], result["handover_tick"] = exact, exact["tick"]
    result["takeover_us"] = exact["t_us"] - pre_t_us
    result["takeover_ticks"] = exact["tick"] - freeze_tick
    vm.qmp.execute("cont")
    result["status_after"] = vm.qmp.execute("query-status")
    time.sleep(2.0)
    vm.qmp.execute("screendump", filename=os.path.join(out, "guest-after.png"))
    return result


def analyse_journal(journal: str) -> dict:
    """What the journal (the authority) says about the takeover."""
    sys.path.insert(0, os.path.join(ROOT, "host", "report"))
    from journal_io import read_journal
    jrn = read_journal(journal)
    ticks, events = jrn.ticks, jrn.events
    auto = [e for e in events if e.event == "mandate_auto_switch"]
    revoke = [e for e in events if e.event == "mandate_revoked"]
    last_b = max([t.tick for t in ticks if t.holder == "B"], default=0)
    fresh_b = max([t.tick for t in ticks if t.holder == "B"
                   and t.source == "holder"], default=0)
    switch_tick = auto[-1].tick if auto else 0
    return {"ticks": len(ticks), "last_holder_b_tick": last_b,
            "last_fresh_order_b_tick": fresh_b,
            "auto_switch_tick": switch_tick,
            "takeover_ticks": max(0, switch_tick - fresh_b),
            "misses_at_takeover": revoke[-1].detail if revoke else 0,
            "malformed_lines": jrn.malformed,
            "events": [(e.tick, e.event, e.node, e.mandate, e.detail) for e in events
                       if e.event.startswith(("mandate", "certified", "decertified"))],
            "sources": {name: sum(1 for t in ticks if t.source == name)
                        for name in ("holder", "shadow", "testament", "fallback")}}


def run_verdict(journal: str, disturbed: bool = False) -> str:
    """The verifier's verdict on the journal (the C verifier is the authority).
    With *disturbed*, the channel is declared lossy (the hypothesis of I4):
    the gap is then measured instead of being enforced.
    """
    cmd = [os.path.join(ROOT, "build", "relais-verify"), journal,
           "--level", "socle"]
    if disturbed:
        cmd.append("--lossy-channel")
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    return (proc.stdout or proc.stderr or "").strip()


def make_proof(out: str, journal: str, summary: dict) -> str:
    """One image: the command curve through the freeze, the source of every
    tick, the mandates, and the verifier's verdict."""
    sys.path.insert(0, os.path.join(ROOT, "host", "report"))
    import charts
    from journal_io import read_journal
    jrn = read_journal(journal)
    ticks, values = ([t.tick for t in jrn.ticks], [t.value for t in jrn.ticks])
    sources, holders = ([t.source for t in jrn.ticks],
                         [t.holder or "-" for t in jrn.ticks])
    mandates = [t.mandate for t in jrn.ticks]
    events = [(e.tick, e.event) for e in jrn.events
              if "mandate" in e.event or e.event in ("cession", "fallback_enter")]
    curve = charts.series_svg(ticks, values, None,
                              title="Command applied by the actuator")
    strip = charts.source_strip_svg(sources)
    mandate = charts.mandate_strip_svg(holders, mandates, events)
    handover = summary.get("handover") or {}
    verdict = summary.get("verdict", "")
    line = ("machine B (QNX 8.0) frozen by QMP stop -> takeover at tick %s "
            "in %s us; %s" % (handover.get("tick", "?"),
                              summary.get("takeover_us", "?"),
                              verdict.splitlines()[0] if verdict else "no verdict"))
    height = 300 + 90 + 95 + 90
    svg = ("<svg xmlns='http://www.w3.org/2000/svg' width='900' height='%d'>"
           "<rect width='900' height='%d' fill='white'/><text x='16' y='24' "
           "font-family='sans-serif' font-size='15' font-weight='bold'>Relais - "
           "machine B in a real QNX 8.0 guest, frozen with QMP `stop`, "
           "then `cont`</text><g transform='translate(0,32)'>%s</g>"
           "<g transform='translate(0,332)'>%s</g>"
           "<g transform='translate(0,427)'>%s</g><text x='16' y='%d' "
           "font-family='monospace' font-size='11'>%s</text></svg>"
           % (height, height, curve, strip, mandate, height - 20, line))
    path = os.path.join(out, "proof.svg")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(svg)
    return path


def copy_evidence(out: str, evidence: Optional[str], figures: Optional[str]) -> None:
    """Copy the proof files where the repository keeps its evidence."""
    tables = ((evidence, ("actuator.jsonl", "guest-console.log", "qnx-demo.json",
                          "qmp.json")),
              (figures, ("proof.svg", "guest-before.png", "guest-frozen.png",
                         "guest-after.png")))
    for directory, names in tables:
        if not directory:
            continue
        os.makedirs(directory, exist_ok=True)
        for name in names:
            src = os.path.join(out, name)
            if os.path.exists(src):
                shutil.copy(src, os.path.join(directory, "qnx-demo-" + name))


def prepare(args: argparse.Namespace) -> tuple[str, str, str]:
    """Create the output dir, check the host binaries, build the image."""
    out = os.path.abspath(args.out)
    shutil.rmtree(out, ignore_errors=True)
    run_dir = os.path.join(out, "run")
    os.makedirs(run_dir, exist_ok=True)
    for binary in ("relais-actuator", "relais-node", "relais-verify"):
        if not os.path.exists(os.path.join(ROOT, "build", binary)):
            raise QnxVmError("build/%s missing: run `make` first" % binary)
    print("== building the QNX 8.0 image (SDP mkqnximage) ==", file=sys.stderr)
    build_image(os.path.abspath(args.vm_dir), args.params, args.guest_ticks)
    return out, os.path.join(out, "actuator.jsonl"), run_dir

def finish_demo(out: str, journal: str, run_dir: str, vm: Optional[QnxVm],
                summary: dict, args: argparse.Namespace) -> None:
    """Analyse the journal, write the numbers, the proof and the evidence."""
    transcript = transcript_path(run_dir)
    if os.path.exists(transcript):
        with open(transcript, encoding="utf-8", errors="replace") as fh:
            summary["guest"] = parse_guest_console(fh.read())
    summary["journal"] = analyse_journal(journal)
    summary["verdict"] = run_verdict(journal)
    summary["verdict_disturbed_channel"] = run_verdict(journal, disturbed=True)
    summary["files"] = {"journal": journal, "transcript": transcript,
                         "proof": os.path.join(out, "proof.svg")}
    with open(os.path.join(out, "qnx-demo.json"), "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)
    with open(os.path.join(out, "qmp.json"), "w", encoding="utf-8") as fh:
        json.dump(summary.get("qmp", []), fh, indent=2)
    make_proof(out, journal, summary)
    copy_evidence(out, args.evidence, args.figures)
    print(json.dumps(summary, indent=2))


def run_demo(args: argparse.Namespace) -> int:
    """The whole demo: image, boot, arm, freeze, takeover, verdict."""
    out, journal, run_dir = prepare(args)
    tap = StatutTap(TAP_PORT, NODE_A_PORT).start()
    cmd = actuator_command(journal, args.ticks, args.params, args.trajectory)
    actuator, act_log = spawn(cmd, os.path.join(out, "actuator.log"))
    node_a: Optional[subprocess.Popen] = None
    vm: Optional[QnxVm] = None
    summary: dict = {"params": args.params, "ticks": args.ticks, "loadavg": list(os.getloadavg())}
    try:
        fwd = "udp:127.0.0.1:%d-:%d" % (GUEST_FWD_PORT, GUEST_NODE_PORT)
        vm = QnxVm(os.path.abspath(args.vm_dir), run_dir, hostfwd=[fwd],
                   cpus=2, ram="1G")
        vm.start()
        boot = vm.wait_shell(timeout=args.boot_timeout)
        summary.update(boot_seconds=round(boot, 1), accel=vm.accel)
        uname, _unused = vm.run("uname -a", timeout=20)
        summary["guest_uname"] = parse_guest_console(uname)["version"]
        print("guest shell up after %.1f s (accel=%s)" % (boot, vm.accel),
              file=sys.stderr)
        node_a = arm_and_freeze(vm, tap, args, out, run_dir, summary, actuator)
        print("== takeover done; the actuator seals its journal ==", file=sys.stderr)
        try:
            actuator.wait(timeout=args.finish_timeout)
        except subprocess.TimeoutExpired:
            terminate(actuator)
        summary["actuator_returncode"] = actuator.returncode
        summary["journal_sealed"] = actuator.returncode == 0
        summary["qmp"] = list(vm.qmp.history) if vm and vm.qmp else []
    finally:
        terminate(node_a)
        if vm is not None:
            vm.stop()
        tap.close()
        act_log.close()
    finish_demo(out, journal, run_dir, vm, summary, args)
    return 0


def _build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="runs/qnx-demo")
    ap.add_argument("--vm-dir", default=os.path.join(ROOT, "build", "qnx-vm"))
    ap.add_argument("--ticks", type=int, default=12000)
    ap.add_argument("--guest-ticks", type=int, default=40000)
    ap.add_argument("--params", default=DEFAULT_PARAMS)
    ap.add_argument("--trajectory", default=DEFAULT_TRAJ)
    ap.add_argument("--boot-timeout", type=float, default=300.0)
    ap.add_argument("--arm-timeout", type=float, default=180.0)
    ap.add_argument("--handover-timeout", type=float, default=30.0)
    ap.add_argument("--finish-timeout", type=float, default=8.0)
    ap.add_argument("--stable-ticks", type=int, default=15)
    ap.add_argument("--evidence", default=None)
    ap.add_argument("--figures", default=None)
    return ap


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        return run_demo(args)
    except QnxVmError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())