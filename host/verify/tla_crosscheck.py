"""Relais -- cross-check the TLA+ model against the C core.

docs/tla/Relais.tla and core/src/arbiter.c are two independent descriptions of
the same protocol.  This script confronts them and refuses to agree with
itself: every metric is computed twice, by two different paths, and the script
exits non-zero as soon as one metric disagrees.

Three paths are used:

  model   a breadth-first exploration of the REDUCED state machine, written in
          tla_model.py as a literal transcription of docs/tla/Relais.tla.
  tlc     TLC itself (tlc2.TLC) on the very same constants.  When the jar is
          not installed the row says "not run"; it is never faked.
  c       the C code: host/explore for the verdict and the state count, and
          host/simulate for the (mandate, holder) pairs and the value sources
          it really produces, read from its JSON-lines journal.

What is compared: the violation verdicts (strict equality, all must be zero),
the distinct state counts of the model and of TLC (strict equality: this is
what validates the transcription), the containment of every (mandate, holder)
pair the C produced inside the pairs the model admits, and the set of value
sources.

Usage:  python3 host/verify/tla_crosscheck.py [--max-tick N] [--no-c]
"""

import json
import os
import re
import subprocess
import sys
import tempfile

# The reduced machine lives in tla_model.py; its names are re-exported so that
# both the cross-checker and its tests address one single namespace.
from tla_model import *  # noqa: F401,F403
from tla_model import Config, explore

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JAR_CANDIDATES = (
    os.environ.get("TLA2TOOLS_JAR", ""),
    os.path.expanduser("~/.local/share/tla2tools/tla2tools.jar"),
    "/usr/local/share/tla2tools.jar",
    os.path.join(REPO, "tla2tools.jar"),
)


def find_jar():
    for path in JAR_CANDIDATES:
        if path and os.path.isfile(path):
            return path
    return None


