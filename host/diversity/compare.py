#!/usr/bin/env python3
"""Relais — tick by tick comparison of the build variants (mission M37).

The two computers of the project run the same source, the same compiler, the
same libc and the same kernel: that is a common cause failure, and docs/CCF.md
measures it instead of hiding it. This script is the measurement for the
COMPILATION axis: it plays one corpus (the published scenarios of scenarios/,
the targeted probes of host/mutants/run_mutants.py, and a seed sweep) on every
variant that host/diversity/build_variants.sh managed to build, and compares the
decisions of every tick BIT FOR BIT.

Two roles in the manifest:
  variant : must agree with the reference. A divergence is a serious defect and
            is published as such, with the first divergent tick.
  control : must NOT agree. Controls are injected defects (-DRELAIS_MUTANT=n)
            and a forbidden floating point option (-ffast-math). A control that
            agrees would mean the comparator is blind, which is reported as a
            failure of THIS harness, not as a success.

Usage:
  python3 host/diversity/compare.py
  python3 host/diversity/compare.py --ticks-cap 1500 --seeds 4 --keep
  python3 host/diversity/compare.py --manifest build/diversity/variants.tsv
Exit code: 0 when every variant agrees and every control diverges, 1 otherwise.
"""
import argparse
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "host", "bench"))
import relais_bench as bench  # reuse: params_string, trajectory_string, fault_arg

FLOAT_COLUMNS = ("value", "pos", "vel", "setpoint", "order", "shadow")
REFERENCE_NAME = "ref-O2"
# Length of the targeted probes, as in host/mutants/run_mutants.py: the faults
# they inject land at ticks 400..900, so a shorter run would not see them.
PROBE_TICKS = 1500
MANIFEST_COLUMNS = ("name", "status", "role", "compiler", "compiler_version",
                    "flags", "binary", "sha256", "bytes", "note")


def bits_to_float(text):
    """Raw IEEE-754 pattern of a trace column as a float, 0.0 when not a float."""
    try:
        return struct.unpack(">f", bytes.fromhex(text))[0]
    except (ValueError, struct.error):
        return 0.0


def bits_to_decimal(text):
    """Render the raw IEEE-754 pattern of a trace column as a decimal."""
    try:
        return "%.9g" % bits_to_float(text)
    except ValueError:
        return text


def read_manifest(path):
    """The manifest written by build_variants.sh: one line per attempted build."""
    built, missing = [], []
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    for line in lines[1:]:
        fields = line.split("\t")
        entry = dict(zip(MANIFEST_COLUMNS, fields))
        (built if entry.get("status") == "BUILT" else missing).append(entry)
    return built, missing


def scenario_args(scenario, ticks_cap, level):
    """Translate one published scenario into relais-trace arguments."""
    args = ["--ticks", str(min(int(scenario.get("ticks", 2000)), ticks_cap)),
            "--seed", str(scenario.get("seed", 1)),
            "--params", bench.params_string(scenario.get("params", {})),
            "--trajectory", bench.trajectory_string(scenario.get("trajectory", {}))]
    if level == "complet":
        args += ["--level", "complet"]
    for fault in scenario.get("faults", []):
        args += ["--fault", bench.fault_arg(fault)]
    return args


def load_scenarios(directory):
    names = sorted(n for n in os.listdir(directory) if n.endswith(".json"))
    out = []
    for name in names:
        with open(os.path.join(directory, name), encoding="utf-8") as fh:
            scenario = json.load(fh)
        out.append((os.path.splitext(name)[0], scenario))
    return out


def probe_runs(ticks_cap):
    """The targeted scenarios of host/mutants/run_mutants.py, replayed here: they
    are the runs that make a defect visible, so they are what gives this
    comparator its sensitivity. They keep their own length (PROBE_TICKS): a
    shorter run would cut the injected fault out and blind the controls."""
    probes = [
        ("probe-forge", ["--fault", "600:forge:1:0:-0.9", "--level", "complet"]),
        ("probe-forge-stale", ["--fault", "600:forge-stale:0:0:-0.9", "--level", "complet"]),
        ("probe-freeze-holder", ["--fault", "600:freeze:0:1", "--level", "complet"]),
        ("probe-double-death", ["--fault", "400:death:0", "--fault", "430:death:1"]),
        ("probe-restart-death", ["--fault", "640:restart:1", "--fault", "650:death:0",
                                 "--level", "complet"]),
        ("probe-actuator-restart", ["--restart-actuator", "800", "--level", "complet"]),
        ("probe-freeze-20", ["--fault", "600:freeze:0:20", "--level", "complet"]),
        ("probe-death-B", ["--fault", "400:death:1", "--level", "complet"]),
    ]
    length = max(ticks_cap, PROBE_TICKS)
    return [(name, ["--ticks", str(length), "--seed", "5"] + args)
            for name, args in probes]


