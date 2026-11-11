#!/usr/bin/env python3
"""Replay the coverage scenario suite against a --coverage build.

`make coverage` compiles core/ and host/ with ``--coverage``, then this driver
replays, in order:
  * every unit test binary (level 0) — must pass;
  * the seeded simulator over a fixed, deterministic fault set (level 2), each
    run emitting an actuator journal and a twin journal, at a declared level;
  * the offline verifier on every journal, at the SAME level it was produced
    (a socle journal verified at complet reports false positives);
  * the exhaustive explorer (level 1) over three budgets — must pass.

A stress scenario (both machines down at once) legitimately breaks an
invariant; that is fine for COVERAGE (the code still ran) but is not a safety
claim — the safety proof lives in the explorer and the campaign, not here.  So
the simulator and the verifier may exit 1 (violation detected) without failing
the driver; only exit >= 2 (usage error / unreadable journal) is a real failure.
Tests and the explorer stay strict.  Stdlib only (project rule 6).

Usage: coverage_driver.py --build BUILD_DIR
"""

import argparse
import glob
import os
import subprocess
import sys

# (name, level, extra arguments). Levels: "socle" (default) or "complet".
SCENARIOS = [
    ("nominal", "socle", []),
    ("nominal-loud", "socle", []),  # the single non-quiet run
    ("death-A", "socle", ["--fault", "500:death:0"]),
    ("death-B", "socle", ["--fault", "500:death:1"]),
    ("freeze-A", "socle", ["--fault", "400:freeze:0:40"]),
    ("freeze-B", "socle", ["--fault", "400:freeze:1:40"]),
    ("thaw", "socle", ["--fault", "400:freeze:0:40", "--fault", "450:thaw:0"]),
    ("cut-A", "socle", ["--fault", "400:cut-actuator:0"]),
    ("cut-B", "socle", ["--fault", "400:cut-actuator:1"]),
    ("cut-peer", "socle", ["--fault", "400:cut-peer:0",
                           "--fault", "400:cut-peer:1"]),
    ("slow", "socle", ["--fault", "300:slow:0"]),
    ("bias-A", "socle", ["--fault", "300:bias:0:0:0.25"]),
    ("bias-B", "socle", ["--fault", "300:bias:1:0:0.25"]),
    ("restart", "socle", ["--fault", "500:death:1", "--fault", "600:restart:1"]),
    ("crash", "socle", ["--fault", "500:crash:0"]),
    ("forge", "socle", ["--fault", "500:forge:0"]),
    ("forge-stale", "socle", ["--fault", "500:forge-stale:0"]),
    ("complet-nominal", "complet", []),
    ("complet-death-A", "complet", ["--fault", "500:death:0"]),
    # stress: both machines down — a violation here is expected, not a failure.
    ("stress-double-death", "socle",
     ["--fault", "400:death:0", "--fault", "420:death:1"]),
    ("stress-auto-faults", "socle", ["--auto-faults", "6"]),
]


def run(argv):
    """Return the exit code of argv (0 ok, 1 violation, >=2 real error)."""
    return subprocess.run(argv, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode


def run_tests(build):
    failed = 0
    for binary in sorted(glob.glob(os.path.join(build, "test_*"))):
        base = os.path.basename(binary)
        if "." in base or not os.access(binary, os.X_OK):
            continue  # skip .gcno/.gcda/.log/.o artifacts
        if run([binary]) != 0:
            failed += 1
    return failed


def run_scenarios(build, journals):
    """Run every scenario; return (real_failures, violations, journal_count)."""
    simulate = os.path.join(build, "relais-simulate")
    if not os.path.isfile(simulate):
        return 0, 0, 0
    os.makedirs(journals, exist_ok=True)
    failures = violations = written = 0
    for name, level, extra in SCENARIOS:
        quiet = [] if name == "nominal-loud" else ["--quiet"]
        journal = os.path.join(journals, name + ".jsonl")
        twin = os.path.join(journals, name + ".twin.jsonl")
        argv = [simulate, "--ticks", "2000", "--seed", "1",
                "--level", level] + extra + \
            ["--journal", journal, "--twin-journal", twin] + quiet
        code = run(argv)
        if code >= 2:
            failures += 1
        elif code == 1:
            violations += 1
        else:
            written += 1
    return failures, violations, written


def run_verify(build, journals):
    """Verify each journal at the level it was produced; return real failures."""
    verify = os.path.join(build, "relais-verify")
    if not os.path.isfile(verify):
        return 0
    failures = 0
    for name, level, _ in SCENARIOS:
        journal = os.path.join(journals, name + ".jsonl")
        if not os.path.isfile(journal):
            continue
        argv = [verify, journal]
        if level == "complet":
            argv.append("--level")
            argv.append("complet")
        if run(argv) >= 2:
            failures += 1
    nominal = os.path.join(journals, "nominal.jsonl")
    if os.path.isfile(nominal) and run([verify, nominal, "--fault-free"]) >= 2:
        failures += 1
    return failures


def run_explorer(build):
    explore = os.path.join(build, "relais-explore")
    if not os.path.isfile(explore):
        return 0
    failures = 0
    for argv in ([explore, "--budget", "100000", "--depth", "12", "--faults", "1"],
                 [explore, "--budget", "150000", "--depth", "14", "--faults", "2"],
                 [explore, "--budget", "100000", "--depth", "12", "--faults", "1",
                  "--level", "complet"]):
        if run(argv) != 0:
            failures += 1
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    args = parser.parse_args()
    build = args.build
    journals = os.path.join(build, "journals")

    failures = run_tests(build)
    sim_fail, sim_viol, _ = run_scenarios(build, journals)
    failures += sim_fail + run_verify(build, journals) + run_explorer(build)

    print("coverage driver: %s (%d stress scenario(s) with an expected "
          "violation)" % ("OK" if failures == 0 else
                          "%d real failure(s)" % failures, sim_viol))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())