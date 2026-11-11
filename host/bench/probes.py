"""Feasibility probes E1..E6 (spec section 10), each with an explicit verdict.

Every probe returns a verdict dict and never raises: OK / ECHEC / BLOQUE, plus
the measurement it obtained. Without a QNX image every probe reports BLOQUE
and says how to build one -- that is the state of the reference PC.
"""
from __future__ import annotations

import os
import time
from typing import Any, Callable, Optional

try:  # flat script directory or package
    from .bench_env import (ACTUATOR_HINT, IMAGE_HINT, BenchError, STATUS_BLOCKED,
                            STATUS_FAIL, STATUS_OK, find_qnx_image, repo_root,
                            verdict)
    from .qemu import QemuConfig, QemuError, QemuGuest, detect_kvm, qemu_version
except ImportError:  # pragma: no cover
    from bench_env import (ACTUATOR_HINT, IMAGE_HINT, BenchError, STATUS_BLOCKED,
                           STATUS_FAIL, STATUS_OK, find_qnx_image, repo_root,
                           verdict)
    from qemu import QemuConfig, QemuError, QemuGuest, detect_kvm, qemu_version

import host_net

PROBE_IDS = ("E1", "E2", "E3", "E4", "E5", "E6")
PROBE_TITLES = {
    "E1": "two QNX guests exchange datagrams through a PC relay, no root",
    "E2": "guest<->PC UDP round trip, median and worst case, with KVM",
    "E3": "what the guest clock sees after stop/cont",
    "E4": "I/O port access and death by isa-debug-exit",
    "E5": "STATUT->order delay under load and network stack priority",
    "E6": "launch of QEMU to a ready machine",
}
DEATH_EXIT_CODES = {1: "isa-debug-exit value 0x00", 3: "isa-debug-exit value 0x01"}


class ProbeContext:
    """Everything a probe needs to run, built once by the CLI."""

    def __init__(self, root: Optional[str] = None, workdir: Optional[str] = None,
                 logger: Callable[[str], None] = print,
                 timeout: float = 120.0) -> None:
        self.root = root or repo_root()
        self.workdir = workdir or os.path.join(self.root, "out", "probe")
        self.logger = logger
        self.timeout = timeout
        self.image = find_qnx_image(self.root)
        self.kvm = detect_kvm()
        self.version = qemu_version()

    def guest(self, name: str = "probe", **overrides: Any) -> QemuGuest:
        """A guest configuration for a short probe run."""
        try:
            from .qemu import DEFAULT_MACS
        except ImportError:  # pragma: no cover
            from qemu import DEFAULT_MACS
        os.makedirs(self.workdir, exist_ok=True)
        config = QemuConfig(name=name, mac=DEFAULT_MACS["A"],
                            workdir=self.workdir, image=self.image,
                            memory_mb=256, **overrides)
        return QemuGuest(config, logger=self.logger)

    def blocked(self, probe: str, detail: str, **fields: Any) -> dict:
        """A clean BLOQUE verdict, never an exception."""
        return verdict(STATUS_BLOCKED, f"probe {probe}", label=f"{probe} "
                       f"{PROBE_TITLES[probe]}", detail=detail,
                       notes=[IMAGE_HINT] if "QNX image" in detail else [],
                       **fields)


def _no_image(ctx: ProbeContext, probe: str, extra: str = "") -> dict:
    """BLOQUE verdict used by every probe that needs a guest."""
    detail = ("QNX image missing: the probe needs a bootable QNX 8.0 guest. "
              + extra).strip()
    return ctx.blocked(probe, detail)


def _findings(ctx: ProbeContext) -> dict:
    """Host facts every probe reports, so a BLOQUE still carries data."""
    return {"qemu": ctx.version or "qemu-system-x86_64 not found",
            "kvm": ctx.kvm, "image": ctx.image or "missing",
            "image_hint": IMAGE_HINT}