def seed_runs(ticks_cap, seeds):
    """A seed sweep over a death and a lossy channel: the random draws change,
    the code does not, so two builds must still agree."""
    out = []
    for seed in range(1, seeds + 1):
        base = ["--ticks", str(ticks_cap), "--seed", str(seed), "--level", "complet"]
        out.append(("seed-%d-death-A" % seed,
                    base + ["--fault", "800:death:0"]))
        out.append(("seed-%d-lossy" % seed,
                    base + ["--loss", "0.05", "--delay", "1", "--fault", "900:death:1"]))
    return out


def build_corpus(ticks_cap, seeds):
    runs = []
    for name, scenario in load_scenarios(os.path.join(ROOT, "scenarios")):
        runs.append((name, scenario_args(scenario, ticks_cap, "complet")))
    runs += probe_runs(ticks_cap)
    runs += seed_runs(ticks_cap, seeds)
    return runs


def run_trace(binary, args, out_path):
    """One run of one variant: the trace file plus the invariant verdict."""
    proc = subprocess.run([binary] + args + ["--out", out_path], cwd=ROOT,
                          capture_output=True, text=True, timeout=600)
    tail = (proc.stderr or "").strip().splitlines()
    return {"exit": proc.returncode, "stderr": tail[-1] if tail else "",
            "stdout": (proc.stdout or "").strip()[:400]}


def read_trace(path):
    """Return (columns, rows) of a trace file; '#' lines are the two headers."""
    columns, rows = [], []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line.startswith("#columns"):
                columns = line.split("\t")[1:]
            elif not line.startswith("#") and line:
                rows.append(line.split("\t"))
    return columns, rows


def first_difference(columns, ref_row, var_row):
    diffs = []
    for i, name in enumerate(columns):
        if i >= len(ref_row) or i >= len(var_row):
            continue
        if ref_row[i] == var_row[i]:
            continue
        shown = (bits_to_decimal(ref_row[i]), bits_to_decimal(var_row[i])) \
            if name in FLOAT_COLUMNS else (ref_row[i], var_row[i])
        diffs.append({"column": name, "reference": shown[0], "variant": shown[1],
                      "ref_bits": ref_row[i], "var_bits": var_row[i]})
    return diffs


def compare_traces(columns, ref_rows, var_rows):
    """Bit exact comparison of two traces; the first divergent tick is kept."""
    result = {"rows": min(len(ref_rows), len(var_rows)),
              "length_mismatch": len(ref_rows) != len(var_rows),
              "differing_rows": 0, "first_tick": None, "first_diff": [],
              "columns": {}, "worst_gap": 0.0, "worst_gap_tick": None,
              "worst_gap_column": None}
    for ref_row, var_row in zip(ref_rows, var_rows):
        diffs = first_difference(columns, ref_row, var_row)
        if not diffs:
            continue
        result["differing_rows"] += 1
        for d in diffs:
            result["columns"][d["column"]] = result["columns"].get(d["column"], 0) + 1
            if d["column"] not in FLOAT_COLUMNS:
                continue
            gap = abs(bits_to_float(d["ref_bits"]) - bits_to_float(d["var_bits"]))
            if gap > result["worst_gap"]:
                result["worst_gap"] = gap
                result["worst_gap_tick"] = ref_row[0]
                result["worst_gap_column"] = d["column"]
        if result["first_tick"] is None:
            result["first_tick"] = ref_row[0]
            result["first_diff"] = diffs
    return result


def verdict(entry, cmp, ref_exit, run_exit):
    """Is this (variant, run) pair what its role promises?"""
    diverged = cmp["differing_rows"] > 0 or cmp["length_mismatch"] or run_exit != ref_exit
    if entry["role"] == "control":
        return ("OK" if diverged else "BLIND"), diverged
    return ("DIVERGENCE" if diverged else "OK"), diverged


