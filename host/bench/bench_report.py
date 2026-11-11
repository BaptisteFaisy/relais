"""Verdict rendering, HTML report and offline verification for the bench.

Kept apart from orchestrator.py so the CLI, the probes and the orchestrator all
share one implementation (no duplication). OBS-05, OBS-07.
"""
from __future__ import annotations

import html
import json
import os
import subprocess
import threading
from typing import Any, Callable, Optional

STATUS_OK, STATUS_FAIL, STATUS_BLOCKED = "OK", "ECHEC", "BLOQUE"
STATUS_ICON = {STATUS_OK: "[ OK ]", STATUS_FAIL: "[FAIL]",
               STATUS_BLOCKED: "[BLOCKED]"}

_CSS = ("body{font-family:system-ui,sans-serif;margin:2rem;color:#1c1c1c}"
        "h1{font-size:1.4rem}table{border-collapse:collapse;width:100%}"
        "th,td{border-bottom:1px solid #ddd;padding:.4rem .6rem;text-align:left}"
        ".OK{color:#0a7a2f;font-weight:600}.ECHEC{color:#b00020;font-weight:600}"
        ".BLOQUE{color:#a06000;font-weight:600}code{background:#f4f4f4;"
        "padding:.1rem .3rem;border-radius:3px}pre{background:#f7f7f7;"
        "padding:.6rem;overflow:auto;font-size:.8rem}")


def format_verdict(item: dict, indent: str = "  ") -> str:
    """One readable block per verdict, for the terminal."""
    icon = STATUS_ICON.get(item.get("status", ""), "[????]")
    lines = [f"{icon} {item.get('label') or item.get('scenario') or 'run'}"
             f" ({item.get('mode', '?')})"]
    detail = item.get("detail")
    if detail:
        lines += [f"{indent}{line}" for line in str(detail).splitlines()]
    for key in ("ticks", "run_dir"):
        if item.get(key):
            lines.append(f"{indent}{key}: {item[key]}")
    violations = item.get("violations") or []
    if violations:
        lines.append(f"{indent}violations: {len(violations)}")
        lines += [f"{indent}- {text}" for text in map(str, violations[:5])]
    for name, path in (item.get("artifacts") or {}).items():
        lines.append(f"{indent}{name}: {path}")
    for note in item.get("notes") or []:
        lines.append(f"{indent}note: {note}")
    for key, value in (item.get("measures") or {}).items():
        lines.append(f"{indent}{key}: {json.dumps(value, sort_keys=True)}")
    return "\n".join(lines)


