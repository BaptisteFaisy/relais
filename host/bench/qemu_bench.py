"""QEMU bench assembly: cable, actuator and the two QNX guests (INJ-01).

One object owns every process of a run, so cleanup is a single call and no
orphan can survive an exception.
"""
from __future__ import annotations

import json
import os
import signal
import subprocess
import time
from typing import Any, Callable, Optional

try:  # flat script directory or package
    from .bench_env import (ACTUATOR_HINT, IMAGE_HINT, BenchError, MissingImageError,
                            dep, find_binary, find_qnx_image, repo_root,
                            require_qnx_image)
    from .qemu import DEFAULT_MACS, QemuConfig, QemuError, QemuGuest, detect_kvm, qemu_version, tail_file
except ImportError:  # pragma: no cover
    from bench_env import (ACTUATOR_HINT, IMAGE_HINT, BenchError, MissingImageError,
                           dep, find_binary, find_qnx_image, repo_root,
                           require_qnx_image)
    from qemu import DEFAULT_MACS, QemuConfig, QemuError, QemuGuest, detect_kvm, qemu_version, tail_file

import json


def run_dir_for(campaign: str, root: Optional[str] = None) -> str:
    """Timestamped run directory, through common.py when it is available."""
    base = os.path.join(root or repo_root(), "out", campaign)
    try:
        common = dep("common")
    except BenchError:
        common = None
    for name in ("run_dir", "new_run_dir", "make_run_dir"):
        helper = getattr(common, name, None) if common else None
        if callable(helper):
            try:
                return str(helper(base))
            except TypeError:
                return str(helper(campaign, base))
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(base, stamp)
    os.makedirs(path, exist_ok=True)
    return path


class Actuator:
    """The actuator: a PC process at level S, a third QNX guest at level C."""

    def __init__(self, scenario: Any, run_dir: str, kind: str = "pc",
                 root: Optional[str] = None,
                 logger: Callable[[str], None] = print) -> None:
        self.scenario = scenario
        self.run_dir = run_dir
        self.kind = kind
        self.root = root or repo_root()
        self.logger = logger
        self.process: Optional[subprocess.Popen] = None
        self.guest: Optional[QemuGuest] = None
        self.binary = find_binary("relais-actuator", self.root)
        self.image = find_qnx_image(self.root)

    @property
    def journal_path(self) -> str:
        """Where the JSON-lines journal lands (file on PC, console on QNX)."""
        if self.kind == "qnx":
            return self.guest.console_path if self.guest else ""
        return os.path.join(self.run_dir, "actuator-journal.jsonl")

    def start(self) -> None:
        if self.kind == "qnx":
            self._start_guest()
            return
        if not self.binary:
            raise BenchError(ACTUATOR_HINT)
        command = self._command()
        self.logger(f"[actuator] {' '.join(command)}")
        handle = open(os.path.join(self.run_dir, "actuator.log"), "wb")
        self.process = subprocess.Popen(command, stdout=handle,
                                        stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL,
                                        start_new_session=True)
        handle.close()

    def _command(self) -> list:
        scenario = self.scenario
        args = [str(self.binary), f"--journal={self.journal_path}",
                f"--ticks={scenario.ticks}", f"--seed={scenario.seed}"]
        args += list(scenario.trajectory_args())
        for key, value in scenario.params_dict().items():
            args.append(f"--{key}={value}")
        return args

    def _start_guest(self) -> None:
        if not self.image:
            raise MissingImageError(IMAGE_HINT)
        config = QemuConfig(name="actuator", mac=DEFAULT_MACS["C"],
                            workdir=self.run_dir, image=self.image,
                            user_net=True)
        self.guest = QemuGuest(config, logger=self.logger).start()

    def stop(self, grace: float = 5.0) -> None:
        if self.guest is not None:
            self.guest.stop(grace)
            self.guest.cleanup()
            self.guest = None
        process, self.process = self.process, None
        if process is None or process.poll() is not None:
            return
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            process.wait(timeout=grace)
        except (OSError, subprocess.TimeoutExpired):
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            except OSError:
                pass
            process.wait(timeout=grace)

    @property
    def alive(self) -> bool:
        if self.guest is not None:
            return self.guest.alive
        return self.process is not None and self.process.poll() is None


