#!/usr/bin/env python3
"""Relais — HIL bench: REAL process death and REAL process freeze (M41).

What is real here (INJ-02, INJ-03, INJ-08):
  * the two computers and the actuator are REAL OS processes on this machine
    (build/relais-actuator, build/relais-node — the same binaries as the PC
    bench, with no fault hook enabled: no --die, no --bias);
  * the fault is a REAL kernel signal delivered to the holder process:
    SIGKILL (the process truly dies, its socket truly disappears) or
    SIGSTOP/SIGCONT (the process is truly frozen by the scheduler, then
    thawed). Nothing is simulated inside the process;
  * only processes started by THIS bench are ever signalled (Popen handles).

What is NOT real here: the actuator is a PC process (not a third QNX board),
the link is loopback UDP (a physical cut needs the relay adapter, see
host/hil/relay_adapter.h and docs/REAL-FAULTS.md), and the plant is simulated.

Measured on the actuator journal (the authority, spec 6.4): the revoke
latency (tick of mandate_revoked minus the first tick without a valid order),
the takeover latency (first tick served by the new holder's own order), in
ticks and in protocol microseconds; the invariants are judged by
build/verify-hil (disturbed channel and strict).

Usage:
  python3 host/hil/processcut.py --cuts 100 [--t 15] [--patience 3]
      [--out build/hil/processcut.json] [--keep] [--quick]
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hil_env import (ROOT, JournalTailer, binary, cleanup, distribution,  # noqa: E402
                     free_port, start, wait_for)

KINDS = ("kill-holder", "freeze-holder", "double-kill", "kill-then-freeze")
PRE_KILL_TICK = 20     # double-fault runs remove the first machine early
FREEZE_TICKS = 25      # SIGSTOP duration, in protocol ticks
STABLE_TICKS = 4       # holder with zero misses for this many ticks = steady
POST_TICKS = 45        # observation window after the main cut
CERT_WAIT_S = 1.2      # how long we wait for a certified second before cutting
STEADY_WAIT_S = 1.8    # how long we wait for a steady holder at all


def params(args):
    return {"T": args.t, "p": args.patience, "N": 8}


def params_string(p):
    return ",".join("%s=%s" % (k, v) for k, v in p.items())


class Run:
    """One actuator + two node processes, their ports and their journal."""

    def __init__(self, index, args, out_root):
        self.index = index
        self.kind = KINDS[index % len(KINDS)]
        self.double = self.kind in ("double-kill", "kill-then-freeze")
        self.dir = os.path.join(out_root, "run%04d-%s" % (index, self.kind))
        self.journal = os.path.join(self.dir, "actuator.jsonl")
        self.p_act, self.p_a, self.p_b = free_port(), free_port(), free_port()
        self.cut_target = 30 + (index % 40)      # sweep of 40 distinct ticks
        self.ticks = (self.cut_target + 260 + POST_TICKS
                      + (FREEZE_TICKS if "freeze" in self.kind else 0))
        self.args = args
        self.procs = []
        self.logs = []
        self.nodes = {}
        self.t_start = 0.0

    def start_procs(self):
        os.makedirs(self.dir, exist_ok=True)
        prms = params_string(params(self.args))
        # The computers bind first, the actuator second: the tempo then starts
        # with both machines listening, instead of a few ticks of nobody.
        for name, port, peer in (("A", self.p_a, self.p_b), ("B", self.p_b, self.p_a)):
            node, nlog = start([binary("relais-node"), "--self", name,
                                "--port", str(port), "--actuator",
                                "127.0.0.1:%d" % self.p_act, "--peer",
                                "127.0.0.1:%d" % peer,
                                "--ticks", str(self.ticks + 100), "--params", prms],
                               os.path.join(self.dir, "node-%s.log" % name))
            self.nodes[name] = node
            self.procs.append(node)
            self.logs.append(nlog)
        time.sleep(0.05)
        act, log = start([binary("relais-actuator"),
                          "--port", str(self.p_act),
                          "--peer", "A=127.0.0.1:%d" % self.p_a,
                          "--peer", "B=127.0.0.1:%d" % self.p_b,
                          "--ticks", str(self.ticks), "--params", prms,
                          "--journal", self.journal,
                          "--state", os.path.join(self.dir, "arb.state")],
                         os.path.join(self.dir, "actuator.log"), line_buffered=True)
        self.actuator = act
        self.t_start = time.monotonic()
        self.procs.append(act)
        self.logs.append(log)

    def finish(self):
        cleanup(self.procs)
        for log in self.logs:
            log.close()


def wait_stable_holder(tailer, min_tick, timeout_s, run, exclude=None,
                       require_certified=True):
    """Wait for a steady holder: its own fresh order, zero misses, for
    STABLE_TICKS consecutive ticks at tick >= min_tick, with the actuator
    pacing in real time. When `require_certified` is set, the second must
    also be certified AT THE CUT INSTANT (the redundancy the F1 claim rests
    on is in place at that precise tick).

    Returns (holder, last_record) or (None, None). `last_record` is the newest
    journal line at return time: the cut is dated on it (the whole pending
    buffer is always consumed, so it cannot lag behind the actuator).
    """
    deadline = time.monotonic() + timeout_s
    run_len, holder, last, last_ns = 0, None, None, 0
    while time.monotonic() < deadline:
        batch = tailer.read_new()
        for rec, ns in batch:
            if rec.get("kind") != "tick":
                continue
            last, last_ns = rec, ns
            ok = steady_tick(rec, exclude, min_tick)
            if ok and rec.get("holder") == holder:
                run_len += 1
            elif ok:
                holder, run_len = rec.get("holder"), 1
            else:
                holder, run_len = None, 0
        if batch:
            paced = last is not None and pacing_ok(run, last, last_ns)
            certified = bool(last and last.get("certified"))
            if run_len >= STABLE_TICKS and paced and (certified or not require_certified):
                return holder, last
        if all(p.poll() is not None for p in run.procs):
            return None, None
        time.sleep(0.001)
    return None, None


def steady_tick(rec, exclude, min_tick):
    """One tick of a healthy state: the holder serves its own fresh order,
    with no miss, and the tick is in the requested window."""
    return (rec.get("holder") in ("A", "B") and rec.get("holder") != exclude
            and rec.get("misses") == 0 and rec.get("source") == "holder"
            and rec.get("tick", 0) >= min_tick)


def pacing_ok(run, last, last_ns):
    """True when the actuator is publishing ticks now, and is not lagging far
    behind the wall clock (a stalled actuator catches up in a burst; cutting
    inside a burst would date the fault on the wrong tick).

    Two conditions: the newest journal line must be fresh (the actuator is
    alive and ticking), and the tick counter must be within 25 % of the wall
    clock (it is not catching up from a long stall)."""
    period_s = params(run.args)["T"] / 1000.0
    period_ms = params(run.args)["T"]
    age_ms = (time.monotonic_ns() - last_ns) / 1e6
    if age_ms > max(5 * period_ms, 30.0):
        return False
    expected = (time.monotonic() - run.t_start) / period_s
    return last.get("tick", 0) >= expected * 0.75 - 10


def first_miss_tick(journal_path, after_tick):
    """Journal-derived effective cut tick: the first tick the actuator itself
    saw without a valid holder order (misses >= 1) after `after_tick`. This
    is the honest protocol-side instant of the fault, independent of any
    bench-side dating."""
    with open(journal_path, encoding="utf-8") as fh:
        for line in fh:
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if (rec.get("kind") == "tick" and rec.get("tick", 0) > after_tick
                    and rec.get("misses", 0) >= 1):
                return int(rec["tick"])
    return None


def cut(run, tailer, node_name, mode, cut_index, records, last_rec,
        expect_takeover=True):
    """Deliver ONE real fault to a real process and measure the handover."""
    proc = run.nodes[node_name]
    t0 = time.monotonic_ns()
    if mode == "kill":
        proc.send_signal(signal.SIGKILL)     # INJ-02: the process truly dies
    else:
        proc.send_signal(signal.SIGSTOP)     # INJ-03: truly frozen by the kernel
    k_tick = int(last_rec.get("tick", 0))
    entry = {"run": run.index, "cut": cut_index, "kind": run.kind, "mode": mode,
             "target": node_name, "pid": proc.pid, "tick_cut": k_tick,
             "t0_wall_ns": t0, "certified_second_at_cut": bool(last_rec.get("certified"))}
    records.append(entry)
    period_ns = params(run.args)["T"] * 1_000_000
    timeout = (POST_TICKS + FREEZE_TICKS) * period_ns / 1e9 + 5.0
    rv = wait_for(tailer, lambda r: r.get("kind") == "event"
                  and r.get("event") == "mandate_revoked"
                  and r.get("node") == node_name, timeout)
    if rv is not None:
        rec, ns = rv
        entry["tick_revoked"] = int(rec["tick"])
        entry["revoke_ticks"] = entry["tick_revoked"] - k_tick
        entry["proto_revoke_us"] = entry["revoke_ticks"] * params(run.args)["T"] * 1000
        entry["wall_revoke_us"] = (ns - t0) // 1000
    tk = None
    if expect_takeover:
        tk = wait_for(tailer, lambda r: r.get("kind") == "tick"
                      and r.get("source") == "holder"
                      and r.get("holder") not in ("", node_name), timeout)
    if tk is not None:
        rec, ns = tk
        entry["new_holder"] = rec.get("holder")
        entry["tick_takeover"] = int(rec["tick"])
        entry["takeover_ticks"] = entry["tick_takeover"] - k_tick
        entry["proto_takeover_us"] = entry["takeover_ticks"] * params(run.args)["T"] * 1000
        entry["wall_takeover_us"] = (ns - t0) // 1000
    else:
        fb = wait_for(tailer, lambda r: r.get("kind") == "tick"
                      and r.get("source") == "fallback", timeout)
        if fb is not None:
            entry["tick_fallback"] = int(fb[0]["tick"])   # both machines down
    return entry, t0


def thaw_and_observe(run, tailer, proc, t_stop, k_tick, entry):
    """SIGCONT the frozen process at cut tick + FREEZE_TICKS and watch it
    come back (node_back) — the second half of INJ-03."""
    period_s = params(run.args)["T"] / 1000.0
    wait_for(tailer, lambda r: r.get("tick", 0) >= k_tick + FREEZE_TICKS,
             FREEZE_TICKS * period_s + 5.0)
    t_cont = time.monotonic_ns()
    proc.send_signal(signal.SIGCONT)
    entry["freeze_ticks"] = FREEZE_TICKS
    entry["wall_frozen_us"] = (t_cont - t_stop) // 1000
    back = wait_for(tailer, lambda r: r.get("kind") == "event"
                    and r.get("event") == "node_back", 40 * period_s + 5.0)
    entry["node_back"] = back is not None
    if back is not None:
        entry["tick_node_back"] = int(back[0]["tick"])


def verify_hil(journal_path, double_fault=False):
    """Two verdicts on the same journal, both from the core invariants:
      lossy  — the claim that holds on a disturbed channel (the OS scheduler
               delays frames): the safety invariants are enforced, the gap is
               measured and published;
      strict — the level-C completeness claim (zero gap), reported so the
               reader sees exactly what the shared machine costs.
    """
    cmd = [binary("verify-hil"), journal_path]
    if double_fault:
        cmd.append("--double-fault")
    out = {}
    for mode in ("lossy", "strict"):
        proc = subprocess.run(cmd + ["--%s" % mode], cwd=ROOT,
                              capture_output=True, text=True)
        match = re.search(r"violations=(\d+)", proc.stdout)
        out[mode] = {"violations": int(match.group(1)) if match else -1,
                     "report": proc.stdout.strip()}
    return out


def play_cuts(run, tailer, records):
    """Wait for the state the fault must land in, then play the run's cuts.

    Returns (entry, mode), or (None, None) when no usable state appeared in
    time and the run must be discarded (counted as a drop, not as a cut).
    """
    double = run.double
    first = PRE_KILL_TICK if double else run.cut_target
    holder, last = wait_stable_holder(tailer, first, CERT_WAIT_S, run,
                                      require_certified=True)
    if holder is None:
        # No certified second in time (the shared PC loses it often): cut
        # anyway, in a steady state, and record the certification.
        holder, last = wait_stable_holder(tailer, first, STEADY_WAIT_S, run,
                                          require_certified=False)
        if holder is None:
            return None, None
    if double:
        cut(run, tailer, holder, "kill", 0, records, last)
        holder, last = wait_stable_holder(tailer, run.cut_target, STEADY_WAIT_S,
                                          run, exclude=holder,
                                          require_certified=False)
        if holder is None:
            return None, None
    mode = "freeze" if "freeze" in run.kind else "kill"
    entry, t_stop = cut(run, tailer, holder, mode, 1 if double else 0, records,
                        last, expect_takeover=not double)
    if mode == "freeze" and not double:
        # Single fault: the frozen machine comes back while the other one
        # keeps serving — the F2 case (freeze then resumption).
        thaw_and_observe(run, tailer, run.nodes[holder], t_stop,
                         entry["tick_cut"], entry)
    return entry, mode


def annotate(records, run, verdicts):
    """Date each cut of the run on the actuator's own evidence (the first tick
    it saw without a valid order) and attach both verdicts."""
    period_us = params(run.args)["T"] * 1000
    for rec in records:
        if rec["run"] != run.index:
            continue
        rec["violations"] = verdicts["lossy"]["violations"]
        rec["violations_strict"] = verdicts["strict"]["violations"]
        rec["verify"] = verdicts["lossy"]["report"]
        rec["verify_strict"] = verdicts["strict"]["report"]
        eff = first_miss_tick(run.journal, rec["tick_cut"])
        if eff is None:
            continue
        rec["tick_cut_effective"] = eff
        if "tick_revoked" in rec:
            rec["revoke_ticks_effective"] = rec["tick_revoked"] - eff + 1
            rec["proto_revoke_us_effective"] = rec["revoke_ticks_effective"] * period_us
        if "tick_takeover" in rec:
            rec["takeover_ticks_effective"] = rec["tick_takeover"] - eff + 1
            rec["proto_takeover_us_effective"] = rec["takeover_ticks_effective"] * period_us


def judge(run, tailer, records, entry, mode):
    """Let the observation window run out, stop the actuator, judge."""
    period_s = params(run.args)["T"] / 1000.0
    end_tick = (entry["tick_cut"] + POST_TICKS
                + (FREEZE_TICKS if mode == "freeze" else 0))
    wait_for(tailer, lambda r: r.get("kind") == "tick" and r.get("tick", 0) >= end_tick,
             (POST_TICKS + FREEZE_TICKS) * period_s + 10.0, procs=[run.actuator])
    try:
        run.actuator.wait(timeout=2.0)
    except Exception:
        run.actuator.terminate()
    verdicts = verify_hil(run.journal, double_fault=run.double)
    annotate(records, run, verdicts)
    return verdicts["lossy"]["violations"]


def execute(run, records):
    """One bench run: real processes, real signals, then the verdict."""
    run.start_procs()
    tailer = JournalTailer(run.journal)
    try:
        entry, mode = play_cuts(run, tailer, records)
        if entry is None:
            return "invalid"
        return "VIOLATION" if judge(run, tailer, records, entry, mode) else "ok"
    finally:
        run.finish()
        tailer.close()


def summarize(records, args):
    by = {}
    for rec in records:
        by.setdefault((rec["kind"], rec["mode"]), []).append(rec)
    def count(key):
        return sum(1 for r in records if r.get(key, 0))
    out = {"requested": args, "cuts": len(records),
           "violations": count("violations"),
           "violations_strict": count("violations_strict"),
           "cuts_with_certified_second": count("certified_second_at_cut"),
           "by_scenario": {}}
    for key, group in sorted(by.items()):
        out["by_scenario"]["%s/%s" % key] = group_stats(group)
    out["by_certified"] = {
        "certified_second": group_stats([r for r in records
                                         if r.get("certified_second_at_cut")]),
        "no_certified_second": group_stats([r for r in records
                                            if not r.get("certified_second_at_cut")]),
    }
    out["overall"] = group_stats(records)
    return out


def group_stats(group):
    """Distribution of the handover latency over a group of cuts, in ticks
    and in protocol microseconds, plus the wall-clock upper bound."""
    def pick(key):
        return [r[key] for r in group if key in r]
    return {
        "n": len(group),
        "revoke_ticks": distribution(pick("revoke_ticks")),
        "takeover_ticks": distribution(pick("takeover_ticks")),
        "revoke_ticks_effective": distribution(pick("revoke_ticks_effective")),
        "takeover_ticks_effective": distribution(pick("takeover_ticks_effective")),
        "wall_takeover_us": distribution(pick("wall_takeover_us")),
        "wall_revoke_us": distribution(pick("wall_revoke_us")),
    }


def print_report(summary, records, t_us):
    print("\n=== HIL processcut: REAL SIGKILL/SIGSTOP on REAL processes ===")
    print("cuts played: %d   safety-invariant violations: %d   "
          "level-C zero-gap trips: %d   period T=%d ms" %
          (summary["cuts"], summary["violations"], summary["violations_strict"],
           summary["requested"]["t"]))
    print("cuts made with a certified second in place: %d/%d" %
          (summary["cuts_with_certified_second"], summary["cuts"]))
    for name, dist in summary["by_scenario"].items():
        rv = dist["revoke_ticks_effective"] or dist["revoke_ticks"]
        tk = dist["takeover_ticks_effective"] or dist["takeover_ticks"]
        print("  %-28s n=%-3d revoke p50/p99/max = %s/%s/%s ticks" %
              (name, dist["n"], fmt(rv, "p50"), fmt(rv, "p99"), fmt(rv, "max")))
        if tk:
            print("  %-28s      takeover p50/p99/max = %s/%s/%s ticks"
                  "  (%s/%s/%s us protocol)" %
                  ("", fmt(tk, "p50"), fmt(tk, "p99"), fmt(tk, "max"),
                   us(fmt(tk, "p50"), t_us), us(fmt(tk, "p99"), t_us),
                   us(fmt(tk, "max"), t_us)))
    ov = summary["overall"]
    for label, key in (("revoke", "revoke_ticks_effective"),
                       ("takeover", "takeover_ticks_effective")):
        dist = ov.get(key) or ov.get(key.replace("_effective", ""))
        if dist:
            print("  overall %-9s ticks p50=%.0f p99=%.0f max=%.0f (n=%d, %s us proto)"
                  % (label, dist["p50"], dist["p99"], dist["max"], dist["n"],
                     us(fmt(dist, "p50"), t_us)))
    wall = ov.get("wall_takeover_us")
    if wall:
        print("  wall takeover (upper bound: flush + 1 ms poll) p50=%.0f us "
              "p99=%.0f us max=%.0f us" % (wall["p50"], wall["p99"], wall["max"]))
    for rec in [r for r in records if r.get("violations", 0) != 0]:
        print("  VIOLATION run %s: %s" % (rec["run"], rec.get("verify", "")))


def fmt(dist, key):
    return "-" if not dist or dist.get(key) is None else ("%.0f" % dist[key])


def us(tick_str, t_ms):
    return "-" if tick_str == "-" else "%.0f" % (float(tick_str) * t_ms * 1000)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cuts", type=int, default=100)
    parser.add_argument("--t", type=int, default=15, help="period ms (1..50)")
    parser.add_argument("--patience", type=int, default=3)
    parser.add_argument("--out", default="build/hil/processcut.json")
    parser.add_argument("--keep", action="store_true", help="keep every run dir")
    parser.add_argument("--quick", action="store_true", help="6 cuts, smoke test")
    args = parser.parse_args()
    if args.quick:
        args.cuts = 6
    out_root = os.path.join(ROOT, "build", "hil", "runs")
    os.makedirs(out_root, exist_ok=True)
    records, index, dropped, failed = [], 0, 0, 0
    target = args.cuts
    started = time.monotonic()
    while len(records) < target and dropped < 60:
        run = Run(index, args, out_root)
        t_run = time.monotonic()
        status = execute(run, records)
        index += 1
        if status == "invalid":
            dropped += 1
            shutil.rmtree(run.dir, ignore_errors=True)
        elif status == "VIOLATION":
            failed += 1
            print("VIOLATION: run kept for reproduction: %s" % run.dir)
        elif not args.keep:
            shutil.rmtree(run.dir, ignore_errors=True)
        done = len(records)
        if done % 10 == 0 or done >= target:
            print("  cuts=%d/%d runs=%d dropped=%d elapsed=%.0fs" %
                  (done, target, index, dropped, time.monotonic() - started))
    summary = summarize(records, {"t": args.t, "patience": args.patience,
                                  "target": target})
    summary["dropped_runs"] = dropped
    summary["records"] = records
    out_path = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=1)
    print_report(summary, records, args.t)
    print("journal of every cut: %s" % out_path)
    return 1 if (failed or summary["violations"] or len(records) < target) else 0


if __name__ == "__main__":
    sys.exit(main())