def trace_path(out_dir, variant, run_name):
    return os.path.join(out_dir, "trace-%s-%s.tsv" % (variant, run_name))


def compare_one_variant(entry, runs, ref_traces, ref_exits, out_dir, keep):
    """Play the whole corpus on one variant and compare it with the reference."""
    records = []
    rows_compared = 0
    for run_name, args in runs:
        path = trace_path(out_dir, entry["name"], run_name)
        info = run_trace(entry["binary"], args, path)
        columns, rows = read_trace(path)
        ref_columns, ref_rows = ref_traces[run_name]
        cmp = compare_traces(ref_columns, ref_rows, rows)
        if columns and columns != ref_columns:
            cmp["length_mismatch"] = True
            cmp["first_diff"] = [{"column": "layout", "reference": "see manifest",
                                  "variant": "column names differ", "ref_bits": "",
                                  "var_bits": ""}]
        rows_compared += cmp["rows"]
        state, diverged = verdict(entry, cmp, ref_exits[run_name], info["exit"])
        records.append({"run": run_name, "state": state, "rows": cmp["rows"],
                        "differing_rows": cmp["differing_rows"],
                        "first_tick": cmp["first_tick"], "first_diff": cmp["first_diff"][:6],
                        "columns": cmp["columns"], "exit": info["exit"],
                        "reference_exit": ref_exits[run_name],
                        "length_mismatch": cmp["length_mismatch"],
                        "worst_gap": cmp["worst_gap"],
                        "worst_gap_tick": cmp["worst_gap_tick"],
                        "worst_gap_column": cmp["worst_gap_column"]})
        # The evidence lives in the JSON (first divergent tick, columns, worst
        # gap), so the trace file itself is dropped unless --keep: a full corpus
        # would otherwise keep hundreds of megabytes of transient traces.
        if not keep:
            os.remove(path)
        del diverged
    return records, rows_compared


def run_reference(ref, runs, out_dir):
    """Play the corpus once with the reference build: the traces everything else
    is compared against, plus its invariant verdict per run."""
    traces, exits = {}, {}
    for run_name, args in runs:
        path = trace_path(out_dir, ref["name"], run_name)
        info = run_trace(ref["binary"], args, path)
        traces[run_name] = read_trace(path)
        exits[run_name] = info["exit"]
    return traces, exits


def pick_reference(built):
    """The reference is the project's own build configuration when it is there."""
    for entry in built:
        if entry["name"] == REFERENCE_NAME and entry["role"] == "variant":
            return entry
    for entry in built:
        if entry["role"] == "variant":
            return entry
    return None


def sweep(built, ref, runs, ref_traces, ref_exits, out_dir, keep):
    """Every built variant against the reference: verdicts, divergences, controls."""
    per_variant, divergences, blind = [], [], []
    detected = 0
    tick_records = 0
    for entry in built:
        if entry["name"] == ref["name"]:
            per_variant.append({"name": entry["name"], "role": entry["role"],
                                "state": "reference", "diverging_runs": 0,
                                "sha256": entry["sha256"], "flags": entry["flags"]})
            continue
        records, rows = compare_one_variant(entry, runs, ref_traces, ref_exits,
                                            out_dir, keep)
        tick_records += rows
        # A control that diverges is a success: its divergent runs are evidence
        # that the comparator is not blind, and are published as such.
        interesting = ([r for r in records if r["state"] == "DIVERGENCE"]
                       if entry["role"] != "control"
                       else [r for r in records if r["state"] == "OK"
                             and (r["differing_rows"] or r["exit"] != r["reference_exit"])])
        for item in interesting:
            item["variant"] = entry["name"]
        if entry["role"] != "control":
            divergences.extend(interesting)
            detected_here = False
        else:
            detected_here = bool(interesting)
            detected += 1 if detected_here else 0
            if not detected_here:
                blind.append({"name": entry["name"],
                              "note": "agrees with the reference on every run"})
        per_variant.append({
            "name": entry["name"], "role": entry["role"], "sha256": entry["sha256"],
            "flags": entry["flags"], "rows_compared": rows,
            "state": ("DIVERGENCE" if entry["role"] != "control" and interesting
                      else "control-detected" if detected_here else "agrees"),
            "diverging_runs": len(interesting),
            "control_evidence": interesting[:3] if entry["role"] == "control" else None,
            "first_divergence": interesting[0] if interesting and
            entry["role"] != "control" else None})
    return per_variant, divergences, blind, detected, tick_records