def probe_e1(ctx: ProbeContext) -> dict:
    """E1: two guests exchange datagrams through the PC relay, no root."""
    probe = "E1"
    if not ctx.image:
        return _no_image(ctx, probe, "Fallback plan must then be assessed "
                                     "with -netdev socket,mcast.")
    port_a, port_b = host_net.free_port(), host_net.free_port()
    relay_a, relay_b = host_net.free_port(), host_net.free_port()
    relay = host_net.UdpRelay(port_a, port_b, relay_a, relay_b).start()
    guests: list[QemuGuest] = []
    try:
        for name, mac, local, remote in (("A", "52:54:00:00:0a:01", port_a, relay_a),
                                         ("B", "52:54:00:00:0b:02", port_b, relay_b)):
            guest = ctx.guest(f"e1-{name}", mac=mac, links={"ab": (local, remote)},
                              user_net=False)
            guests.append(guest.start())
            guest.wait_ready(min(60.0, ctx.timeout),
                             markers=("relais: ready", "net ready"))
        time.sleep(1.0)
        sizes = {guest.name: _pcap_size(guest.pcap_path) for guest in guests}
        ok = all(size > 0 for size in sizes.values())
        return verdict(STATUS_OK if ok else STATUS_FAIL, f"probe {probe}",
                       label=f"{probe} {PROBE_TITLES[probe]}",
                       detail=("frames captured on both links" if ok else
                               "no frame captured on at least one link"),
                       measures={"pcap_bytes": sizes,
                                 "relay_forwarded": relay.forwarded,
                                 "rtt": host_net.measure_rtt(
                                     ("127.0.0.1", port_b), count=50)
                                 if ok else {}},
                       runs=_findings(ctx))
    finally:
        for guest in guests:
            guest.cleanup()
        relay.stop()


def _pcap_size(path: str) -> int:
    """Size of a pcap file, 0 when QEMU has not written it yet."""
    return os.path.getsize(path) if path and os.path.exists(path) else 0


def probe_e2(ctx: ProbeContext) -> dict:
    """E2: guest<->PC UDP round trip, median and worst case, with KVM."""
    probe = "E2"
    if not ctx.image:
        echo = host_net.UdpEchoServer().start()
        try:
            local_rtt = host_net.measure_rtt(echo.addr, count=200)
        finally:
            echo.stop()
        return _no_image(ctx, probe, "PC loopback reference measured meanwhile: "
                                     f"median {local_rtt.get('median')} ms, "
                                     f"max {local_rtt.get('max')} ms.")
    port = host_net.free_port()
    guest = ctx.guest("e2", user_net=True,
                      user_hostfwd=[f"udp:127.0.0.1:{port}-:5000"])
    try:
        guest.start()
        guest.wait_ready(min(90.0, ctx.timeout),
                         markers=("relais: ready", "probe echo ready"))
        rtt = host_net.measure_rtt(("127.0.0.1", port), count=200)
        ok = rtt.get("count", 0) > 0
        return verdict(STATUS_OK if ok else STATUS_FAIL, f"probe {probe}",
                       label=f"{probe} {PROBE_TITLES[probe]}",
                       detail=("median/worst measured" if ok else
                               "no answer from the guest echo (is "
                               "relais-probe-echo running on UDP 5000?)"),
                       measures={"rtt": rtt, "kvm": ctx.kvm["accel"]},
                       runs=_findings(ctx))
    finally:
        guest.cleanup()


def probe_e3(ctx: ProbeContext) -> dict:
    """E3: what the guest clock sees after stop/cont."""
    probe = "E3"
    if not ctx.image:
        return _no_image(ctx, probe, "The answer only shapes the demo story: "
                                     "the protocol is safe either way (I5).")
    guest = ctx.guest("e3", user_net=False)
    try:
        guest.start()
        guest.wait_ready(min(90.0, ctx.timeout))
        before = guest.status()
        frozen_at = time.monotonic()
        guest.freeze()
        time.sleep(1.0)
        during = guest.status()
        guest.thaw()
        thawed_at = time.monotonic()
        after = guest.status()
        measures = {"status_before": before, "status_frozen": during,
                    "status_after": after,
                    "host_frozen_s": round(thawed_at - frozen_at, 4),
                    "guest_packets_while_frozen": _pcap_size(guest.pcap_path)}
        return verdict(STATUS_OK, f"probe {probe}",
                       label=f"{probe} {PROBE_TITLES[probe]}",
                       detail="stop holds the vCPU; cont resumes it without "
                              "catch-up (compare the console timestamps)",
                       measures=measures, runs=_findings(ctx))
    finally:
        guest.cleanup()


