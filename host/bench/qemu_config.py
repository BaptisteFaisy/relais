"""QEMU command line builder for the Relais bench (QEMU 8.2+, no root).

Everything that decides *how* a guest is launched lives here; QemuGuest (in
qemu.py) drives the process and talks QMP. Requirement IDs: OBS-04, INJ-07.
"""
from __future__ import annotations

import dataclasses
import os
import shutil
import subprocess
from typing import Optional

KVM_PATH = "/dev/kvm"
QEMU_BINARY = "qemu-system-x86_64"
DEBUG_EXIT_IOBASE = "0xf4"
DEBUG_EXIT_IOSIZE = "0x04"
MIN_PORT = 1024  # nothing below: no root on the host
DEFAULT_MACS = {"A": "52:54:00:00:0a:01", "B": "52:54:00:00:0b:02",
                "C": "52:54:00:00:0c:03"}
ROOT_ONLY_NETDEVS = ("tap", "bridge", "vhost-user", "vhost-vdpa", "l2tpv3")
READY_MARKERS = ("relais: ready", "relais ready", "certified", "holder")


class QemuError(RuntimeError):
    """Any failure of the QEMU layer, always with an actionable message."""


def detect_kvm(path: str = KVM_PATH) -> dict:
    """Probe KVM. Returns {"available": bool, "accel": ..., "reason": ...}."""
    if not os.path.exists(path):
        return {"available": False, "accel": "tcg",
                "reason": f"{path} missing: falling back to TCG (slow, "
                          "timings not usable for E2/E5)"}
    if not (os.access(path, os.R_OK) and os.access(path, os.W_OK)):
        return {"available": False, "accel": "tcg",
                "reason": f"{path} not readable/writable by this user "
                          "(no root allowed): falling back to TCG"}
    return {"available": True, "accel": "kvm", "reason": "kvm usable"}


@dataclasses.dataclass
class QemuConfig:
    """Everything needed to build one guest command line."""

    name: str
    mac: str
    workdir: str
    image: Optional[str] = None
    image_kind: str = "kernel"          # kernel | disk | cdrom
    memory_mb: int = 512
    cpus: int = 1
    machine: str = "q35"
    accel: Optional[str] = None         # None -> detect_kvm()
    cpu_model: Optional[str] = None     # None -> host (kvm) / max (tcg)
    net_model: str = "e1000"            # QNX 8.0 has an e1000 driver
    links: Optional[dict] = None        # id -> (local_port, peer_port)
    user_net: bool = True               # link towards the PC actuator
    user_hostfwd: Optional[list] = None
    pcap: bool = True                   # OBS-04: capture every link
    debug_exit: bool = True             # INJ-07: death points
    ready_markers: tuple = READY_MARKERS
    extra_args: Optional[list] = None
    kernel_args: str = ""

    def __post_init__(self) -> None:
        if self.links is None:
            self.links = {}
        for net_id, ports in self.links.items():
            for port in ports:
                if port < MIN_PORT:
                    raise QemuError(f"link {net_id} port {port} < {MIN_PORT}")
        if self.net_model not in ("e1000", "virtio-net-pci", "rtl8139"):
            raise QemuError(f"unsupported net model {self.net_model}")


def resolve_accel(cfg: QemuConfig) -> dict:
    """Fill in accel/cpu model, documenting the TCG fallback."""
    probe = detect_kvm()
    accel = cfg.accel or probe["accel"]
    if accel == "kvm" and not probe["available"]:
        raise QemuError("kvm requested but unavailable: " + probe["reason"])
    cpu = cfg.cpu_model or ("host" if accel == "kvm" else "max")
    return {"accel": accel, "cpu": cpu, "kvm": probe}


def build_command(cfg: QemuConfig, binary: str = QEMU_BINARY) -> list[str]:
    """Build the full qemu-system-x86_64 command line (see QEMU.md)."""
    resolved = resolve_accel(cfg)
    args = [binary, "-machine", cfg.machine, "-accel", resolved["accel"],
            "-cpu", resolved["cpu"], "-smp", str(cfg.cpus),
            "-m", str(cfg.memory_mb)]
    if resolved["accel"] == "kvm":
        args += ["-enable-kvm"]
    args += _image_args(cfg) + _link_args(cfg) + _capture_args(cfg)
    if cfg.debug_exit:
        args += ["-device", f"isa-debug-exit,iobase={DEBUG_EXIT_IOBASE},"
                            f"iosize={DEBUG_EXIT_IOSIZE}"]
    args += ["-nographic", "-serial", f"file:{serial_path(cfg)}",
             "-monitor", "none", "-no-reboot",
             "-qmp", f"unix:{qmp_path(cfg)},server=on,wait=off"]
    if cfg.extra_args:
        args += list(cfg.extra_args)
    return args


