"""QMP (QEMU Machine Protocol) client over a Unix socket, stdlib only.

One connection, a JSON-lines conversation, timeouts on every read, and a log
of every command sent and answer received so a run can be replayed on paper.
"""
from __future__ import annotations

import json
import os
import socket
import time
from typing import Any, Callable, Optional


class QmpError(RuntimeError):
    """QMP answered with an error, or the conversation broke."""


class QmpTimeout(QmpError):
    """No answer inside the deadline."""


class QmpClient:
    """Minimal QMP client: hello, qmp_capabilities, then commands."""

    def __init__(self, path: str, timeout: float = 5.0,
                 log: Optional[Callable[[str], None]] = None) -> None:
        self.path = path
        self.timeout = timeout
        self.log = log or (lambda _line: None)
        self._sock: Optional[socket.socket] = None
        self._buffer = b""
        self.history: list[dict] = []

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def connect(self, wait: float = 15.0) -> None:
        """Wait for the socket, read the greeting, enable capabilities."""
        self._wait_socket(wait)
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(self.timeout)
        try:
            self._sock.connect(self.path)
        except OSError as exc:
            self.close()
            raise QmpError(f"cannot connect to {self.path}: {exc}") from exc
        greeting = self._read_json(self.timeout)
        if not isinstance(greeting, dict) or "QMP" not in greeting:
            raise QmpError(f"bad QMP greeting: {greeting!r}")
        self.execute("qmp_capabilities")

    def _wait_socket(self, wait: float) -> None:
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            if os.path.exists(self.path):
                return
            time.sleep(0.05)
        raise QmpTimeout(f"QMP socket {self.path} did not appear in {wait}s")

    def execute(self, command: str, **arguments: Any) -> dict:
        """Send one command, skip events, return the "return" payload."""
        if self._sock is None:
            raise QmpError("QMP client is not connected")
        payload = {"execute": command}
        if arguments:
            payload["arguments"] = arguments
        line = json.dumps(payload)
        self.log(f">> {line}")
        try:
            self._sock.sendall(line.encode() + b"\n")
            answer = self._await_answer(command)
        except (BrokenPipeError, ConnectionResetError, OSError) as exc:
            self.close()
            raise QmpError(f"{command}: connection lost ({exc})") from exc
        self.history.append({"command": command, "arguments": arguments,
                             "answer": answer})
        self.log(f"<< {json.dumps(answer)}")
        return answer

    def _await_answer(self, command: str) -> dict:
        deadline = time.monotonic() + self.timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise QmpTimeout(f"{command}: no answer in {self.timeout}s")
            message = self._read_json(left)
            if not isinstance(message, dict):
                continue
            if "event" in message:
                self.log(f"== event {message.get('event')}")
                continue
            if "error" in message:
                raise QmpError(f"{command}: {message['error']}")
            if "return" in message:
                return message["return"]

    def _read_json(self, timeout: float) -> Any:
        """Read one newline-delimited JSON object, honouring *timeout*."""
        assert self._sock is not None
        self._sock.settimeout(timeout)
        while True:
            index = self._buffer.find(b"\n")
            if index >= 0:
                line, self._buffer = self._buffer[:index], self._buffer[index + 1:]
                text = line.strip()
                if text:
                    return json.loads(text.decode("utf-8", "replace"))
                continue
            try:
                chunk = self._sock.recv(65536)
            except socket.timeout as exc:
                raise QmpTimeout(f"read timeout after {timeout}s") from exc
            if not chunk:
                raise QmpError("QMP peer closed the connection")
            self._buffer += chunk

    def close(self) -> None:
        """Close the socket and remove the socket file we created."""
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None
        try:
            if os.path.exists(self.path):
                os.unlink(self.path)
        except OSError:
            pass

    def __enter__(self) -> "QmpClient":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()