class QemuBench:
    """The whole QEMU bench: cable, actuator, two QNX guests, fault plan."""

    def __init__(self, scenario: Any, run_dir: str, root: Optional[str] = None,
                 actuator_kind: str = "pc", use_cable: bool = True,
                 logger: Callable[[str], None] = print, net_model: str = "e1000",
                 memory_mb: int = 512, cpus: int = 1) -> None:
        self.scenario = scenario
        self.run_dir = run_dir
        self.root = root or repo_root()
        self.actuator_kind = actuator_kind
        self.use_cable = use_cable
        self.logger = logger
        self.net_model = net_model
        self.memory_mb = memory_mb
        self.cpus = cpus
        self.guests: dict[str, QemuGuest] = {}
        self.cable: Any = None
        self.actuator: Optional[Actuator] = None
        self.notes: list[str] = []
        self.applied: list[dict] = []
        self._pending: list[tuple] = []

    # -- start / stop -----------------------------------------------------
    def start(self) -> None:
        import host_net
        image = require_qnx_image(self.root)
        os.makedirs(self.run_dir, exist_ok=True)
        local_a, local_b = host_net.free_port(), host_net.free_port()
        peer_a, peer_b = self._start_cable(local_a, local_b, host_net)
        self.actuator = Actuator(self.scenario, self.run_dir,
                                 kind=self.actuator_kind, root=self.root,
                                 logger=self.logger)
        self.actuator.start()
        for name, mac, local, peer in (("A", DEFAULT_MACS["A"], local_a, peer_a),
                                       ("B", DEFAULT_MACS["B"], local_b, peer_b)):
            config = QemuConfig(name=name, mac=mac, workdir=self.run_dir,
                                image=image, links={"ab": (local, peer)},
                                net_model=self.net_model,
                                memory_mb=self.memory_mb, cpus=self.cpus)
            self.guests[name] = QemuGuest(config, logger=self.logger).start()
        self.write_manifest(image)

    def _start_cable(self, local_a: int, local_b: int, host_net: Any) -> tuple:
        """Programmable cable between the guests, or a plain dgram fallback."""
        if not self.use_cable:
            self.notes.append("cable disabled: plain dgram link A<->B")
            return local_b, local_a
        try:
            cable = dep("cable")
        except BenchError as exc:
            self.notes.append(f"cable.py unavailable ({exc}); plain dgram link")
            return local_b, local_a
        port_a, port_b = host_net.free_port(), host_net.free_port()
        control = os.path.join(self.run_dir, "cable.sock")
        self.cable = cable.Cable(port_a, port_b, control).start()
        return port_a, port_b

    def write_manifest(self, image: str) -> str:
        """INJ-06: one file describing the trial, so it can be replayed."""
        manifest = {
            "scenario": getattr(self.scenario, "name", "?"),
            "scenario_id": getattr(self.scenario, "id", 0),
            "seed": getattr(self.scenario, "seed", 0),
            "ticks": getattr(self.scenario, "ticks", 0),
            "params": self.scenario.params_dict(),
            "image": image, "qemu": qemu_version(), "kvm": detect_kvm(),
            "actuator": self.actuator_kind, "cable": bool(self.cable),
            "run_dir": self.run_dir, "notes": self.notes,
            "command_lines": {name: guest.command_line
                              for name, guest in self.guests.items()},
        }
        path = os.path.join(self.run_dir, "bench.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
        return path

    def stop(self) -> None:
        """Stop everything, guests first: no orphan process, ever."""
        for guest in self.guests.values():
            try:
                guest.stop(grace=3.0)
            except (QemuError, OSError) as exc:
                self.logger(f"stop failed: {exc}")
            finally:
                guest.cleanup()
        self.guests.clear()
        if self.actuator is not None:
            self.actuator.stop()
            self.actuator = None
        if self.cable is not None:
            try:
                self.cable.stop()
            finally:
                self.cable = None

    # -- readiness --------------------------------------------------------
    def wait_ready(self, timeout: float = 180.0) -> dict:
        """Wait until both machines are up and the actuator has a holder."""
        deadline = time.monotonic() + timeout
        ready = {name: False for name in self.guests}
        ready["actuator"] = False
        while time.monotonic() < deadline:
            self._refresh_ready(ready)
            if all(ready.values()):
                return ready
            self._check_alive()
            time.sleep(0.2)
        raise BenchError(f"bench not ready in {timeout}s: {ready}\n"
                         + self._ready_hint())

    def _refresh_ready(self, ready: dict) -> None:
        for name, guest in self.guests.items():
            if not ready[name] and _marker_seen(guest.console_path,
                                                guest.config.ready_markers):
                ready[name] = True
        if not ready["actuator"] and self.actuator is not None:
            ready["actuator"] = _journal_has_holder(self.actuator.journal_path)

    def _check_alive(self) -> None:
        for name, guest in self.guests.items():
            if not guest.alive:
                raise BenchError(f"guest {name} died before being ready:\n"
                                 f"{tail_file(guest.log_path)}")

    def _ready_hint(self) -> str:
        parts = []
        for name, guest in self.guests.items():
            parts.append(f"--- {name} console ---\n{tail_file(guest.console_path)}")
            parts.append(f"--- {name} qemu ---\n{tail_file(guest.log_path)}")
        return "\n".join(parts)


def _marker_seen(path: str, markers: tuple) -> bool:
    """True when any readiness marker already appears in a console log."""
    if not os.path.exists(path):
        return False
    wanted = tuple(marker.lower() for marker in markers)
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            lowered = line.lower()
            if any(marker in lowered for marker in wanted):
                return True
    return False


def _journal_has_holder(path: str) -> bool:
    """True when the actuator journal already names a holder (tick record)."""
    if not path or not os.path.exists(path):
        return False
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            if record.get("kind") == "tick" and record.get("holder"):
                return True
    return False
