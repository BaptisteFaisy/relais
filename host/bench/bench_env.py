"""Environment discovery for the bench: repo root, image, binaries, verdicts.

Shared by orchestrator.py, qemu_bench.py, faults.py and the CLI, so nothing is
duplicated. Nothing here ever raises for a missing optional module: it turns
into an actionable BLOQUE verdict further up.
"""
from __future__ import annotations

import importlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from typing import Any, Callable, Optional

try:  # flat script directory or package
    from .qemu import (DEFAULT_MACS, QemuConfig, QemuError, QemuGuest,
                       detect_kvm, qemu_version, tail_file)
except ImportError:  # pragma: no cover
    from qemu import (DEFAULT_MACS, QemuConfig, QemuError, QemuGuest,
                      detect_kvm, qemu_version, tail_file)

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

IMAGE_CANDIDATES = ("image/qnx-ifs.img", "image/ifs-pc.img", "image/qnx.img",
                    "build/qnx-ifs.img", "image/boot/qnx-ifs.img")
IMAGE_HINT = (
    "No QNX 8.0 image found. Build one with the QNX SDP 8.0 (free through QNX "
    "Everywhere):\n"
    "  mkqnximage --type=qemu --target=x86_64 --boot --image image/qnx-ifs.img\n"
    "(fragments for the bench live in image/: MAC-based identity, security "
    "policy, autostart of relais-net/relais-core/relais-ctl).\n"
    "Then either put the image at image/qnx-ifs.img or export "
    "RELAIS_QNX_IMAGE=/path/to/ifs.img.\n"
    "Without an image, use --mode pc: the same scenarios run on the PC "
    "simulation and give the same verdicts.")
ACTUATOR_HINT = ("Actuator binary missing: run `make` to build "
                 "build/relais-actuator (actuator/pc/*.c).")
LINK_NETDEVS = {"A-B": "ab", "A-actuator": "act", "B-actuator": "act"}
STATUS_OK, STATUS_FAIL, STATUS_BLOCKED = "OK", "ECHEC", "BLOQUE"


class BenchError(RuntimeError):
    """Any bench failure, always carrying an actionable message."""


class MissingImageError(BenchError):
    """The QNX image is absent: mode qemu cannot start a guest."""


def repo_root(start: Optional[str] = None) -> str:
    """Walk up until the repository root (the directory holding CONTRACT.md)."""
    path = os.path.abspath(start or HERE)
    while True:
        if os.path.exists(os.path.join(path, "CONTRACT.md")):
            return path
        parent = os.path.dirname(path)
        if parent == path:
            return os.path.abspath(start or HERE)
        path = parent


def find_qnx_image(root: Optional[str] = None) -> Optional[str]:
    """Locate the QNX image: RELAIS_QNX_IMAGE first, then image/ candidates."""
    env = os.environ.get("RELAIS_QNX_IMAGE")
    if env and os.path.exists(env):
        return env
    base = root or repo_root()
    for candidate in IMAGE_CANDIDATES:
        path = os.path.join(base, candidate)
        if os.path.exists(path):
            return path
    return None


def require_qnx_image(root: Optional[str] = None) -> str:
    image = find_qnx_image(root)
    if not image:
        raise MissingImageError(IMAGE_HINT)
    return image


def find_binary(name: str, root: Optional[str] = None) -> Optional[str]:
    """Find a built bench binary (build/<name>) or one on PATH."""
    base = root or repo_root()
    candidate = os.path.join(base, "build", name)
    if os.path.exists(candidate) and os.access(candidate, os.X_OK):
        return candidate
    return shutil.which(name)


def dep(module: str) -> Any:
    """Import a bench module owned by the scenario/cable/runner lot."""
    try:
        return importlib.import_module(module)
    except ImportError as exc:
        raise BenchError(f"module '{module}' is not available ({exc}). It "
                         "belongs to the scenario/cable/runner lot; until it "
                         "lands, use --probe or --demo with stubs.") from exc


def verdict(status: str, mode: str, **fields: Any) -> dict:
    """Build the verdict every entry point returns (never an exception)."""
    out = {"status": status, "mode": mode, "ok": status == STATUS_OK,
           "violations": [], "ticks": 0, "artifacts": {}, "notes": [],
           "run_dir": None, "scenario": None}
    out.update(fields)
    return out


def load_scenario(path: str) -> Any:
    """Load a scenario file through scenario.py, or a plain JSON fallback."""
    try:
        return dep("scenario").load(path)
    except BenchError:
        if not os.path.exists(path):
            raise BenchError(f"scenario file not found: {path}")
        with open(path, "r", encoding="utf-8") as handle:
            return _DictScenario(json.load(handle))


class _DictScenario:
    """Minimal scenario stand-in used only when scenario.py is absent."""

    def __init__(self, data: dict) -> None:
        self._data = data
        self.id = data.get("id", 0)
        self.name = data.get("name", "unnamed")
        self.ticks = int(data.get("ticks", 1000))
        self.seed = int(data.get("seed", 0))
        self.faults = data.get("faults", [])

    def params_dict(self) -> dict:
        return dict(self._data.get("params", {}))

    def trajectory_args(self) -> list:
        traj = self._data.get("trajectory", {})
        return [f"--trajectory={traj.get('kind', 'slalom')}"] + [
            f"--{key}={value}" for key, value in traj.items() if key != "kind"]


def fault_dict(fault: Any) -> dict:
    """Normalise a fault: dict as-is, object through its attributes."""
    if isinstance(fault, dict):
        return fault
    return {key: getattr(fault, key) for key in
            ("at_tick", "type", "target", "link", "direction", "ticks",
             "crash", "point") if hasattr(fault, key)}


def period_seconds(params: dict) -> float:
    """Tick period in seconds; params['T'] is in milliseconds (spec 6.3)."""
    return float(params.get("T", 5)) / 1000.0
