"""Small PC-side network helpers for the Relais bench (probes E1/E2/E5).

Stdlib only, no root, no port below 1024. Everything here is non blocking and
driven by ``select`` so a probe can time out instead of hanging.
"""
from __future__ import annotations

import errno
import select
import socket
import statistics
import threading
import time
from typing import Optional, Tuple

MIN_PORT = 1024
DEFAULT_HOST = "127.0.0.1"
RECV_SIZE = 2048


class NetError(RuntimeError):
    """Raised when a network operation cannot be done without root."""


def check_port(port: int) -> int:
    """Reject privileged ports: the bench never needs root (0 = ephemeral)."""
    if port and port < MIN_PORT:
        raise NetError(f"port {port} < {MIN_PORT}: privileged ports need root")
    return port


def free_port(host: str = DEFAULT_HOST) -> int:
    """Return an unused UDP port on *host* (>= MIN_PORT)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((host, 0))
        return check_port(sock.getsockname()[1])
    finally:
        sock.close()


def open_udp(host: str = DEFAULT_HOST, port: int = 0,
             blocking: bool = False) -> socket.socket:
    """Open a UDP socket, non blocking by default."""
    check_port(port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(blocking)
    sock.bind((host, port))
    return sock


def wait_readable(sock: socket.socket, timeout: float) -> bool:
    """True when *sock* has data, False on timeout (never blocks forever)."""
    ready, _, _ = select.select([sock], [], [], max(0.0, timeout))
    return bool(ready)


def recv(sock: socket.socket, timeout: float,
         size: int = RECV_SIZE) -> Optional[Tuple[bytes, Tuple[str, int]]]:
    """Receive one datagram with a timeout, or None when nothing came."""
    if not wait_readable(sock, timeout):
        return None
    try:
        return sock.recvfrom(size)
    except BlockingIOError:
        return None
    except OSError as exc:  # pragma: no cover - rare on loopback
        if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
            return None
        raise


def send(sock: socket.socket, data: bytes, addr: Tuple[str, int]) -> int:
    """Send one datagram, waiting for the socket to become writable."""
    _, ready, _ = select.select([], [sock], [], 1.0)
    if not ready:
        raise NetError(f"socket not writable towards {addr}")
    return sock.sendto(data, addr)


def stats(samples: list[float], unit: float = 1e3) -> dict:
    """Summarise timings (seconds in, *unit* out; default milliseconds)."""
    if not samples:
        return {"count": 0}
    ordered = sorted(samples)

    def scaled(value: float) -> float:
        return round(value * unit, 4)

    return {
        "count": len(ordered),
        "unit": "ms",
        "min": scaled(ordered[0]),
        "median": scaled(statistics.median(ordered)),
        "p95": scaled(_percentile(ordered, 95)),
        "p99": scaled(_percentile(ordered, 99)),
        "max": scaled(ordered[-1]),
        "mean": scaled(statistics.fmean(ordered)),
    }


def _percentile(ordered: list[float], pct: float) -> float:
    """Nearest-rank percentile over an already sorted list."""
    if len(ordered) == 1:
        return ordered[0]
    idx = min(len(ordered) - 1, max(0, int(round(pct / 100.0 * len(ordered))) - 1))
    return ordered[idx]


def round_trip(sock: socket.socket, addr: Tuple[str, int], payload: bytes,
               timeout: float) -> Optional[float]:
    """One UDP round trip, monotonic clock, None when the answer is lost."""
    started = time.monotonic()
    send(sock, payload, addr)
    got = recv(sock, timeout)
    if got is None:
        return None
    return time.monotonic() - started


def measure_rtt(addr: Tuple[str, int], count: int = 200,
                payload: bytes = b"relais-probe", timeout: float = 0.25,
                interval: float = 0.0) -> dict:
    """Measure UDP round trips towards *addr* and summarise them."""
    sock = open_udp(DEFAULT_HOST, 0)
    samples: list[float] = []
    lost = 0
    try:
        for _ in range(count):
            elapsed = round_trip(sock, addr, payload, timeout)
            if elapsed is None:
                lost += 1
            else:
                samples.append(elapsed)
            if interval:
                time.sleep(interval)
    finally:
        sock.close()
    result = stats(samples)
    result["lost"] = lost
    result["sent"] = count
    return result


class UdpEchoServer:
    """Threaded UDP echo server used as the PC side of E1/E2/E5."""

    def __init__(self, host: str = DEFAULT_HOST, port: int = 0,
                 delay: float = 0.0) -> None:
        self.sock = open_udp(host, port)
        self.addr: Tuple[str, int] = self.sock.getsockname()
        self.delay = delay
        self.served = 0
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> "UdpEchoServer":
        """Start serving in a daemon thread."""
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self

    def _serve(self) -> None:
        while not self._stop.is_set():
            got = recv(self.sock, 0.2)
            if got is None:
                continue
            data, peer = got
            if self.delay:
                time.sleep(self.delay)
            send(self.sock, data, peer)
            self.served += 1

    def stop(self, timeout: float = 2.0) -> None:
        """Stop the server and close the socket (no thread left behind)."""
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout)
            self._thread = None
        self.sock.close()

    def __enter__(self) -> "UdpEchoServer":
        return self.start()

    def __exit__(self, *_exc: object) -> None:
        self.stop()


class UdpRelay:
    """Forward UDP both ways between two guests: the PC relay fallback of E1.

    Guest A points its dgram ``remote.port`` at ``port_a``; the relay sends
    what it receives there to guest B's local port (``target_b``), and the
    other way round for ``port_b``/``target_a``.
    """

    def __init__(self, target_a: int, target_b: int, port_a: int = 0,
                 port_b: int = 0, host: str = DEFAULT_HOST) -> None:
        self.sock_a = open_udp(host, port_a)
        self.sock_b = open_udp(host, port_b)
        self.port_a = self.sock_a.getsockname()[1]
        self.port_b = self.sock_b.getsockname()[1]
        self.target_a = check_port(target_a)
        self.target_b = check_port(target_b)
        self.forwarded = {"a_to_b": 0, "b_to_a": 0}
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []

    def start(self) -> "UdpRelay":
        """Start one pump per direction."""
        for sock, target, key in ((self.sock_a, self.target_b, "a_to_b"),
                                  (self.sock_b, self.target_a, "b_to_a")):
            thread = threading.Thread(target=self._pump,
                                      args=(sock, target, key), daemon=True)
            thread.start()
            self._threads.append(thread)
        return self

    def _pump(self, source: socket.socket, target: int, key: str) -> None:
        while not self._stop.is_set():
            got = recv(source, 0.2)
            if got is None:
                continue
            data, _peer = got
            try:
                send(source, data, (DEFAULT_HOST, target))
                self.forwarded[key] += 1
            except NetError:
                continue

    def stop(self, timeout: float = 2.0) -> None:
        """Stop the pumps and close both sockets."""
        self._stop.set()
        for thread in self._threads:
            thread.join(timeout)
        self._threads.clear()
        self.sock_a.close()
        self.sock_b.close()

    def __enter__(self) -> "UdpRelay":
        return self.start()

    def __exit__(self, *_exc: object) -> None:
        self.stop()
