#!/usr/bin/env python3
"""Relais — play the impaired-network profiles on the REAL bench processes (M38).

For every profile of host/impair/profiles.json this driver wires relais-actuator
and two relais-node processes through the seeded user-space UDP relay
(host/impair/impair_proxy.py), lets the run play out in real time, then measures
in the actuator journal (the authority, spec 6.4):

  - the takeovers: mandate_revoked events, and for each of them the delay until
    a fresh value is served again and until the new holder's own order is
    applied, in ticks and in microseconds (journal ns stamps are 0 on the PC
    writer, so microseconds are the logical protocol time: ticks * T * 1000);
  - the ticks without a fresh order (served by the will or the fallback law);
  - the invariant verdicts of relais-verify, strict and --lossy-channel. The
    lossy verdict is the safety verdict of this mission: on a disturbed channel
    the continuity bounds of I4/I5/I9 are outside the timing hypothesis
    (ROB-03, spec 7.3 item 5) and are published, not enforced; every safety
    invariant stays enforced. A non-zero lossy verdict is a REAL defect.

Usage:
  python3 host/impair/run_impaired.py [--out runs/impair] [--profile NAME]...
                                      [--base-port 7380] [--ticks N]
Exit code: 0 every profile is safe, 1 a safety violation or a broken run.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROFILES = os.path.join(ROOT, "host", "impair", "profiles.json")
PROXY = os.path.join(ROOT, "host", "impair", "impair_proxy.py")
TAKEOVER_TRIGGER = "mandate_revoked"
FRESH_SOURCES = ("holder", "shadow")
REPORT = ("ticks=%(ticks)d violations=%(violations)d max_gap=%(max_gap)d "
          "(bound %(bound)d)")


def binary(name: str) -> str:
    path = os.path.join(ROOT, "build", name)
    if not os.path.exists(path):
        raise SystemExit("missing %s: run `make all` first" % path)
    return path


def free_port_block(base: int, count: int, tries: int = 20) -> int:
    """Return the first block of *count* free consecutive UDP ports."""
    for shift in range(tries):
        start = base + shift * (count + 1)
        sockets = []
        try:
            for port in range(start, start + count):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.bind(("", port))
                sockets.append(sock)
            return start
        except OSError:
            continue
        finally:
            for sock in sockets:
                sock.close()
    raise SystemExit("no block of %d free UDP ports near %d" % (count, base))


def topology(base: int) -> Dict[str, Any]:
    """The nine ports: three real endpoints, six impaired directions."""
    real = {"act": base, "A": base + 1, "B": base + 2}
    aliases = {"A_to_act": base + 3, "B_to_act": base + 4, "act_to_A": base + 5,
               "act_to_B": base + 6, "A_to_B": base + 7, "B_to_A": base + 8}
    return {"real": real, "aliases": aliases}


def start(cmd: List[str], log_path: str) -> Tuple[subprocess.Popen, Any]:
    log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True, text=True)
    return proc, log


def wait_ready(path: str, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    return False


def proxy_command(profile: Dict[str, Any], topo: Dict[str, Any], out_dir: str,
                  seed: int) -> List[str]:
    cmd = [sys.executable, PROXY, "--profiles", PROFILES,
           "--profile", profile["name"], "--seed", str(seed),
           "--stats", os.path.join(out_dir, "proxy-stats.json"),
           "--ready", os.path.join(out_dir, "proxy.ready")]
    dest = {"A_to_act": topo["real"]["act"], "B_to_act": topo["real"]["act"],
            "act_to_A": topo["real"]["A"], "act_to_B": topo["real"]["B"],
            "A_to_B": topo["real"]["B"], "B_to_A": topo["real"]["A"]}
    for direction, alias in topo["aliases"].items():
        cmd += ["--link", "%s=%d:127.0.0.1:%d" % (direction, alias, dest[direction])]
    return cmd


def bench_commands(profile: Dict[str, Any], topo: Dict[str, Any], out_dir: str,
                   params: str, ticks: int, traj: str) -> List[List[str]]:
    alias = topo["aliases"]
    real = topo["real"]
    journal = os.path.join(out_dir, "actuator.jsonl")
    actuator = [binary("relais-actuator"), "--port", str(real["act"]),
                "--peer", "A=127.0.0.1:%d" % alias["act_to_A"],
                "--peer", "B=127.0.0.1:%d" % alias["act_to_B"],
                "--ticks", str(ticks), "--params", params, "--trajectory", traj,
                "--journal", journal,
                "--state", os.path.join(out_dir, "arb.state")]
    common = ["--ticks", str(ticks), "--params", params]
    node_a = [binary("relais-node"), "--self", "A", "--port", str(real["A"]),
              "--actuator", "127.0.0.1:%d" % alias["A_to_act"],
              "--peer", "127.0.0.1:%d" % alias["A_to_B"]] + common
    node_b = [binary("relais-node"), "--self", "B", "--port", str(real["B"]),
              "--actuator", "127.0.0.1:%d" % alias["B_to_act"],
              "--peer", "127.0.0.1:%d" % alias["B_to_A"]] + common
    return [actuator, node_a, node_b]


def stop(proc: subprocess.Popen, sig: int = signal.SIGTERM) -> None:
    if proc.poll() is None:
        try:
            proc.send_signal(sig)
            proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, ProcessLookupError, OSError):
            try:
                proc.kill()
            except OSError:
                pass


def play_profile(profile: Dict[str, Any], args: argparse.Namespace,
                 params: str, period_ms: int) -> Dict[str, Any]:
    """One profile: relay + actuator + two nodes, then the journal is judged."""
    name = profile["name"]
    out_dir = os.path.join(args.out, name)
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir, exist_ok=True)
    ticks = args.ticks or int(profile.get("ticks", 300))
    traj = profile.get("trajectory", "kind=slalom amp=0.6 freq=0.5")
    topo = topology(free_port_block(args.base_port, 9))
    ready = os.path.join(out_dir, "proxy.ready")
    relay, relay_log = start(proxy_command(profile, topo, out_dir,
                                           int(profile.get("seed", 0))),
                             os.path.join(out_dir, "proxy.log"))
    if not wait_ready(ready, 10.0):
        stop(relay, signal.SIGKILL)
        relay_log.close()
        return {"profile": name, "status": "BLOCKED", "why": "proxy not ready"}
    procs, logs = [], []
    for index, cmd in enumerate(
            bench_commands(profile, topo, out_dir, params, ticks, traj)):
        proc, log = start(cmd, os.path.join(out_dir, "proc%d.log" % index))
        procs.append(proc)
        logs.append(log)
    budget = ticks * period_ms / 1000.0 + args.grace
    try:
        procs[0].wait(timeout=budget)
    except subprocess.TimeoutExpired:
        stop(procs[0], signal.SIGKILL)
    for proc in procs[1:]:
        stop(proc, signal.SIGKILL)
    stop(relay)
    for log in logs + [relay_log]:
        log.close()
    return judge(name, out_dir, period_ms, args.level)


def read_journal(path: str) -> Tuple[List[Dict], List[Dict]]:
    ticks: List[Dict] = []
    events: List[Dict] = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("kind") == "tick":
                ticks.append(record)
            elif record.get("kind") == "event":
                events.append(record)
    return ticks, events


def takeovers(ticks: List[Dict], events: List[Dict],
              period_ms: int) -> List[Dict[str, Any]]:
    """Every mandate_revoked, and how long the recovery took."""
    rows = []
    for event in events:
        if event.get("event") != TAKEOVER_TRIGGER:
            continue
        at = int(event["tick"])
        row: Dict[str, Any] = {"revoked_at": at, "mandate": event.get("mandate"),
                               "delay_fresh": None, "delay_holder": None}
        for record in ticks:
            tick = int(record["tick"])
            if tick < at:
                continue
            if row["delay_fresh"] is None and record["source"] in FRESH_SOURCES:
                row["delay_fresh"] = tick - at
            if row["delay_holder"] is None and record["source"] == "holder":
                row["delay_holder"] = tick - at
                break
        for key in ("delay_fresh", "delay_holder"):
            if row[key] is not None:
                row[key + "_us"] = row[key] * period_ms * 1000
        rows.append(row)
    return rows


def holder_changes(ticks: List[Dict]) -> int:
    """Holder transitions towards a machine: the actuator's own handovers."""
    changes, previous = 0, ""
    for record in ticks:
        holder = record.get("holder") or ""
        if holder and holder != previous:
            changes += 1
        previous = holder
    return changes