def probe_e4(ctx: ProbeContext) -> dict:
    """E4: I/O port access and death by isa-debug-exit."""
    probe = "E4"
    if not ctx.image:
        return _no_image(ctx, probe, "the device is accepted on this host; the "
                                     "in-guest outb to 0xf4 is what E4 checks")
    guest = ctx.guest("e4", kernel_args="relais.death_at=P1")
    try:
        guest.start()
        code = _wait_exit(guest, min(90.0, ctx.timeout))
        label = DEATH_EXIT_CODES.get(code, f"exit code {code}")
        ok = code in DEATH_EXIT_CODES
        return verdict(STATUS_OK if ok else STATUS_FAIL, f"probe {probe}",
                       label=f"{probe} {PROBE_TITLES[probe]}",
                       detail=f"QEMU left with {label}",
                       measures={"exit_code": code},
                       runs=_findings(ctx))
    finally:
        guest.cleanup()


def _wait_exit(guest: QemuGuest, timeout: float) -> int:
    """Wait for the guest process to leave, returning its exit code."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not guest.alive:
            return int(guest.process.returncode or 0) if guest.process else 0
        time.sleep(0.1)
    guest.kill()
    return -1


def probe_e5(ctx: ProbeContext) -> dict:
    """E5: STATUT->order delay under load, and network stack priority."""
    probe = "E5"
    if not ctx.image:
        return _no_image(ctx, probe, "needs the in-guest trace (OBS-03): tick "
                                     "events are only visible inside QNX")
    return ctx.blocked(probe, "requires the QNX trace events OBS-03 in a "
                              "running guest; run the bench in mode qemu and "
                              "read the journal events instead.",
                       runs=_findings(ctx))


def probe_e6(ctx: ProbeContext) -> dict:
    """E6: launch of QEMU to a ready machine."""
    probe = "E6"
    launched = time.monotonic()
    guest = ctx.guest("e6", user_net=False)
    qmp_ms: Optional[float] = None
    try:
        guest.start()
        qmp_ms = round((time.monotonic() - launched) * 1e3, 1)
        if not ctx.image:
            return ctx.blocked(
                probe, "QNX image missing: cannot measure the boot to ready. "
                       f"QEMU itself reached QMP in {qmp_ms} ms (no guest "
                       "booted).", measures={"qemu_to_qmp_ms": qmp_ms},
                runs=_findings(ctx))
        marker = guest.wait_ready(min(120.0, ctx.timeout))
        ready_ms = round((time.monotonic() - launched) * 1e3, 1)
        return verdict(STATUS_OK, f"probe {probe}",
                       label=f"{probe} {PROBE_TITLES[probe]}",
                       detail=f"ready on marker {marker!r}",
                       measures={"qemu_to_ready_ms": ready_ms,
                                 "qemu_to_qmp_ms": qmp_ms},
                       runs=_findings(ctx))
    finally:
        guest.cleanup()


PROBES = {"E1": probe_e1, "E2": probe_e2, "E3": probe_e3,
          "E4": probe_e4, "E5": probe_e5, "E6": probe_e6}


def run_probe(name: str, ctx: ProbeContext) -> dict:
    """Run one probe by id; any failure becomes a readable ECHEC verdict."""
    probe = (name or "").upper()
    if probe not in PROBES:
        raise BenchError(f"unknown probe {name!r}: available {list(PROBE_IDS)}")
    try:
        return PROBES[probe](ctx)
    except (BenchError, QemuError, host_net.NetError, OSError) as exc:
        return verdict(STATUS_FAIL, f"probe {probe}",
                       label=f"{probe} {PROBE_TITLES[probe]}",
                       detail=f"{type(exc).__name__}: {exc}")


def run_all(ctx: ProbeContext, only: Optional[list[str]] = None) -> list[dict]:
    """Run the six probes (or a subset) and return their verdicts."""
    wanted = [name.upper() for name in (only or PROBE_IDS)]
    return [run_probe(name, ctx) for name in wanted if name in PROBE_IDS]
