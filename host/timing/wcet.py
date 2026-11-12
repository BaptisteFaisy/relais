#!/usr/bin/env python3
"""wcet.py — worst-case extrapolation of a latency sample set.

Consumes latency samples produced by the M02 measurement layer and turns them
into the numbers of a temporal argument (PERF-01, E2, E5): descriptive
statistics, exact percentiles, and an extrapolated bound with a confidence
interval, plus the sample count needed to affirm a given bound.

Sample format (defined here, consumed by M02's --latency writer):
  JSONL, one object per line. The latency must be a non-negative number of
  MICROSECONDS under the key "latency_us". Optional context keys ("tick",
  "source", "role") are tolerated and ignored by this tool:

      {"latency_us": 412, "tick": 7, "source": "net_rx", "role": "A"}

  CSV is also accepted: one numeric value per line (first column), an optional
  non-numeric header line is skipped.

Only the Python standard library is used (rule 6). This is a pure analysis
tool: it never touches core/ and it proves nothing about the QNX target, whose
numbers are "to measure" — see docs/TIMING.md.
"""
import argparse
import csv
import json
import math
import statistics
import sys

# Defaults for the extrapolation and the confidence argument.
DEFAULT_THRESHOLD = 0.99     # quantile of the "excess threshold" (Gumbel POT)
DEFAULT_TAIL_PROB = 1e-6     # target survival: one exceedance in a million
DEFAULT_CONFIDENCE = 0.95    # confidence for the bound argument
MIN_EXCEED = 20              # minimum thresholds-above samples for the fit
MIN_SAMPLES = 100            # minimum total samples before any extrapolation


def load_samples(path):
    """Return a list of non-negative float latencies in microseconds."""
    values = []
    with open(path, "r", encoding="utf-8") as fh:
        first = fh.readline()
        fh.seek(0)
        is_jsonl = first.lstrip().startswith("{")
        if is_jsonl:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                obj = json.loads(line)
                v = extract_latency(obj)
                if v is not None:
                    values.append(v)
        else:
            for row in csv.reader(fh):
                if not row:
                    continue
                try:
                    values.append(float(row[0]))
                except ValueError:
                    continue  # a header line
    values = [v for v in values if v >= 0.0]
    values.sort()
    return values


def extract_latency(obj):
    """Pick the latency from a JSON object; None when there is none.

    Key precedence, documented for M02: latency_us, us, value, latency.
    """
    if not isinstance(obj, dict):
        return None
    for key in ("latency_us", "us", "value", "latency"):
        if key in obj:
            try:
                return float(obj[key])
            except (TypeError, ValueError):
                return None
    return None


