"""Fault plan: turn a scenario fault into bench primitives (INJ-02..INJ-04/07).

One place maps ``death|freeze|thaw|cut|restore|slow|restart|crash|bias`` onto
QMP calls, the programmed cable and the actuator, and records what happened so
the report can cite it (INJ-06).
"""
from __future__ import annotations

import time
from typing import Any, Callable, Optional

try:  # flat script directory or package
    from .bench_env import BenchError, fault_dict
except ImportError:  # pragma: no cover
    from bench_env import BenchError, fault_dict


class FaultRunner:
    """Plays a fault plan against a running bench, in wall-clock time."""

    def __init__(self, bench: Any, period_s: float,
                 logger: Callable[[str], None] = print) -> None:
        self.bench = bench
        self.period_s = max(period_s, 1e-4)
        self.logger = logger
        self._pending: list[tuple[float, str, Callable[[], None]]] = []
        self.applied: list[dict] = []

    # -- plan -------------------------------------------------------------
    def run(self, faults: list, total_s: Optional[float] = None) -> list[dict]:
        """Apply every fault at its tick, honouring the scheduled thaws."""
        planned = sorted((fault_dict(f) for f in faults),
                         key=lambda f: f.get("at_tick", 0))
        started = time.monotonic()
        horizon = total_s if total_s is not None else self._horizon(planned)
        index = 0
        while time.monotonic() - started < horizon:
            self._release(time.monotonic())
            now = time.monotonic() - started
            while index < len(planned) and self._due(planned[index]) <= now:
                self.apply(planned[index])
                index += 1
            time.sleep(0.02)
        for fault in planned[index:]:
            self.apply(fault)
        self._release(float("inf"))
        return self.applied

    def _horizon(self, planned: list) -> float:
        last = planned[-1].get("at_tick", 0) if planned else 0
        return last * self.period_s + 1.0

    def _due(self, fault: dict) -> float:
        return fault.get("at_tick", 0) * self.period_s

    def at(self, when: float, label: str, action: Callable[[], None]) -> None:
        """Schedule a primitive (a thaw, a short freeze) at a monotonic time."""
        self._pending.append((when, label, action))

    def _release(self, now: float) -> None:
        """Run the scheduled primitives whose time has come."""
        for item in [entry for entry in self._pending if entry[0] <= now]:
            self._pending.remove(item)
            _when, label, action = item
            try:
                action()
                self.logger(f"[fault] scheduled {label} done")
            except (BenchError, OSError) as exc:
                self.logger(f"[fault] scheduled {label} failed: {exc}")

    # -- dispatch ---------------------------------------------------------
    def apply(self, fault: dict) -> dict:
        """Apply one fault now, record it, and return the record."""
        kind = fault.get("type", "?")
        handler = getattr(self, f"_f_{kind}", None)
        if handler is None:
            return self._record(kind, fault, "ignored", ok=False,
                                detail=f"unknown fault type {kind}")
        try:
            return handler(fault)
        except (BenchError, OSError) as exc:
            return self._record(kind, fault, "failed", ok=False,
                                detail=str(exc))

    def _record(self, kind: str, fault: dict, action: str, ok: bool = True,
                detail: str = "") -> dict:
        record = {"type": kind, "at_tick": fault.get("at_tick", 0),
                  "target": fault.get("target"), "link": fault.get("link"),
                  "action": action, "ok": ok, "detail": detail}
        self.applied.append(record)
        where = record["target"] or record["link"] or ""
        suffix = f" ({detail})" if detail else ""
        self.logger(f"[fault] tick {record['at_tick']} {kind} {where} "
                    f"-> {action}{suffix}")
        return record

    # -- primitives -------------------------------------------------------
    def _guest(self, fault: dict) -> Any:
        name = fault.get("target")
        guest = self.bench.guests.get(name)
        if guest is None:
            raise BenchError(f"no guest named {name!r} (known: "
                             f"{sorted(self.bench.guests)})")
        return guest

    def _f_death(self, fault: dict) -> dict:
        guest = self._guest(fault)
        point = fault.get("point")
        if point:
            self.bench.notes.append(
                f"death point {point}: the guest must write isa-debug-exit "
                "(0xf4); host-side fallback is a brutal kill at this instant")
        if fault.get("graceful"):
            guest.stop(grace=2.0)
            return self._record("death", fault, "quit", detail="graceful")
        guest.kill()
        return self._record("death", fault, "kill", detail=point or "brutal")

    def _f_freeze(self, fault: dict) -> dict:
        guest = self._guest(fault)
        guest.freeze()
        hold = float(fault.get("ticks", 1)) * self.period_s
        self.at(time.monotonic() + hold, f"thaw {fault.get('target')}",
                guest.thaw)
        return self._record("freeze", fault, f"stop for {hold:.3f}s")

    def _f_thaw(self, fault: dict) -> dict:
        self._guest(fault).thaw()
        return self._record("thaw", fault, "cont")

    def _f_restart(self, fault: dict) -> dict:
        self._guest(fault).reset()
        return self._record("restart", fault, "system_reset")

    def _f_cut(self, fault: dict) -> dict:
        return self._link(fault, cut=True)

    def _f_restore(self, fault: dict) -> dict:
        return self._link(fault, cut=False)

    def _link(self, fault: dict, cut: bool) -> dict:
        link = fault.get("link", "A-B")
        verb = "cut" if cut else "restore"
        direction = fault.get("direction", "both")
        detail: list[str] = []
        if link in ("A-B", "B-A"):
            command = f"{verb} {link} {direction}"
            if self.bench.cable is None:
                detail.append("no programmed cable: link left as is")
            else:
                self.bench.cable.apply(command)
                detail.append(f"cable: {command}")
            if fault.get("carrier"):
                for name in ("A", "B"):
                    self.bench.guests[name].set_link("ab", not cut)
                detail.append("carrier lost" if cut else "carrier back")
        else:
            name = link.split("-")[0]
            self._guest({"target": name}).set_link("act", not cut)
            detail.append(f"set_link act {'down' if cut else 'up'} on {name}")
        return self._record(verb, fault, f"{verb} {link}",
                            detail="; ".join(detail))

    def _f_slow(self, fault: dict) -> dict:
        """INJ-09: steal CPU with repeated short freezes."""
        guest = self._guest(fault)
        bursts = int(fault.get("count", 5))
        gap = max(float(fault.get("gap_ticks", 1)) * self.period_s, 1e-3)
        slice_s = max(self.period_s / 2.0, 0.001)
        now = time.monotonic()
        for step in range(bursts):
            self.at(now + step * gap, f"freeze #{step + 1}/{bursts}",
                    guest.freeze)
            self.at(now + step * gap + slice_s, f"thaw #{step + 1}/{bursts}",
                    guest.thaw)
        return self._record("slow", fault,
                            f"{bursts} freezes of {slice_s:.4f}s every {gap:.3f}s")

    def _f_crash(self, fault: dict) -> dict:
        """A process dies inside the guest: the QNX supervisor must restart it."""
        detail = (f"process {fault.get('crash')} is restarted by the QNX "
                  "supervisor; the host cannot drive it from QMP")
        self.bench.notes.append(detail)
        return self._record("crash", fault, "guest-side only", ok=False,
                            detail=detail)

    def _f_bias(self, fault: dict) -> dict:
        detail = ("value fault needs the in-guest test hook (INJ-10); "
                  "the host cannot inject it")
        self.bench.notes.append(detail)
        return self._record("bias", fault, "guest-side only", ok=False,
                            detail=detail)
