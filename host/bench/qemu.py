"""QEMU guest driver for the Relais bench (QEMU 8.2+, x86_64, no root).

Drives one guest process: QMP over a Unix socket, a serialised command queue,
console readiness detection, and a cleanup path that leaves no orphan and no
stale socket behind. The command line itself is built in qemu_config.py.
Requirement IDs: INJ-01, INJ-02, INJ-03, INJ-04, INJ-07, OBS-04.
"""
from __future__ import annotations

import os
import queue
import signal
import subprocess
import threading
import time
from typing import Any, Callable, Optional

try:  # works both inside a package and as a flat script directory
    from .qmp import QmpClient, QmpError, QmpTimeout
    from .qemu_config import (DEFAULT_MACS, DEBUG_EXIT_IOBASE, KVM_PATH,
                              MIN_PORT, QEMU_BINARY, QemuConfig, QemuError,
                              READY_MARKERS, ROOT_ONLY_NETDEVS, build_command,
                              check_no_root_needed, detect_kvm, find_qemu_binary,
                              pcap_paths, qmp_path, qemu_version, resolve_accel,
                              serial_path)
except ImportError:  # pragma: no cover - flat layout
    from qmp import QmpClient, QmpError, QmpTimeout
    from qemu_config import (DEFAULT_MACS, DEBUG_EXIT_IOBASE, KVM_PATH,
                             MIN_PORT, QEMU_BINARY, QemuConfig, QemuError,
                             READY_MARKERS, ROOT_ONLY_NETDEVS, build_command,
                             check_no_root_needed, detect_kvm, find_qemu_binary,
                             pcap_paths, qmp_path, qemu_version, resolve_accel,
                             serial_path)

__all__ = ["QemuGuest", "QemuConfig", "QemuError", "build_command",
           "detect_kvm", "qemu_version", "find_qemu_binary", "DEFAULT_MACS",
           "QEMU_BINARY", "MIN_PORT", "pcap_paths", "serial_path", "qmp_path",
           "check_no_root_needed", "tail_file", "resolve_accel", "KVM_PATH",
           "READY_MARKERS", "ROOT_ONLY_NETDEVS", "DEBUG_EXIT_IOBASE",
           "QmpClient", "QmpError", "QmpTimeout"]


class _Job:
    """One queued QMP command plus its outcome."""

    def __init__(self, command: str, arguments: dict, timeout: float) -> None:
        self.command = command
        self.arguments = arguments
        self.timeout = timeout
        self.done = threading.Event()
        self.result: Any = None
        self.error: Optional[BaseException] = None