def run_verify(journal: str, level: str, lossy: bool) -> Dict[str, Any]:
    cmd = [binary("relais-verify"), journal, "--level", level]
    if lossy:
        cmd.append("--lossy-channel")
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    match = re.search(r"ticks=(\d+) violations=(\d+) max_gap=(\d+) \(bound (\d+)\)",
                      proc.stdout)
    out = {"exit": proc.returncode, "report": proc.stdout.strip().splitlines()[0]
           if proc.stdout.strip() else ""}
    if match:
        out.update(ticks=int(match.group(1)), violations=int(match.group(2)),
                   max_gap=int(match.group(3)), bound=int(match.group(4)))
    else:
        out.update(ticks=0, violations=-1, max_gap=-1, bound=-1)
    return out


def judge(name: str, out_dir: str, period_ms: int, level: str) -> Dict[str, Any]:
    """Turn one run directory into the row of the campaign report."""
    journal = os.path.join(out_dir, "actuator.jsonl")
    if not os.path.exists(journal):
        return {"profile": name, "status": "BLOCKED", "why": "no journal"}
    ticks, events = read_journal(journal)
    if not ticks:
        return {"profile": name, "status": "BLOCKED", "why": "empty journal"}
    sources = {kind: sum(1 for t in ticks if t["source"] == kind)
               for kind in ("holder", "shadow", "testament", "fallback")}
    row: Dict[str, Any] = {
        "profile": name, "status": "OK", "ticks": len(ticks),
        "handovers": holder_changes(ticks),
        "takeovers": takeovers(ticks, events, period_ms),
        "no_fresh_order_ticks": sources["testament"] + sources["fallback"],
        "sources": sources,
        "events": {kind: sum(1 for e in events if e.get("event") == kind)
                   for kind in ("mandate_revoked", "mandate_auto_switch",
                                "mandate_granted", "cession", "fallback_enter",
                                "order_rejected", "certified", "decertified")},
    }
    row["verify_lossy"] = run_verify(journal, level, True)
    row["verify_strict"] = run_verify(journal, level, False)
    stats = os.path.join(out_dir, "proxy-stats.json")
    if os.path.exists(stats):
        try:
            with open(stats, encoding="utf-8") as handle:
                row["proxy"] = json.load(handle).get("totals", {})
        except (ValueError, OSError) as exc:
            row["proxy"] = {}
            row["proxy_stats_error"] = str(exc)
    return row