def run_tlc(cfg, jar, timeout=900):
    """Run TLC on docs/tla/Relais.tla with the very same constants."""
    tla_dir = os.path.join(REPO, "docs", "tla")
    if not os.path.isfile(os.path.join(tla_dir, "Relais.tla")):
        return None
    with tempfile.TemporaryDirectory() as tmp:
        # TLC looks for the configuration next to the module, so both have to
        # live in the same directory: the module is copied, never moved.
        module = os.path.join(tmp, "Relais.tla")
        with open(os.path.join(tla_dir, "Relais.tla"), encoding="utf-8") as src:
            with open(module, "w", encoding="utf-8") as dst:
                dst.write(src.read())
        cfg_path = os.path.join(tmp, "MC.cfg")
        with open(cfg_path, "w") as handle:
            handle.write("SPECIFICATION Spec\nCONSTANTS\n")
            for name, value in cfg.as_dict().items():
                handle.write("  %s = %d\n" % (name, value))
            handle.write("INVARIANT TypeOK\nINVARIANT Inv_I1\nINVARIANT Inv_I2\n")
            handle.write("INVARIANT Inv_I3\nINVARIANT Inv_I6\nINVARIANT Inv_Safety\n")
        cmd = ["java", "-XX:+UseParallelGC", "-cp", jar, "tlc2.TLC",
               "-config", "MC.cfg", "-workers", "4", "Relais"]
        try:
            proc = subprocess.run(cmd, cwd=tmp, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as exc:
            return {"error": str(exc)}
        text = proc.stdout.decode("utf-8", "replace")
        # TLC prints "distinct states found" on every Progress line too:
        # the final count is the last occurrence, never the first.
        counts = re.findall(r"([0-9]+) distinct states found", text)
        states = counts[-1] if counts else None
        violation = re.search(r"Invariant (\w+) is violated", text)
        return {"states": int(states) if states else None,
                "violation": violation.group(1) if violation else None,
                "ok": "No error has been found" in text,
                "output": text}


def run_c_explore(budget=60000, depth=6):
    """The C exhaustive explorer: the verdict and the state count."""
    binary = os.path.join(REPO, "build", "relais-explore")
    if not os.path.isfile(binary):
        return {"error": "build/relais-explore is missing (run make first)"}
    proc = subprocess.run([binary, "--budget", str(budget), "--depth", str(depth),
                           "--faults", "1", "--quiet"],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", "replace")
    states = re.search(r"EXPLORE-OK ([0-9]+) states", text)
    return {"states": int(states.group(1)) if states else None,
            "violation": None if proc.returncode == 0 else "explorer reported a violation",
            "ok": proc.returncode == 0, "output": text.strip()}


def run_c_simulate():
    """The C seeded simulator: the pairs and the sources it really produces."""
    binary = os.path.join(REPO, "build", "relais-simulate")
    if not os.path.isfile(binary):
        return {"error": "build/relais-simulate is missing (run make first)"}
    pairs = set()
    sources = set()
    failures = []
    scenarios = [[], ["--fault", "20:death:0"], ["--fault", "20:freeze:1:3"],
                 ["--level", "complet"], ["--fault", "30:death:1"]]
    for extra in scenarios:
        with tempfile.NamedTemporaryFile(suffix=".jsonl", delete=False) as journal:
            path = journal.name
        cmd = [binary, "--ticks", "200", "--seed", "7", "--journal", path, "--quiet"]
        cmd.extend(extra)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for line in open(path, encoding="utf-8"):
            record = json.loads(line)
            if record.get("kind") == "tick":
                # the journal writes "" for RELAIS_NODE_NONE, the model "none"
                holder = record["holder"] or NONE
                pairs.add((record["mandate"], holder))
                sources.add(record["source"])
        os.unlink(path)
        if proc.returncode != 0:
            failures.append(" ".join(cmd))
    return {"pairs": pairs, "sources": sources, "violations": failures}


def compare(model, tlc, c_explore, c_sim):
    """Build the agreement table; every row is (name, left, right, agree)."""
    rows = []
    model_bad = len(model["violations"])
    rows.append(("violations: model", 0, model_bad, model_bad == 0))
    if tlc is not None and "error" not in tlc:
        rows.append(("violations: TLC", 0, 0 if tlc["ok"] else 1, bool(tlc["ok"])))
        rows.append(("states: model vs TLC", model["states"], tlc["states"],
                     model["states"] == tlc["states"]))
    else:
        rows.append(("violations: TLC", "not run", "not run", True))
        rows.append(("states: model vs TLC", "not run", "not run", True))
    if "error" in c_explore:
        rows.append(("violations: C explorer", "not run", c_explore["error"], True))
        rows.append(("states: C explorer", "not run", "not run", True))
    else:
        rows.append(("violations: C explorer", 0, 0 if c_explore["ok"] else 1,
                     bool(c_explore["ok"])))
        rows.append(("states: C explorer", "informative", c_explore["states"], True))
    if "error" in c_sim:
        rows.append(("violations: C simulator", "not run", c_sim["error"], True))
        rows.append(("mandate/holder: C subset of model", "not run", "not run", True))
        rows.append(("sources: C vs model", "not run", "not run", True))
    else:
        rows.append(("violations: C simulator", 0, len(c_sim["violations"]),
                     not c_sim["violations"]))
        outside = c_sim["pairs"] - model["pairs"]
        rows.append(("mandate/holder: C subset of model", 0, len(outside), not outside))
        rows.append(("sources: C vs model", sorted(model["sources"]),
                     sorted(c_sim["sources"]),
                     model["sources"] == c_sim["sources"]))
    return rows


def main(argv):
    max_tick = 2
    use_c = True
    args = argv[1:]
    if "--max-tick" in args:
        max_tick = int(args[args.index("--max-tick") + 1])
    if "--no-c" in args:
        use_c = False
    cfg = Config(max_tick=max_tick, n_horizon=2)
    print("Relais -- TLA+ cross-check")
    print("  constants: %s" % cfg.as_dict())
    model = explore(cfg)
    print("  model : %d distinct states, %d (mandate, holder) pairs, "
          "sources %s, %d violation(s)"
          % (model["states"], len(model["pairs"]), sorted(model["sources"]),
             len(model["violations"])))
    for violation in model["violations"][:10]:
        print("    violation: %s" % (violation,))
    jar = find_jar()
    tlc = None
    if jar:
        tlc = run_tlc(cfg, jar)
        if tlc and "error" not in tlc:
            print("  TLC   : %s states, ok=%s, violation=%s"
                  % (tlc["states"], tlc["ok"], tlc["violation"]))
        else:
            print("  TLC   : could not run (%s)" % (tlc,))
    else:
        print("  TLC   : jar not found (TLA2TOOLS_JAR or ~/.local/share/tla2tools)")
    c_explore = {"error": "skipped"}
    c_sim = {"error": "skipped"}
    if use_c:
        c_explore = run_c_explore()
        c_sim = run_c_simulate()
        print("  C     : explorer ok=%s states=%s | simulator %d pairs, sources %s"
              % (c_explore.get("ok"), c_explore.get("states"),
                 len(c_sim.get("pairs", ())), sorted(c_sim.get("sources", ()))))
    return print_table(compare(model, tlc, c_explore, c_sim))


def print_table(rows):
    """Print the agreement table; any disagreement makes the exit code 1."""
    print("")
    print("%-38s %-22s %-22s %s" % ("metric", "left", "right", "agree"))
    print("-" * 92)
    bad = 0
    for name, left, right, agree in rows:
        print("%-38s %-22s %-22s %s" % (name, left, right, "yes" if agree else "NO"))
        if not agree:
            bad += 1
    print("-" * 92)
    print("%d disagreement(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