class QemuGuest:
    """One QEMU guest: process, QMP queue, console log and link captures."""

    def __init__(self, config: QemuConfig, binary: str = QEMU_BINARY,
                 qmp_timeout: float = 5.0, connect_wait: float = 20.0,
                 logger: Optional[Callable[[str], None]] = None) -> None:
        self.config = config
        self.binary = binary
        self.qmp_timeout = qmp_timeout
        self.connect_wait = connect_wait
        self.logger = logger or (lambda _line: None)
        self.command_line: list[str] = []
        self.process: Optional[subprocess.Popen] = None
        self.started_at: Optional[float] = None
        self.ready_after: Optional[float] = None
        self._client: Optional[QmpClient] = None
        self._queue: "queue.Queue[Optional[_Job]]" = queue.Queue()
        self._worker: Optional[threading.Thread] = None
        self._stopping = threading.Event()

    # -- paths ------------------------------------------------------------
    @property
    def name(self) -> str:
        return self.config.name

    @property
    def pid(self) -> Optional[int]:
        return self.process.pid if self.process else None

    @property
    def log_path(self) -> str:
        """QEMU's own stdout/stderr (boot errors, option errors)."""
        return os.path.join(self.config.workdir, f"{self.name}-qemu.log")

    @property
    def console_path(self) -> str:
        """Guest serial console: where /dev/relais/status lines show up."""
        return serial_path(self.config)

    @property
    def qmp_socket(self) -> str:
        return qmp_path(self.config)

    @property
    def pcap_paths(self) -> dict[str, str]:
        return pcap_paths(self.config)

    @property
    def pcap_path(self) -> str:
        """Capture of the main link (inter-computer link, else PC link)."""
        paths = self.pcap_paths
        for net_id in self.config.links:
            if net_id in paths:
                return paths[net_id]
        return next(iter(paths.values()), "")

    @property
    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    # -- lifecycle --------------------------------------------------------
    def start(self) -> "QemuGuest":
        """Spawn QEMU, connect QMP and start the command queue worker."""
        os.makedirs(self.config.workdir, exist_ok=True)
        for stale in (self.qmp_socket,):
            if os.path.exists(stale):
                os.unlink(stale)
        self.command_line = build_command(self.config, self.binary)
        problems = check_no_root_needed(self.command_line)
        if problems:
            raise QemuError(f"command line needs root: {problems}")
        self.logger(f"[{self.name}] {' '.join(self.command_line)}")
        log_file = open(self.log_path, "wb")
        try:
            self.process = subprocess.Popen(
                self.command_line, stdout=log_file, stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL, start_new_session=True)
        finally:
            log_file.close()
        self.started_at = time.monotonic()
        self._stopping.clear()
        self._worker = threading.Thread(target=self._serve_queue, daemon=True,
                                        name=f"qmp-{self.name}")
        self._worker.start()
        self._connect()
        return self

    def _connect(self) -> None:
        self._client = QmpClient(self.qmp_socket, self.qmp_timeout, self.logger)
        try:
            self._client.connect(self.connect_wait)
        except QmpError as exc:
            self.cleanup()
            raise QemuError(f"[{self.name}] QMP unavailable: {exc}\n"
                            f"log tail:\n{tail_file(self.log_path)}") from exc

    def _serve_queue(self) -> None:
        """Serialise QMP commands: one at a time, each logged, each timed."""
        while not self._stopping.is_set():
            try:
                job = self._queue.get(timeout=0.1)
            except queue.Empty:
                continue
            if job is None:
                break
            try:
                job.result = self._run_job(job)
            except BaseException as exc:  # noqa: BLE001 - surfaced to caller
                job.error = exc
            finally:
                job.done.set()

    def _run_job(self, job: _Job) -> Any:
        if self._client is None:
            raise QemuError(f"[{self.name}] not connected")
        self._client.timeout = job.timeout
        try:
            return self._client.execute(job.command, **job.arguments)
        except QmpTimeout:
            self._reconnect()
            raise
        except QmpError:
            if self.alive:
                self._reconnect()
            raise

    def _reconnect(self) -> None:
        """Resume after a broken or stalled QMP connection."""
        if self._client is not None:
            self._client.close()
            self._client = None
        if self.alive:
            try:
                self._connect()
            except QemuError as exc:
                self.logger(f"[{self.name}] reconnect failed: {exc}")

    def qmp(self, command: str, timeout: Optional[float] = None,
            **arguments: Any) -> Any:
        """Queue one QMP command and wait for its answer."""
        if self._worker is None:
            raise QemuError(f"[{self.name}] guest not started")
        job = _Job(command, arguments, timeout or self.qmp_timeout)
        self._queue.put(job)
        if not job.done.wait(job.timeout + 2.0):
            raise QmpTimeout(f"[{self.name}] {command} stuck in the queue")
        if job.error is not None:
            raise job.error
        return job.result

    def freeze(self) -> Any:
        """INJ-03: stop the vCPU (the guest clock stops with it)."""
        return self.qmp("stop")

    def thaw(self) -> Any:
        """INJ-03: resume the vCPU."""
        return self.qmp("cont")

    def reset(self) -> Any:
        """Warm reboot of the guest (INJ-02 recovery path)."""
        return self.qmp("system_reset")

    def set_link(self, name: str, up: bool) -> Any:
        """INJ-04: unplug/replug a netdev without touching the guest."""
        return self.qmp("set_link", name=name, up=bool(up))

    def status(self) -> Any:
        return self.qmp("query-status")

    def wait_ready(self, timeout: float = 120.0,
                   markers: Optional[tuple] = None) -> str:
        """Wait for a readiness marker on the guest console."""
        wanted = tuple(m.lower() for m in (markers or self.config.ready_markers))
        deadline = time.monotonic() + timeout
        seen = 0
        while time.monotonic() < deadline:
            seen, hit = _scan_markers(self.console_path, wanted, seen)
            if hit:
                self.ready_after = time.monotonic() - (self.started_at or 0)
                return hit
            if not self.alive:
                raise QemuError(f"[{self.name}] died before becoming ready:\n"
                                f"{tail_file(self.log_path)}")
            time.sleep(0.2)
        raise QemuError(f"[{self.name}] not ready in {timeout}s "
                        f"(markers {wanted}); console tail:\n"
                        f"{tail_file(self.console_path)}")

    def stop(self, grace: float = 5.0) -> int:
        """Orderly death: QMP quit, then TERM, then KILL. Returns the code."""
        if self.alive:
            try:
                self.qmp("quit", timeout=min(3.0, grace))
            except (QemuError, QmpError) as exc:
                self.logger(f"[{self.name}] quit failed ({exc}), signalling")
        return self._terminate(grace)

    def kill(self) -> int:
        """INJ-02 brutal death: SIGKILL, no QMP, nothing flushed."""
        return self._terminate(0.0, force=True)

    def _terminate(self, grace: float, force: bool = False) -> int:
        process, self.process = self.process, None
        if process is None:
            return 0
        code = process.poll()
        if code is None:
            _signal_group(process, signal.SIGKILL if force or grace <= 0
                          else signal.SIGTERM)
            try:
                code = process.wait(timeout=max(grace, 2.0))
            except subprocess.TimeoutExpired:
                _signal_group(process, signal.SIGKILL)
                code = process.wait(timeout=5.0)
        self.logger(f"[{self.name}] exited with code {code}")
        return int(code or 0)

    def cleanup(self) -> None:
        """No orphan, no stale socket: safe to call twice."""
        self._stopping.set()
        worker, self._worker = self._worker, None
        if worker is not None:
            self._queue.put(None)
            worker.join(timeout=2.0)
        client, self._client = self._client, None
        if client is not None:
            client.close()
        self._terminate(2.0)
        for path in (self.qmp_socket,):
            try:
                if os.path.exists(path):
                    os.unlink(path)
            except OSError:
                pass

    def __enter__(self) -> "QemuGuest":
        return self.start()

    def __exit__(self, *_exc: object) -> None:
        self.cleanup()


def _signal_group(process: subprocess.Popen, sig: int) -> None:
    """Signal the whole process group so QEMU cannot leave children behind."""
    try:
        os.killpg(os.getpgid(process.pid), sig)
    except (ProcessLookupError, PermissionError, OSError):
        try:
            process.send_signal(sig)
        except (ProcessLookupError, OSError):
            pass


def _scan_markers(path: str, wanted: tuple, seen: int) -> tuple[int, str]:
    """Scan new console lines for a readiness marker."""
    if not os.path.exists(path):
        return seen, ""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()
    for line in lines[seen:]:
        lowered = line.lower()
        for marker in wanted:
            if marker in lowered:
                return len(lines), marker
    return len(lines), ""


def tail_file(path: str, lines: int = 12) -> str:
    """Last *lines* of a file, for error messages."""
    if not os.path.exists(path):
        return f"(no such file: {path})"
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return "".join(handle.readlines()[-lines:]) or "(empty)"
