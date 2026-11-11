"""Relais bench orchestrator: ONE command starts the whole bench (INJ-01).

Mode ``pc`` delegates to runner.py (same inputs, same verdicts) so CI and the
jury can run everything without a QNX SDP. Mode ``qemu`` starts the programmed
cable, the actuator and the two QNX guests, waits for "second certified" /
"holder", plays the fault plan through QMP, then collects journal and pcaps.

Nothing escapes as an exception: missing image, missing module and missing
binary all come back as an actionable BLOQUE/ECHEC verdict.
"""
from __future__ import annotations

import os
import time
from typing import Any, Callable, Optional

try:  # flat script directory or package
    from .bench_env import (BenchError, MissingImageError, STATUS_BLOCKED,
                            STATUS_FAIL, STATUS_OK, dep, find_binary,
                            load_scenario, period_seconds, verdict)
    from .bench_report import (JournalTailer, journal_summary, verify_journal)
    from .faults import FaultRunner
    from .qemu_bench import QemuBench, run_dir_for
except ImportError:  # pragma: no cover
    from bench_env import (BenchError, MissingImageError, STATUS_BLOCKED,
                           STATUS_FAIL, STATUS_OK, dep, find_binary,
                           load_scenario, period_seconds, verdict)
    from bench_report import JournalTailer, journal_summary, verify_journal
    from faults import FaultRunner
    from qemu_bench import QemuBench, run_dir_for

BENCH_BINARIES = ("relais-actuator", "relais-simulate", "relais-verify",
                  "relais-explore")
JOURNAL_NAMES = ("actuator-journal.jsonl", "journal.jsonl", "relais.jsonl")


