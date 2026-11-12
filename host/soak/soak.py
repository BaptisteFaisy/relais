#!/usr/bin/env python3
"""Relais -- the long soak pilot (mission M42, idea 15).

The pilot runs a continuous, parameterised campaign: it rotates seeds, fault
profiles and levels, keeps at most two bench processes alive, journals every
run in a digest-chained log, and STOPS THE MOMENT an anomaly appears, keeping
the whole scene (journal, argv, output, history) frozen on disk.

The nightly probe (host/nightly/) replays the proofs once per night: it is a
snapshot. This pilot is the curve: the same proofs, continuously, with a hunter
looking for the rare anomaly.

Usage:
  python3 host/soak/soak.py --minutes 30 [--ticks 100000] [--jobs 2]
                            [--profiles all|nominal,death,...] [--level mixed]
                            [--out results/soak] [--seed0 4200] [--calib 12]

Requirement IDs: PERF-05 (long runs), ROB-06 (replayable campaigns),
INJ-08 (fault sweeps), OBS-07 (evidence published with its context).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import sys
import time
from collections import deque
from typing import Any, Dict, List, Optional, Sequence, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import anomaly   # noqa: E402  (repo-local modules, path set above)
import machine   # noqa: E402
import profiles  # noqa: E402
import report as soak_report  # noqa: E402

SIM = os.path.join(_ROOT, "build", "relais-simulate")
VERIFY = profiles.VERIFY

JOBS_MAX = 2           # at most two bench processes at a time (user rule)

effective_minutes = machine.effective_minutes   # kept for the unit tests
LAT_FACTOR = 4.0       # A09 threshold: reference p99.9 times this factor
RSS_WINDOW = 12        # A10: number of consecutive runs watched
RSS_GROWTH_KB = 1024   # A10: growth that counts as a leak

class Pilot:
    """Drives the campaign: launches runs, hunts anomalies, freezes on one."""

    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.out = os.path.abspath(args.out)
        self.jobs = max(1, min(args.jobs, JOBS_MAX))
        self.log_path = os.path.join(self.out, "soak.jsonl")
        self.digest = ""
        self.seq = 0
        self.records: deque = deque(maxlen=100)   # last runs, for the freeze
        self.pilot_rss: List[int] = []            # RSS of the pilot itself
        self.latency_samples: List[float] = []
        self.rss_series: Dict[str, List[int]] = {}
        self.anomalies: List[Dict[str, Any]] = []
        self.finalized = False
        self.ticks_total = 0
        self.handovers = 0
        self.mandates = 0
        self.runs = 0
        self.index = 0
        self.stopped = False
        self.started = time.time()
        self.start = time.monotonic()
        self.minutes = float(args.minutes)
        self.guard_reason = ""
        self.profiles = self._select_profiles()

    def _select_profiles(self) -> List[Dict[str, Any]]:
        """Keep the profiles asked for, at the level asked for."""
        if self.args.profiles in ("", "all"):
            picked = list(profiles.PROFILES)
        else:
            wanted = {p.strip() for p in self.args.profiles.split(",") if p.strip()}
            picked = [p for p in profiles.PROFILES if p["name"] in wanted]
        if not picked:
            raise SystemExit("no known profile in %r" % self.args.profiles)
        if self.args.level == "mixed":
            return [dict(p) for p in picked]
        out = []
        for p in picked:
            if self.args.level in p["levels"]:
                entry = dict(p)
                entry["levels"] = (self.args.level,)
                out.append(entry)
        if not out:
            raise SystemExit("no profile available at level %r" % self.args.level)
        return out

    # -- preparation ----------------------------------------------------
    def prepare(self) -> None:
        """Create the output folder, apply the machine guard, open the log."""
        os.makedirs(self.out, exist_ok=True)
        with open(os.path.join(self.out, "soak.pid"), "w", encoding="utf-8") as fh:
            fh.write("%d\n" % os.getpid())
        load = os.getloadavg()[0]
        mem_gb = machine.mem_available_gb()
        self.minutes, self.guard_reason = machine.effective_minutes(self.minutes, load, mem_gb)
        print("soak: %.0f min, %d job(s), %d tick(s)/run, profiles: %s"
              % (self.minutes, self.jobs, self.args.ticks,
                 ",".join(p["name"] for p in self.profiles)), flush=True)
        print("soak: guard: %s" % self.guard_reason, flush=True)
        print("soak: rules %s, free disk %.1f GB"
              % (anomaly.RULES_VERSION, machine.disk_free_gb(self.out)), flush=True)

    def next_spec(self) -> Optional[Dict[str, Any]]:
        """Build the next run of the campaign (seeds, profiles, levels rotate)."""
        profile = self.profiles[self.index % len(self.profiles)]
        seed = self.args.seed0 + self.index
        level = profile["levels"][(self.index // len(self.profiles)) % len(profile["levels"])]
        self.index += 1
        return profiles.build_run(profile, seed, self.args.ticks, level,
                                 self.out, self.index, SIM)

    # -- running --------------------------------------------------------
    def launch(self, spec: Dict[str, Any]) -> int:
        """Start one bench process; returns its pid."""
        out_fd = os.open(spec["stdout_path"], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        try:
            pid = os.posix_spawn(SIM, spec["argv"], os.environ,
                                 file_actions=[(os.POSIX_SPAWN_DUP2, out_fd, 1),
                                               (os.POSIX_SPAWN_DUP2, out_fd, 2)])
        finally:
            os.close(out_fd)
        spec["pid"] = pid
        spec["launched"] = time.monotonic()
        return pid

    def finish_run(self, spec: Dict[str, Any], status: int,
                   rusage: Any) -> None:
        """Post-process one finished run: hunt, log, freeze on a finding."""
        wall = time.monotonic() - spec["launched"]
        rc = os.waitstatus_to_exitcode(status)
        text = read_text(spec["stdout_path"])
        context = profiles.profile_context(spec)
        findings = sim_findings(rc, text, spec)
        journal_stats: Dict[str, Any] = {}
        if os.path.isfile(spec["journal_path"]):
            found, journal_stats = anomaly.scan_journal(spec["journal_path"],
                                                        context, spec["level"])
            findings.extend(found)
            verify = anomaly.verify_journal(spec["journal_path"], context, spec["level"])
            if verify:
                findings.append(verify)
        metric = latency_metric(rusage, spec["ticks"])
        findings.extend(self.check_trend(spec, metric, rusage))
        self.runs += 1
        self.ticks_total += spec["ticks"]
        self.handovers += int(journal_stats.get("handovers", 0))
        self.mandates += int(journal_stats.get("mandates", 0))
        record = self.log_run(spec, rc, wall, metric, rusage, journal_stats, findings)
        if findings:
            self.freeze(spec, record, findings)
        else:
            self.cleanup(spec)

    def check_trend(self, spec: Dict[str, Any], metric: float,
                    rusage: Any) -> List[Dict[str, Any]]:
        """A09 and A10: the trend rules, on the series of finished runs."""
        out: List[Dict[str, Any]] = []
        peak_kb = int(rusage.ru_maxrss)
        name = spec["profile"]["name"]
        series = self.rss_series.setdefault(name, [])
        series.append(peak_kb)
        if len(self.latency_samples) >= self.args.calib:
            reference = anomaly.latency_reference(self.latency_samples[:self.args.calib])
            drift = anomaly.check_latency(metric, reference, self.args.latency_factor)
            if drift:
                drift["context"].update({"profile": name, "seed": spec["seed"]})
                out.append(drift)
        growth = anomaly.check_rss(series, RSS_WINDOW, RSS_GROWTH_KB)
        if growth:
            growth["context"].update({"profile": name, "seed": spec["seed"]})
            out.append(growth)
        leak = anomaly.check_rss(self.pilot_rss, RSS_WINDOW, RSS_GROWTH_KB)
        if leak:
            leak["context"].update({"process": "pilot",
                                    "detail": "the pilot itself grows, it does not "
                                              "hold the campaign in memory"})
            out.append(leak)
        return out

    def log_run(self, spec: Dict[str, Any], rc: int, wall: float, metric: float,
                rusage: Any, journal_stats: Dict[str, Any],
                findings: List[Dict[str, Any]]) -> Dict[str, Any]:
        """Append one record to the digest-chained soak log."""
        self.latency_samples.append(metric)
        payload = {
            "seq": self.seq,
            "ts": time.time(),
            "run": spec["index"],
            "profile": spec["profile"]["name"],
            "level": spec["level"],
            "seed": spec["seed"],
            "ticks": spec["ticks"],
            "exit_code": rc,
            "wall_s": round(wall, 3),
            "cpu_us_per_ktick": round(metric, 1),
            "maxrss_kb": int(rusage.ru_maxrss),
            "journal": journal_stats,
            "verified_offline": bool(spec["flags"].get("verify")),
            "argv": spec["argv"],
            "findings": [f["rule"] for f in findings],
        }
        if os.path.isfile(spec["journal_path"]):
            payload["journal_sha256"] = anomaly.sha256_file(spec["journal_path"])
        self.digest = anomaly.chain_digest(self.digest, payload)
        payload["digest"] = self.digest
        self.seq += 1
        with open(self.log_path, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(payload, sort_keys=True, ensure_ascii=False) + "\n")
        self.records.append(payload)
        return payload

    # -- freezing -------------------------------------------------------
    def freeze(self, spec: Dict[str, Any], record: Dict[str, Any],
               findings: List[Dict[str, Any]]) -> None:
        """Stop everything and keep the whole scene of the anomaly."""
        self.anomalies.extend(findings)
        folder = os.path.join(self.out, "frozen", "run-%04d" % spec["index"])
        os.makedirs(folder, exist_ok=True)
        for src, name in ((spec["journal_path"], "journal.jsonl"),
                          (spec["stdout_path"], "simulate.out")):
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(folder, name))
        context = {
            "findings": findings,
            "record": record,
            "argv": spec["argv"],
            "profile": spec["profile"]["name"],
            "level": spec["level"],
            "seed": spec["seed"],
            "simulate_output": read_text(spec["stdout_path"])[-2000:],
            "journal_sha256": record.get("journal_sha256", ""),
            "log_tail": list(self.records)[-50:],
        }
        with open(os.path.join(folder, "anomaly.json"), "w", encoding="utf-8") as fh:
            json.dump(context, fh, indent=2, sort_keys=True, default=str)
        print("soak: ANOMALY %s -- scene frozen in %s"
              % (",".join(f["rule"] for f in findings), folder), flush=True)
        self.finalize("frozen")

    def cleanup(self, spec: Dict[str, Any]) -> None:
        """Remove the artefacts of a clean run (the evidence stays in the log)."""
        for path in (spec["journal_path"], spec["stdout_path"]):
            try:
                os.remove(path)
            except OSError:
                pass

    # -- ending ---------------------------------------------------------
    def finalize(self, status: str) -> Dict[str, Any]:
        """Write the summary and the reliability curve once, then report."""
        if self.finalized:
            return {}
        self.finalized = True
        summary = self.summary(status)
        with open(os.path.join(self.out, "summary.json"), "w", encoding="utf-8") as fh:
            json.dump(summary, fh, indent=2, sort_keys=True, default=str)
        svg = soak_report.build_svg(summary, read_log(self.out))
        with open(os.path.join(self.out, "reliability.svg"), "w", encoding="utf-8") as fh:
            fh.write(svg)
        self.print_summary(summary)
        return summary

    def summary(self, status: str) -> Dict[str, Any]:
        """Assemble the summary of the campaign."""
        duration = time.monotonic() - self.start
        reference = anomaly.latency_reference(self.latency_samples[:self.args.calib])
        all_rss = [kb for series in self.rss_series.values() for kb in series]
        return {
            "mission": "M42",
            "rules_version": anomaly.RULES_VERSION,
            "status": status,
            "started": time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(self.started)),
            "duration_s": round(duration, 1),
            "requested_minutes": self.args.minutes,
            "effective_minutes": round(self.minutes, 2),
            "machine_guard": self.guard_reason,
            "jobs": self.jobs,
            "ticks_per_run": self.args.ticks,
            "profiles": [p["name"] for p in self.profiles],
            "runs": self.runs,
            "ticks_total": self.ticks_total,
            "handovers_total": self.handovers,
            "mandates_total": self.mandates,
            "anomaly_count": len(self.anomalies),
            "anomalies": self.anomalies,
            "latency": {
                "metric": "cpu microseconds per 1000 ticks",
                "reference_p999": round(reference, 1),
                "factor": self.args.latency_factor,
                "threshold": round(reference * self.args.latency_factor, 1),
                "samples": len(self.latency_samples),
            },
            "rss": {
                "min_kb": min(all_rss) if all_rss else 0,
                "max_kb": max(all_rss) if all_rss else 0,
                "last_kb": all_rss[-1] if all_rss else 0,
                "window": RSS_WINDOW,
                "by_profile": {name: {"min_kb": min(v), "max_kb": max(v),
                                      "runs": len(v)}
                               for name, v in sorted(self.rss_series.items())},
            },
            "log": os.path.basename(self.log_path),
            "svg": "reliability.svg",
        }

    def print_summary(self, summary: Dict[str, Any]) -> None:
        """Print the one-line verdict of the campaign."""
        print("soak: %s -- %d runs, %d ticks, %d handovers, %d anomalies, %.1f min"
              % (summary["status"], summary["runs"], summary["ticks_total"],
                 summary["handovers_total"], summary["anomaly_count"],
                 summary["duration_s"] / 60.0), flush=True)

    # -- main loop ------------------------------------------------------
    def run(self) -> int:
        """Run the campaign until the budget is spent or an anomaly freezes it."""
        deadline = self.start + self.minutes * 60.0
        active: Dict[int, Dict[str, Any]] = {}
        last_rss = time.monotonic()
        while True:
            now = time.monotonic()
            if self.stopped or (now >= deadline and not active):
                break
            if now < deadline and not self.stopped and len(active) < self.jobs:
                if machine.disk_free_gb(self.out) < machine.DISK_MIN_GB:
                    print("soak: disk guard -- less than %.0f GB free, campaign stops"
                          % machine.DISK_MIN_GB, flush=True)
                    break
                spec = self.next_spec()
                if spec is not None:
                    active[self.launch(spec)] = spec
            for pid in list(active):
                done, status, rusage = os.wait4(pid, os.WNOHANG)
                if done == 0:
                    continue
                spec = active.pop(pid)
                self.finish_run(spec, status, rusage)
                if self.anomalies:
                    self.stopped = True
                    break
            if self.anomalies:
                break
            if time.monotonic() - last_rss > 1.0:
                last_rss = time.monotonic()
                self.pilot_rss.append(machine.self_rss_kb())
            time.sleep(0.1)
        for pid in list(active):
            terminate(pid)
            try:
                os.wait4(pid, 0)
            except ChildProcessError:
                pass
        self.finalize("frozen" if self.anomalies else
                      ("stopped" if self.stopped else "completed"))
        return 3 if self.anomalies else 0


def terminate(pid: int) -> None:
    """Stop a bench process that is still running."""
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.kill(pid, sig)
        except OSError:
            return
        time.sleep(0.2)


def read_log(out: str) -> List[Dict[str, Any]]:
    """Read back the digest-chained log of the campaign."""
    path = os.path.join(out, "soak.jsonl")
    records: List[Dict[str, Any]] = []
    if not os.path.isfile(path):
        return records
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return records


def read_text(path: str, limit: int = 4000) -> str:
    """Read the tail of a text file (the bench output)."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()[-limit:]
    except OSError:
        return ""