def read_digest(out_dir):
    path = os.path.join(out_dir, "source-digest.txt")
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8") as fh:
        return fh.read().strip()


def build_result(built, missing, ref, runs, per_variant, divergences, blind,
                 detected, tick_records, columns_count, out_dir):
    return {
        "reference": ref["name"],
        "source_digest": read_digest(out_dir),
        "variants_built": [e["name"] for e in built],
        "variants_not_built": [{"name": e["name"], "status": e["status"],
                                "note": e.get("note", "")} for e in missing],
        "corpus_runs": len(runs),
        "corpus": [n for n, _ in runs],
        "tick_records_compared": tick_records,
        "columns_per_tick": columns_count,
        "field_comparisons": tick_records * columns_count,
        "variant_divergences": len(divergences),
        "divergences": divergences[:60],
        "controls_total": sum(1 for e in built if e["role"] == "control"),
        "controls_detected": detected,
        "blind_controls": blind,
        "per_variant": per_variant,
    }


def summarise(result, out_dir):
    with open(os.path.join(out_dir, "comparison.json"), "w", encoding="utf-8") as fh:
        json.dump(result, fh, indent=1, sort_keys=True)
    print("variants built       : %d (reference: %s)"
          % (len(result["variants_built"]), result["reference"]))
    print("variants not built   : %d" % len(result["variants_not_built"]))
    print("corpus runs          : %d" % result["corpus_runs"])
    print("tick records compared: %d" % result["tick_records_compared"])
    print("field comparisons    : %d (%d columns per tick)"
          % (result["field_comparisons"], result["columns_per_tick"]))
    print("variant divergences  : %d" % result["variant_divergences"])
    print("controls detected    : %d / %d"
          % (result["controls_detected"], result["controls_total"]))
    for item in result["blind_controls"]:
        print("  BLIND CONTROL: %s never diverged (%s)" % (item["name"], item["note"]))
    for entry in result["per_variant"]:
        for ev in (entry.get("control_evidence") or [])[:1]:
            print("  CONTROL %s detected on %s at tick %s: %d differing rows, "
                  "worst %s gap %.3g at tick %s"
                  % (entry["name"], ev["run"], ev["first_tick"], ev["differing_rows"],
                     ev["worst_gap_column"], ev["worst_gap"], ev["worst_gap_tick"]))
    for item in result["divergences"][:12]:
        print("  DIVERGENCE %s on %s at tick %s: %s"
              % (item["variant"], item["run"], item["first_tick"],
                 ", ".join("%s %s != %s" % (d["column"], d["reference"], d["variant"])
                           for d in item["first_diff"][:3])))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--manifest",
                    default=os.path.join(ROOT, "build/diversity/variants.tsv"))
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "build/diversity"))
    ap.add_argument("--ticks-cap", type=int, default=1500)
    ap.add_argument("--seeds", type=int, default=4)
    ap.add_argument("--keep", action="store_true", help="keep every trace file")
    args = ap.parse_args()

    built, missing = read_manifest(args.manifest)
    if not built:
        sys.exit("no built variant in %s: run build_variants.sh first" % args.manifest)
    ref = pick_reference(built)
    if ref is None:
        sys.exit("no variant can serve as the reference")
    os.makedirs(args.out_dir, exist_ok=True)
    runs = build_corpus(args.ticks_cap, args.seeds)
    ref_traces, ref_exits = run_reference(ref, runs, args.out_dir)
    per_variant, divergences, blind, detected, tick_records = sweep(
        built, ref, runs, ref_traces, ref_exits, args.out_dir, args.keep)
    result = build_result(built, missing, ref, runs, per_variant, divergences, blind,
                          detected, tick_records, len(ref_traces[runs[0][0]][0]),
                          args.out_dir)
    summarise(result, args.out_dir)
    ok = not divergences and not blind
    print("VERDICT: %s" % ("every variant agrees with the reference, every control "
                           "was detected" if ok else
                           "divergences or blind controls above: read them"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