def delay_stats(row: Dict[str, Any], key: str) -> str:
    values = [t[key] for t in row.get("takeovers", []) if t.get(key) is not None]
    if not values:
        return "-"
    if len(values) == 1:
        return "%d" % values[0]
    return "%d/%d/%d" % (min(values), int(statistics.median(values)), max(values))


def print_table(rows: List[Dict[str, Any]]) -> None:
    print("\nprofile            ticks handov takeovers delay_fresh delay_holder "
          "no-fresh max_gap viol(strict/lossy)")
    for row in rows:
        if row.get("status") != "OK":
            print("%-18s BLOCKED: %s" % (row["profile"], row.get("why")))
            continue
        strict = row["verify_strict"]["violations"]
        lossy = row["verify_lossy"]["violations"]
        print("%-18s %5d %6d %9d %11s %12s %8d %7d %d/%d" % (
            row["profile"], row["ticks"], row["handovers"],
            len(row["takeovers"]), delay_stats(row, "delay_fresh"),
            delay_stats(row, "delay_holder"), row["no_fresh_order_ticks"],
            row["verify_lossy"]["max_gap"], strict, lossy))
    print("delays in ticks (min/median/max); microseconds = ticks * T * 1000")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Relais impaired-network campaign")
    parser.add_argument("--profiles", default=PROFILES)
    parser.add_argument("--profile", action="append", default=[],
                        help="play only this profile (repeatable)")
    parser.add_argument("--out", default=os.path.join("runs", "impair"))
    parser.add_argument("--base-port", type=int, default=7380)
    parser.add_argument("--ticks", type=int, default=0, help="override every profile")
    parser.add_argument("--grace", type=float, default=20.0)
    parser.add_argument("--level", default="complet", choices=["socle", "complet"])
    args = parser.parse_args(argv)
    args.out = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)

    with open(args.profiles, encoding="utf-8") as handle:
        catalog = json.load(handle)
    params_spec = catalog.get("params", {"T": 25, "p": 3, "N": 8})
    params = ",".join("%s=%s" % (k, v) for k, v in params_spec.items())
    period_ms = int(params_spec.get("T", 25))
    profiles = [p for p in catalog["profiles"]
                if not args.profile or p["name"] in args.profile]
    if not profiles:
        raise SystemExit("no profile selected")
    os.makedirs(args.out, exist_ok=True)

    rows = []
    for profile in profiles:
        started = time.monotonic()
        print("== profile %s (seed %s) ==" % (profile["name"], profile.get("seed")))
        row = play_profile(profile, args, params, period_ms)
        row["wall_s"] = round(time.monotonic() - started, 2)
        rows.append(row)
    print_table(rows)
    safe = all(row.get("status") == "OK" and
               row["verify_lossy"]["violations"] == 0 for row in rows)
    summary = {"params": params, "period_ms": period_ms,
               "level": args.level, "rows": rows, "safe": safe}
    with open(os.path.join(args.out, "summary.json"), "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2)
    print("IMPAIR-OK" if safe else "IMPAIR-ECHEC")
    print("summary: %s" % os.path.join(args.out, "summary.json"))
    return 0 if safe else 1


if __name__ == "__main__":
    sys.exit(main())