class Orchestrator:
    """Runs one scenario through the PC runner or through real QEMU guests."""

    def __init__(self, mode: str = "pc", campaign: str = "campaign",
                 root: Optional[str] = None, workdir: Optional[str] = None,
                 parallel: int = 1, actuator: str = "pc",
                 use_cable: bool = True, ready_timeout: float = 180.0,
                 max_run_s: float = 900.0,
                 logger: Callable[[str], None] = print) -> None:
        if mode not in ("pc", "qemu"):
            raise BenchError(f"unknown mode {mode!r}: use pc or qemu")
        self.mode = mode
        self.campaign = campaign
        self.root = root
        self.workdir = workdir
        self.parallel = parallel
        self.actuator = actuator
        self.use_cable = use_cable
        self.ready_timeout = ready_timeout
        self.max_run_s = max_run_s
        self.logger = logger
        self.bench: Optional[QemuBench] = None

    # -- public -----------------------------------------------------------
    def run(self, scenario_path: str, verify: bool = False,
            on_tick: Optional[Callable[[dict], None]] = None) -> dict:
        """Run one scenario and always return a verdict dict."""
        try:
            scenario = load_scenario(scenario_path)
        except BenchError as exc:
            return verdict(STATUS_BLOCKED, self.mode, detail=str(exc),
                           scenario=os.path.basename(scenario_path))
        run_dir = self.workdir or run_dir_for(self.campaign, self.root)
        result = self._dispatch(scenario, run_dir, on_tick)
        result["scenario"] = getattr(scenario, "name", str(scenario_path))
        result["run_dir"] = run_dir
        result.setdefault("artifacts", {})["scenario_file"] = scenario_path
        if verify:
            result["verify"] = verify_journal(
                result["artifacts"].get("journal", ""), self.root)
        return result

    def _dispatch(self, scenario: Any, run_dir: str,
                  on_tick: Optional[Callable[[dict], None]]) -> dict:
        runner = self._run_pc if self.mode == "pc" else self._run_qemu
        try:
            return runner(scenario, run_dir, on_tick)
        except MissingImageError as exc:
            return verdict(STATUS_BLOCKED, self.mode, detail=str(exc),
                           run_dir=run_dir)
        except BenchError as exc:
            return verdict(STATUS_BLOCKED, self.mode, detail=str(exc),
                           run_dir=run_dir)
        except (OSError, RuntimeError) as exc:
            return verdict(STATUS_FAIL, self.mode,
                           detail=f"{type(exc).__name__}: {exc}",
                           run_dir=run_dir)

    # -- mode pc ----------------------------------------------------------
    def _run_pc(self, scenario: Any, run_dir: str,
                on_tick: Optional[Callable[[dict], None]]) -> dict:
        binaries = {name: find_binary(name, self.root)
                    for name in BENCH_BINARIES}
        missing = [name for name, path in binaries.items() if not path]
        if missing:
            raise BenchError(
                f"bench binaries not built: {', '.join(missing)}. Run `make` "
                "first (build/relais-actuator, build/relais-simulate, "
                "build/relais-verify).")
        runner = dep("runner")
        os.makedirs(run_dir, exist_ok=True)
        tailer = self._tail(run_dir, on_tick) if on_tick else None
        try:
            outcome = runner.Runner(binaries=binaries,
                                    workdir=run_dir).run(scenario, twin=True)
        finally:
            if tailer is not None:
                tailer.stop()
        journal = _find_journal(getattr(outcome, "run_dir", run_dir))
        violations = list(getattr(outcome, "violations", []) or [])
        ok = bool(getattr(outcome, "ok", not violations)) and not violations
        return verdict(STATUS_OK if ok else STATUS_FAIL, "pc",
                       ticks=getattr(outcome, "ticks", 0),
                       violations=violations,
                       run_dir=getattr(outcome, "run_dir", run_dir),
                       artifacts={"journal": journal},
                       measures=_summarise(journal))

    def _tail(self, run_dir: str,
              on_tick: Callable[[dict], None]) -> "JournalTailer":
        path = os.path.join(run_dir, "actuator-journal.jsonl")
        return JournalTailer(path, on_tick).start()

    # -- mode qemu --------------------------------------------------------
    def _run_qemu(self, scenario: Any, run_dir: str,
                  on_tick: Optional[Callable[[dict], None]]) -> dict:
        period = period_seconds(scenario.params_dict())
        self.bench = QemuBench(scenario, run_dir, root=self.root,
                               actuator_kind=self.actuator,
                               use_cable=self.use_cable, logger=self.logger)
        bench = self.bench
        tailer = None
        applied: list = []
        ready: dict = {}
        try:
            bench.start()
            ready = bench.wait_ready(self.ready_timeout)
            journal = bench.actuator.journal_path if bench.actuator else ""
            if on_tick:
                tailer = JournalTailer(journal, on_tick).start()
            faults = list(getattr(scenario, "faults", []) or [])
            total_s = min(getattr(scenario, "ticks", 0) * period,
                          self.max_run_s)
            applied = FaultRunner(bench, period, self.logger).run(
                faults, total_s=total_s)
        finally:
            if tailer is not None:
                tailer.stop()
            artifacts = _collect_artifacts(bench)
            notes = list(bench.notes)
            bench.stop()
        failures = [rec for rec in applied
                    if not rec["ok"] and rec["action"] != "guest-side only"]
        return verdict(STATUS_FAIL if failures else STATUS_OK, "qemu",
                       ticks=getattr(scenario, "ticks", 0),
                       artifacts=artifacts, notes=notes,
                       measures={"ready": ready, "faults": applied,
                                 "journal": _summarise(artifacts.get("journal"))},
                       violations=[rec["detail"] for rec in failures])

    def close(self) -> None:
        """Stop a bench if a caller aborts in the middle of a run."""
        if self.bench is not None:
            self.bench.stop()
            self.bench = None

    def __enter__(self) -> "Orchestrator":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


def _find_journal(run_dir: str) -> str:
    """Locate the actuator journal inside a run directory."""
    for name in JOURNAL_NAMES:
        candidate = os.path.join(run_dir, name)
        if os.path.exists(candidate):
            return candidate
    return ""


def _collect_artifacts(bench: QemuBench) -> dict:
    """Journal, pcaps and bench manifest collected before stopping."""
    artifacts: dict[str, Any] = {}
    if bench.actuator is not None:
        artifacts["journal"] = bench.actuator.journal_path
    artifacts["manifest"] = os.path.join(bench.run_dir, "bench.json")
    for name, guest in bench.guests.items():
        artifacts[f"pcap-{name}"] = guest.pcap_path
        artifacts[f"console-{name}"] = guest.console_path
    return artifacts


def _summarise(journal: str) -> dict:
    """Journal summary through measures.py when present, else a local tally."""
    if not journal or not os.path.exists(journal):
        return {"status": "no journal yet"}
    try:
        return dep("measures").Journal(journal).summary()
    except (BenchError, OSError, ValueError):
        return journal_summary(journal)