def verify_journal(path: str, root: Optional[str] = None,
                   timeout: int = 120) -> dict:
    """Run relais-verify over a journal (OBS-07). Never raises."""
    if not path or not os.path.exists(path):
        return {"status": STATUS_BLOCKED,
                "detail": f"no journal to verify ({path or 'none'})"}
    binary = _find("relais-verify", root)
    if not binary:
        return {"status": STATUS_BLOCKED,
                "detail": "build/relais-verify not built: run `make verify`"}
    try:
        done = subprocess.run([binary, path], capture_output=True, text=True,
                              timeout=timeout, check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        return {"status": STATUS_FAIL, "detail": f"relais-verify failed: {exc}"}
    status = STATUS_OK if done.returncode == 0 else STATUS_FAIL
    return {"status": status, "detail": (done.stdout + done.stderr).strip(),
            "returncode": done.returncode}


def _find(name: str, root: Optional[str] = None) -> Optional[str]:
    base = root or os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    candidate = os.path.join(base, "build", name)
    return candidate if os.access(candidate, os.X_OK) else None


def render_report_html(items: list[dict], title: str = "Relais bench") -> str:
    """Self-contained HTML report (OBS-05): one row per verdict."""
    rows = []
    for item in items:
        status = html.escape(str(item.get("status", "?")))
        rows.append(
            "<tr>"
            f"<td>{html.escape(str(item.get('label') or item.get('scenario') or ''))}</td>"
            f"<td>{html.escape(str(item.get('mode', '')))}</td>"
            f"<td class='{status}'>{status}</td>"
            f"<td>{html.escape(str(item.get('ticks') or ''))}</td>"
            f"<td>{len(item.get('violations') or [])}</td>"
            f"<td><pre>{html.escape(_detail(item))}</pre></td>"
            "</tr>")
    return ("<!doctype html><html><head><meta charset='utf-8'>"
            f"<title>{html.escape(title)}</title><style>{_CSS}</style></head>"
            f"<body><h1>{html.escape(title)}</h1>"
            "<table><thead><tr><th>run</th><th>mode</th><th>status</th>"
            "<th>ticks</th><th>violations</th><th>detail</th></tr></thead>"
            f"<tbody>{''.join(rows)}</tbody></table></body></html>")


def _detail(item: dict) -> str:
    parts = [str(item.get("detail", ""))]
    parts += [f"note: {note}" for note in item.get("notes") or []]
    parts += [f"{key}: {json.dumps(value, sort_keys=True)}"
              for key, value in (item.get("measures") or {}).items()]
    parts += [f"{name}={path}" for name, path in
              (item.get("artifacts") or {}).items()]
    return "\n".join(part for part in parts if part)


def write_report(path: str, items: list[dict],
                 title: str = "Relais bench") -> str:
    """Write the HTML report, creating its directory. Returns the path."""
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(render_report_html(items, title))
    return path


def journal_summary(path: str) -> dict[str, Any]:
    """Cheap JSON-lines summary of an actuator journal (no external module)."""
    summary: dict[str, Any] = {"ticks": 0, "events": 0, "sources": {},
                               "holders": {}, "lamps": {}}
    if not path or not os.path.exists(path):
        return summary
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            _tally(summary, line)
    return summary


def _tally(summary: dict, line: str) -> None:
    line = line.strip()
    if not line:
        return
    try:
        record = json.loads(line)
    except json.JSONDecodeError:
        return
    kind = record.get("kind")
    if kind == "tick":
        summary["ticks"] += 1
        _count(summary["sources"], record.get("source", "?"))
        _count(summary["holders"], record.get("holder", "?"))
        _count(summary["lamps"], record.get("lamp", "?"))
    elif kind == "event":
        summary["events"] += 1


def _count(bucket: dict, key: str) -> None:
    bucket[key] = bucket.get(key, 0) + 1


class JournalTailer:
    """Follow a JSON-lines journal and feed each new record to a callback.

    Used by --demo to show the mandate, the source of every tick and the lamp
    while the run is going on (OBS-06). Daemon thread: stop() is enough.
    """

    def __init__(self, path: str, on_record: Callable[[dict], None],
                 interval: float = 0.1) -> None:
        self.path = path
        self.on_record = on_record
        self.interval = interval
        self.records = 0
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._offset = 0

    def start(self) -> "JournalTailer":
        self._thread = threading.Thread(target=self._follow, daemon=True,
                                        name="journal-tail")
        self._thread.start()
        return self

    def _follow(self) -> None:
        while not self._stop.is_set():
            self._read_once()
            self._stop.wait(self.interval)

    def _read_once(self) -> None:
        if not self.path or not os.path.exists(self.path):
            return
        with open(self.path, "r", encoding="utf-8", errors="replace") as handle:
            handle.seek(self._offset)
            chunk = handle.read()
            self._offset = handle.tell()
        for line in chunk.splitlines():
            self._emit(line)

    def _emit(self, line: str) -> None:
        line = line.strip()
        if not line:
            return
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            return
        self.records += 1
        try:
            self.on_record(record)
        except Exception:  # noqa: BLE001 - a display bug must not kill a run
            pass

    def stop(self, timeout: float = 3.0) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout)
            self._thread = None
        self._read_once()