def serial_path(cfg: QemuConfig) -> str:
    """Path of the guest console log (also the /dev/relais/status window)."""
    return os.path.join(cfg.workdir, f"{cfg.name}-console.log")


def qmp_path(cfg: QemuConfig) -> str:
    """Path of the QMP Unix socket."""
    return os.path.join(cfg.workdir, f"{cfg.name}-qmp.sock")


def _image_args(cfg: QemuConfig) -> list[str]:
    if not cfg.image:
        return []
    if cfg.image_kind == "kernel":
        args = ["-kernel", cfg.image]
        if cfg.kernel_args:
            args += ["-append", cfg.kernel_args]
        return args
    if cfg.image_kind == "cdrom":
        return ["-cdrom", cfg.image]
    return ["-drive", f"file={cfg.image},format=raw,if=ide,index=0,media=disk"]


def _link_args(cfg: QemuConfig) -> list[str]:
    """dgram netdevs (inter-computer link) plus the user net (PC actuator)."""
    args: list[str] = []
    for net_id, (local_port, peer_port) in cfg.links.items():
        args += ["-netdev",
                 f"dgram,id={net_id},local.type=inet,"
                 f"local.host=127.0.0.1,local.port={local_port},"
                 f"remote.type=inet,remote.host=127.0.0.1,"
                 f"remote.port={peer_port}",
                 "-device", f"{cfg.net_model},netdev={net_id},mac={cfg.mac}"]
    if cfg.user_net:
        fwd = "".join(f",hostfwd={item}" for item in (cfg.user_hostfwd or []))
        args += ["-netdev", f"user,id=act,net=10.0.2.0/24,host=10.0.2.2{fwd}",
                 "-device", f"{cfg.net_model},netdev=act,mac={cfg.mac}"]
    return args


def _capture_args(cfg: QemuConfig) -> list[str]:
    """OBS-04: one filter-dump per netdev, written inside the run directory."""
    if not cfg.pcap:
        return []
    args: list[str] = []
    for net_id in list(cfg.links) + (["act"] if cfg.user_net else []):
        args += ["-object", f"filter-dump,id=dump-{net_id},netdev={net_id},"
                            f"file={pcap_paths(cfg)[net_id]}"]
    return args


def pcap_paths(cfg: QemuConfig) -> dict[str, str]:
    """Map netdev id -> pcap file, for every captured link."""
    ids = list(cfg.links) + (["act"] if cfg.user_net else [])
    return {net_id: os.path.join(cfg.workdir, f"{cfg.name}-{net_id}.pcap")
            for net_id in ids}


def check_no_root_needed(args: list[str]) -> list[str]:
    """Return the reasons why *args* would need root (empty when clean)."""
    problems: list[str] = []
    for arg in args:
        lowered = arg.lower()
        for backend in ROOT_ONLY_NETDEVS:
            if lowered.startswith(f"{backend},") or f",{backend}," in lowered:
                problems.append(f"netdev {backend} needs root")
        problems += _privileged_ports(lowered)
    return problems


def _privileged_ports(lowered: str) -> list[str]:
    """Flag any port below 1024 in dgram/hostfwd options."""
    out: list[str] = []
    for token in lowered.split(","):
        for key in ("local.port", "remote.port"):
            if token.startswith(f"{key}=") and token.split("=")[1].isdigit():
                port = int(token.split("=", 1)[1])
                if port < MIN_PORT:
                    out.append(f"{key}={port} < {MIN_PORT}")
    for part in lowered.split("hostfwd=")[1:]:
        host_side = part.split(",")[0].split("-")[0]
        token = host_side.split(":")[-1]
        if token.isdigit() and int(token) < MIN_PORT:
            out.append(f"hostfwd host port {token} < {MIN_PORT}")
    return out


def find_qemu_binary(name: str = QEMU_BINARY) -> Optional[str]:
    """Locate the QEMU binary, or None when it is not installed."""
    return shutil.which(name)


def qemu_version(binary: str = QEMU_BINARY, timeout: int = 10) -> str:
    """Return `qemu-system-x86_64 --version` first line ('' when absent)."""
    path = find_qemu_binary(binary)
    if not path:
        return ""
    try:
        done = subprocess.run([path, "--version"], capture_output=True,
                              text=True, timeout=timeout, check=False)
    except (OSError, subprocess.SubprocessError):
        return ""
    text = done.stdout or done.stderr
    return text.splitlines()[0] if text else ""