def percentile(sorted_values, p):
    """Linear-interpolation percentile of an ascending list (0.0 <= p <= 1.0)."""
    if not sorted_values:
        raise ValueError("empty sample set")
    if p <= 0.0:
        return sorted_values[0]
    if p >= 1.0:
        return sorted_values[-1]
    n = len(sorted_values)
    rank = p * (n - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return sorted_values[lo]
    frac = rank - lo
    return sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac


def describe(sorted_values):
    """Descriptive statistics for the temporal argument."""
    n = len(sorted_values)
    if n == 0:
        raise ValueError("no latency samples")
    return {
        "n": n,
        "min": sorted_values[0],
        "mean": statistics.fmean(sorted_values),
        "stdev": statistics.stdev(sorted_values) if n >= 2 else 0.0,
        "p50": percentile(sorted_values, 0.50),
        "p90": percentile(sorted_values, 0.90),
        "p99": percentile(sorted_values, 0.99),
        "p99_9": percentile(sorted_values, 0.999),
        "p99_99": percentile(sorted_values, 0.9999),
        "max": sorted_values[-1],
    }


def gumbel_pot_bound(sorted_values, threshold, tail_prob, min_exceed):
    """Extrapolate an upper bound with Gumbel Peaks-Over-Threshold.

    Rationale: for latency tails in the Gumbel maximum domain of attraction,
    the exceedances above a high threshold follow an exponential distribution
    whose MLE scale parameter is the sample mean of the exceedances. This is
    the simplest EVT form with a closed-form MLE (so it is exact, stdlib-only
    and testable). The return level at survival probability q is
    x(q) = u - theta * ln(q / p_u), with p_u = P(X > u) estimated empirically.

    Returns a dict, or None when there are too few exceedances to fit.
    """
    n = len(sorted_values)
    u = percentile(sorted_values, threshold)
    exceed = [x - u for x in sorted_values if x > u]
    k = len(exceed)
    if n < MIN_SAMPLES or k < min_exceed:
        return None
    theta = statistics.fmean(exceed)          # MLE scale of the exponential tail
    if theta <= 0.0:
        return None
    p_u = k / n
    if tail_prob >= p_u:
        return None  # the asked tail is not beyond the threshold: no extrapolation
    log_ratio = math.log(p_u / tail_prob)     # positive
    bound = u + theta * log_ratio
    se_theta = theta / math.sqrt(k)           # standard error of the MLE scale
    z = 1.96                                  # 95% asymptotic confidence
    half = z * se_theta * log_ratio
    return {
        "bound": bound,
        "lo": bound - half,
        "hi": bound + half,
        "theta": theta,
        "threshold": u,
        "p_u": p_u,
        "exceed": k,
    }


def samples_for_quantile_bound(quantile, confidence):
    """Samples so the observed max covers the true quantile with `confidence`.

    P(max_n >= q-th quantile) = 1 - q^n >= confidence  =>  n >= ln(1-c)/ln(q).
    """
    if not (0.0 < quantile < 1.0) or not (0.0 < confidence < 1.0):
        raise ValueError("quantile and confidence must be strictly in (0,1)")
    return int(math.ceil(math.log(1.0 - confidence) / math.log(quantile)))


def render(desc, fit, args):
    """Human-readable verdict (the mission's 'X at confidence Y / need N')."""
    lines = []
    lines.append("latency samples: n=%d  min=%.1f  mean=%.1f  stdev=%.1f us"
                 % (desc["n"], desc["min"], desc["mean"], desc["stdev"]))
    lines.append("  p50=%.1f  p90=%.1f  p99=%.1f  p99.9=%.1f  p99.99=%.1f  "
                 "max=%.1f us"
                 % (desc["p50"], desc["p90"], desc["p99"], desc["p99_9"],
                    desc["p99_99"], desc["max"]))

    n = desc["n"]
    conf_max = 1.0 - args.quantile ** n
    need = samples_for_quantile_bound(args.quantile, args.confidence)
    lines.append("empirical bound: observed max is an upper bound of the true "
                 "%.2f%% quantile at confidence %.3f%% (%g samples)"
                 % (args.quantile * 100.0, conf_max * 100.0, n))
    lines.append("to affirm that same bound at %.0f%% confidence: %d samples"
                 % (args.confidence * 100.0, need))

    if fit is None:
        lines.append("extrapolated bound: INSUFFICIENT samples "
                     "(need >= %d total and >= %d above the threshold); "
                     "no EVT extrapolation is reported"
                     % (MIN_SAMPLES, args.min_exceed))
    else:
        lines.append("extrapolated Gumbel-POT bound at survival %.1e: "
                     "%.1f us  [95%% CI %.1f .. %.1f] "
                     "(threshold=%.1f, theta=%.2f, p_u=%.4f, %d exceedances)"
                     % (args.tail_prob, fit["bound"], fit["lo"], fit["hi"],
                        fit["threshold"], fit["theta"], fit["p_u"],
                        fit["exceed"]))
    return "\n".join(lines)


def main(argv):
    ap = argparse.ArgumentParser(description="WCET extrapolation of latency samples")
    ap.add_argument("file", help="CSV or JSONL sample file")
    ap.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD,
                    help="quantile of the excess threshold (default %.2f)"
                         % DEFAULT_THRESHOLD)
    ap.add_argument("--tail-prob", type=float, default=DEFAULT_TAIL_PROB,
                    help="target survival probability for the bound")
    ap.add_argument("--confidence", type=float, default=DEFAULT_CONFIDENCE,
                    help="confidence for the empirical bound argument")
    ap.add_argument("--quantile", type=float, default=0.9999,
                    help="quantile of the empirical bound argument")
    ap.add_argument("--min-exceed", type=int, default=MIN_EXCEED,
                    help="minimum exceedances for the EVT fit")
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = ap.parse_args(argv)

    values = load_samples(args.file)
    if not values:
        print("wcet: no numeric samples found in %s" % args.file, file=sys.stderr)
        return 1
    desc = describe(values)
    fit = gumbel_pot_bound(values, args.threshold, args.tail_prob, args.min_exceed)

    if args.json:
        payload = dict(desc)
        payload["extrapolated"] = fit
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print(render(desc, fit, args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))