def sim_findings(rc: int, text: str, spec: Dict[str, Any]) -> List[Dict[str, Any]]:
    """A01: the core reports the invariant violations it saw while running."""
    if rc == 0 and "violations=0" in text:
        return []
    return [anomaly.finding("A01", exit_code=rc, profile=spec["profile"]["name"],
                             level=spec["level"], seed=spec["seed"],
                             output=text[-600:],
                             detail="the core reported a violation or failed")]


def latency_metric(rusage: Any, ticks: int) -> float:
    """CPU microseconds spent per 1000 simulated ticks."""
    if ticks <= 0:
        return 0.0
    cpu_us = (rusage.ru_utime + rusage.ru_stime) * 1e6
    return cpu_us / ticks * 1000.0


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    """Parse the command line of the pilot."""
    ap = argparse.ArgumentParser(description="Relais long soak (mission M42).")
    ap.add_argument("--minutes", type=float, default=3.0,
                    help="wall-clock budget of the campaign")
    ap.add_argument("--ticks", type=int, default=100000,
                    help="ticks per bench run")
    ap.add_argument("--jobs", type=int, default=2,
                    help="bench processes at the same time (2 at most)")
    ap.add_argument("--profiles", default="all",
                    help="comma separated profiles, or all")
    ap.add_argument("--level", default="mixed", choices=["mixed", "socle", "complet"],
                    help="level of the campaign")
    ap.add_argument("--out", default="results/soak", help="output folder")
    ap.add_argument("--seed0", type=int, default=4200, help="first seed")
    ap.add_argument("--calib", type=int, default=12,
                    help="runs used to calibrate the latency reference")
    ap.add_argument("--latency-factor", type=float, default=LAT_FACTOR,
                    help="A09 factor applied to the reference p99.9")
    return ap.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    """Entry point: prepare, run, report."""
    args = parse_args(argv)
    pilot = Pilot(args)
    pilot.prepare()

    def on_signal(_sig: int, _frame: Any) -> None:
        pilot.stopped = True
        print("soak: signal received, the campaign stops cleanly", flush=True)

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    return pilot.run()


if __name__ == "__main__":
    sys.exit(main())
