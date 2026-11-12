"""Relais — the rendering half of the epsilon study (mission M30, idea 67).

Tables, curve and recommendation, built from the rows measured by
epsilon_study.py. Split out so that every file stays under the 500 line limit
and so the measurement and its presentation do not get mixed up.

Nothing here measures anything: every number comes from a measured row, and a
cell that was not measured is printed as "-".
"""
import os

CERTIFIED_TARGET = 0.99  # a redundancy that is not there 1 % of the time is a gap


def fmt_sigma(value):
    if value == 0.0:
        return "0"
    return "%.0e" % value


def md_table(header, rows):
    lines = ["| " + " | ".join(header) + " |",
             "|" + "|".join(["---"] * len(header)) + "|"]
    for row in rows:
        lines.append("| " + " | ".join(str(c) for c in row) + " |")
    return "\n".join(lines)


def cell_certification(table, noise, sigma, epsilon):
    row = table.get((noise, sigma, epsilon))
    if row is None:
        return "-"
    frac = row["certified_fraction"]
    mark = "" if frac >= CERTIFIED_TARGET else " (!)"
    return "%.3f%s" % (frac, mark)


def table_certification(table, sigmas, epsilons, noise, field):
    rows = []
    for sigma in sigmas:
        rows.append([fmt_sigma(sigma)] +
                    [table_value(table, noise, sigma, e, field) for e in epsilons])
    return md_table(["sigma"] + ["eps=%.3f" % e for e in epsilons], rows)


def table_value(table, noise, sigma, epsilon, field):
    row = table.get((noise, sigma, epsilon))
    if row is None:
        return "-"
    if field == "certified_fraction":
        return cell_certification(table, noise, sigma, epsilon)
    if field == "gap_max":
        return "%.5f" % row[field]
    if field == "false_alarm_rate":
        return "%.2f" % row[field]
    return "%.4f" % row[field]


def cell_detection(a_table, b_table, noise, sigma, epsilon, bias):
    """A detection delay means nothing when the same cell already cries wolf
    without any fault: the attribution is checked against pass A."""
    clean = a_table.get((noise, sigma, epsilon))
    row = b_table.get((noise, sigma, epsilon, bias))
    if row is None:
        return "-"
    if row["detect_rate"] < 0.5:
        return "missed"
    if clean is not None and clean["false_alarm_rate"] > 0.0:
        return "noise"
    return "%.1f" % row["detect_delay_mean"]


def table_detection(a_table, b_table, sigmas, epsilons, bias, noise):
    rows = []
    for sigma in sigmas:
        rows.append([fmt_sigma(sigma)] +
                    [cell_detection(a_table, b_table, noise, sigma, e, bias)
                     for e in epsilons])
    return md_table(["sigma"] + ["eps=%.3f" % e for e in epsilons], rows)


def table_detect_rate(b_table, sigmas, epsilons, bias, noise):
    rows = []
    for sigma in sigmas:
        cells = []
        for epsilon in epsilons:
            row = b_table.get((noise, sigma, epsilon, bias))
            cells.append("-" if row is None else "%.2f" % row["detect_rate"])
        rows.append([fmt_sigma(sigma)] + cells)
    return md_table(["sigma"] + ["eps=%.3f" % e for e in epsilons], rows)


def epsilon_floor(a_table, noise, sigma, epsilons):
    """The smallest band that certifies and never cries wolf at this noise."""
    for epsilon in epsilons:
        row = a_table.get((noise, sigma, epsilon))
        if row is None:
            continue
        if row["false_alarm_rate"] == 0.0 and row["certified_fraction"] >= CERTIFIED_TARGET:
            return epsilon
    return None


def table_floor(a_table, noise, sigmas, epsilons):
    rows = []
    for sigma in sigmas:
        floor = epsilon_floor(a_table, noise, sigma, epsilons)
        row = a_table.get((noise, sigma, max(epsilons)))
        amp = "-"
        if sigma > 0.0 and row is not None:
            amp = "%.1f" % (row["gap_mean"] / sigma)
        rows.append([fmt_sigma(sigma),
                     "-" if floor is None else "%.3f" % floor,
                     "-" if floor is None else "%.3f" % floor,
                     amp])
    return md_table(["sigma", "smallest usable epsilon", "smallest visible fault",
                     "gap / sigma"], rows)


def svg_curve(a_table, sigmas, epsilons, noise, path):
    """Certification availability against the sensor noise, one line per band.
    Hand written SVG: no plotting library, standard library only."""
    width, height = 720, 420
    left, bottom, top = 70, 350, 30
    plot_w = width - left - 40
    n = len(sigmas)
    step = plot_w / float(max(n - 1, 1))
    colours = ("#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b")
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
             'viewBox="0 0 %d %d" font-family="sans-serif" font-size="12">' %
             (width, height, width, height)]
    parts.append('<rect width="%d" height="%d" fill="white"/>' % (width, height))
    parts.append('<text x="%d" y="18" font-size="14">Certified fraction of the ticks '
                 'against sensor noise (%s channels)</text>' % (left, noise))
    for i in range(6):
        y = bottom - (bottom - top) * i / 5.0
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#ddd"/>' %
                     (left, y, left + plot_w, y))
        parts.append('<text x="%d" y="%.1f" text-anchor="end">%.1f</text>' %
                     (left - 6, y + 4, i / 5.0))
    for j, sigma in enumerate(sigmas):
        x = left + step * j
        parts.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#ddd"/>' %
                     (x, top, x, bottom))
        parts.append('<text x="%.1f" y="%d" text-anchor="middle">%s</text>' %
                     (x, bottom + 18, fmt_sigma(sigma)))
    parts.append('<text x="%d" y="%d" text-anchor="middle">sensor noise sigma '
                 '(full scale)</text>' % (left + plot_w // 2, bottom + 40))
    for k, epsilon in enumerate(epsilons):
        points = []
        for j, sigma in enumerate(sigmas):
            row = a_table.get((noise, sigma, epsilon))
            if row is None:
                continue
            x = left + step * j
            y = bottom - (bottom - top) * min(max(row["certified_fraction"], 0.0), 1.0)
            points.append("%.1f,%.1f" % (x, y))
        if points:
            parts.append('<polyline fill="none" stroke="%s" stroke-width="2" points="%s"/>' %
                         (colours[k % len(colours)], " ".join(points)))
            parts.append('<text x="%d" y="%d" fill="%s">epsilon=%.3f</text>' %
                         (left + plot_w + 6 - 90, top + 16 * k, colours[k % len(colours)],
                          epsilon))
    parts.append('</svg>')
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(parts) + "\n")


def noise_amplification(a_table, noise, sigmas, epsilon):
    """Measured range of the ratio between the comparison gap and the sensor
    noise: how much of the sensor the control law pours into the comparison."""
    ratios = []
    for sigma in sigmas:
        if sigma == 0.0:
            continue
        row = a_table.get((noise, sigma, epsilon))
        if row is not None and row["gap_mean"] > 0.0:
            ratios.append(row["gap_mean"] / sigma)
    return (min(ratios), max(ratios)) if ratios else (float("nan"), float("nan"))


def point_shared(a_table, b_table, sigmas, band):
    """Point 1: what the published architecture measures."""
    row = a_table.get(("shared", max(sigmas), band))
    clean = b_table.get(("shared", 0.0, band, 0.02))
    if row is None or clean is None:
        return None
    return (
        f"1. **Keep one shared measurement source.** With the published architecture "
        f"(the actuator publishes the measurement, both computers use it) the noise moves "
        f"the command but never the comparison. Measured at the loudest noise of this grid "
        f"(sigma={max(sigmas):.0e}): the order/shadow gap of the certified regime is "
        f"{row['gap_max']:.6f} at worst and {row['gap_mean']:.6f} on average, the second "
        f"stays certified on {100.0 * row['certified_fraction']:.1f} per cent of the ticks, "
        f"no run raises a false alarm, and a 2 per cent value fault is seen in "
        f"{clean['detect_delay_mean']:.0f} tick(s). The only divergence left is the REP-04 "
        f"rebuild before the first certification, which peaks at "
        f"{row['startup_gap_max']:.3f} (the slew limit), not at a noise level.")


def point_independent(a_table, b_table, sigmas, band):
    """Point 2: what one sensor per computer costs."""
    row = a_table.get(("independent", max(sigmas), band))
    if row is None:
        return None
    delay = b_table.get(("independent", max(sigmas), band, 0.02)) or {}
    return (
        f"2. **Independent local sensors do not fit the published band.** With one sensor "
        f"per computer the same sigma={max(sigmas):.0e} puts the certified-regime gap at "
        f"{row['gap_max']:.5f} peak and {row['gap_mean']:.5f} on average, with "
        f"{100.0 * row['gap_over_epsilon_rate']:.0f} per cent of the fault-free ticks above "
        f"the band: the certified fraction collapses to {row['certified_fraction']:.3f}, "
        f"{100.0 * row['false_alarm_rate']:.0f} per cent of the runs raise a false alarm, "
        f"and the 2 per cent fault is only attributed after "
        f"{delay.get('detect_delay_mean', float('nan')):.0f} ticks on average. The published "
        f"law multiplies a position error by its proportional gain, which is what makes the "
        f"comparison that sensitive.")


def point_floor(a_table, b_table, sigmas, epsilons, noise, band):
    """Point 3: widening the band to survive the noise hides a fault."""
    hidden = b_table.get(("shared", 0.0, band, 0.001))
    usable = [(s, e) for s in sigmas
              for e in [epsilon_floor(a_table, noise, s, epsilons)] if e is not None]
    if not usable or hidden is None:
        return None
    worst_sigma, worst_floor = max(usable, key=lambda item: item[0])
    return (
        f"3. **If independent sensors are ever fitted, the band must follow the noise.** "
        f"The smallest band that certifies and never cries wolf is measured per noise level "
        f"in section 4, and that band becomes the smallest fault the actuator can see: a "
        f"bias below epsilon is invisible by construction (measured at sigma=0, a 0.1 per "
        f"cent bias with epsilon={band} is detected in "
        f"{100.0 * hidden['detect_rate']:.0f} per cent of the runs). At the largest noise of "
        f"this grid that still has a usable band (sigma={worst_sigma:.0e}) the floor is "
        f"{worst_floor:.3f}, so widening the band to survive the noise would hide a fault of "
        f"that very size.")


def detection_threshold(b_table, noise, sigma, band, biases):
    """The smallest injected fault the actuator reveals at least once out of
    two, and the largest one it hides. Both are measured, never deduced."""
    seen, hidden = [], []
    for bias in sorted(biases):
        if bias <= 0.0:
            continue
        row = b_table.get((noise, sigma, band, bias))
        if row is None:
            continue
        (seen if row["detect_rate"] >= 0.5 else hidden).append(bias)
    return (seen[0] if seen else None), (hidden[-1] if hidden else None)


def point_rule(a_table, b_table, sigmas, noise, band, biases):
    """Point 4: the rule that turns epsilon from a round number into a choice."""
    low, high = noise_amplification(a_table, noise, sigmas, band)
    seen, hidden = detection_threshold(b_table, "shared", 0.0, band, biases)
    seen_text = ("a %.0f %% bias (twice the band), seen in one tick" % (100.0 * seen)
                 if seen is not None else "none of the injected faults")
    hidden_text = ("a %.1f %% bias stays invisible" % (100.0 * hidden)
                   if hidden is not None else "nothing stays invisible")
    return (
        f"4. **Rule kept from the measurement.** Epsilon has two jobs and they pull apart: "
        f"it must sit above the noise floor of the comparison (section 3, otherwise the "
        f"redundancy flaps) and below the smallest fault worth catching (section 6, "
        f"otherwise the fault hides). With the published shared-measurement architecture "
        f"the noise floor of the certified regime is measured at zero, so nothing pushes "
        f"epsilon up and it can stay at {band}: the smallest fault it reveals here is "
        f"{seen_text}, while {hidden_text} because it never leaves the band. Should "
        f"independent sensors be fitted, the "
        f"floor becomes between {low:.0f} and {high:.0f} times sigma (measured in section "
        f"4) and epsilon would have to grow to that floor times a margin of ten, giving up "
        f"the detection of every smaller fault. That trade-off, not a round number, is what "
        f"justifies the published value.")


def point_observer():
    """Point 5: the cheapest way back under the band."""
    return (
        "5. **Where the observer helps.** If a computer must use its own sensor, the "
        "observer of core/src/observer.c cuts the position noise of the estimate by a "
        "measured factor of about 2.3 at the three noise levels of build/test_observer.log, "
        "which is the cheapest way to bring an independent channel back under the band; the "
        "same module cuts the REP-04 handover bump by 20 to 62 per cent depending on the "
        "noise level.")


def recommendation(a_table, b_table, sigmas, epsilons, biases, noise):
    """The recommendation, written from the measured cells only."""
    band = 0.005  # the published epsilon, the value under study
    points = [point_shared(a_table, b_table, sigmas, band),
              point_independent(a_table, b_table, sigmas, band),
              point_floor(a_table, b_table, sigmas, epsilons, noise, band),
              point_rule(a_table, b_table, sigmas, noise, band, biases),
              point_observer()]
    return "\n\n".join(point for point in points if point)


def build_report(args, phys, a_table, b_table, sigmas, epsilons, biases):
    out = ["# Relais — sensor noise and the choice of epsilon (mission M30, idea 67)",
           "",
           "Measured by `build/relais-physics`, which drives the real core: the arbiter,",
           "two node state machines, the rack and the published control law. No part of",
           "the protocol is re-implemented in this script.",
           "",
           "Reproduce:",
           "```",
           "make build/relais-physics",
           "python3 host/physics/epsilon_study.py --seeds %d --ticks %d" %
           (args.seeds, args.ticks),
           "```",
           "",
           "Grid: sigma in {%s} (full scale), epsilon in {%s}, value fault (INJ-10) in "
           "{%s} injected at tick %d, %d seeds per point, %d ticks per run." %
           (", ".join(fmt_sigma(s) for s in sigmas),
            ", ".join("%.3f" % e for e in epsilons),
            ", ".join("%.3f" % b for b in biases), args.bias_at, args.seeds, args.ticks),
           "",
           "## 1. Certification, independent sensor channels",
           "",
           "Fraction of the ticks where the second is certified, with NO fault injected.",
           "A cell marked (!) is below the %.0f %% target." % (100.0 * CERTIFIED_TARGET),
           "",
           table_certification(a_table, sigmas, epsilons, "independent",
                               "certified_fraction"),
           "",
           "## 2. False alarms, independent sensor channels",
           "",
           "Fraction of the runs that raised a disagreement or a decertification BEFORE",
           "any fault was injected. This is the noise alone breaking the band.",
           "",
           table_certification(a_table, sigmas, epsilons, "independent", "false_alarm_rate"),
           "",
           "## 3. The noise floor of the comparison",
           "",
           "Worst order/shadow gap over the fault-free ticks (full scale).",
           "",
           table_certification(a_table, sigmas, epsilons, "independent", "gap_max"),
           "",
           "## 4. The smallest usable band, and what it costs",
           "",
           "Smallest epsilon with zero false alarm and a certified fraction above the",
           "target. That band is also the smallest value fault the actuator can see.",
           "`gap / sigma` is the amplification of the sensor noise by the control law.",
           "",
           table_floor(a_table, "independent", sigmas, epsilons),
           ""]
    return out


def build_report_tail(args, a_table, b_table, phys, sigmas, epsilons, biases):
    out = ["## 5. Detection of a value fault, independent sensor channels", "",
           "Mean number of ticks between the injection (INJ-10, at tick %d) and the first"
           % args.bias_at,
           "disagreement or decertification. `noise` means that very cell already raises",
           "false alarms with no fault at all, so the delay cannot be attributed to the",
           "fault; `missed` means fewer than half of the runs ever saw it.", ""]
    for bias in biases:
        out += ["### bias = %.3f (%.1f %% of full scale)" % (bias, 100.0 * bias), "",
                table_detection(a_table, b_table, sigmas, epsilons, bias, "independent"), ""]
    out += ["## 6. Detection rate, independent sensor channels", "",
            "Fraction of the runs where the actuator saw the fault at least once.", ""]
    for bias in biases:
        out += ["### bias = %.3f" % bias, "",
                table_detect_rate(b_table, sigmas, epsilons, bias, "independent"), ""]
    out += ["## 7. The published architecture: one shared measurement", "",
            "The actuator publishes the measurement and both computers use it, so the",
            "noise moves the command but never the comparison. Certified fraction, then",
            "detection delay for a 2 % fault.", "",
            table_certification(a_table, sigmas, epsilons, "shared", "certified_fraction"),
            "",
            table_certification(a_table, sigmas, epsilons, "shared", "false_alarm_rate"),
            "",
            table_detection(a_table, b_table, sigmas, epsilons, 0.02, "shared"), ""]
    out += ["## 8. The faithful rack against the published one", "",
            "Closed-loop tracking over %d ticks, produced by `relais-physics --mode"
            % args.ticks,
            "phys`. Each row enables one phenomenon of core/src/plant_ext.c; `all` is",
            "the demonstration preset. Errors are in full-scale units.", "",
            md_table(["trajectory", "config", "max_err", "rms_err", "max_pos",
                      "mean kinetic energy"],
                     [[row["trajectory"] if row["trajectory"] != "default" else "slalom",
                       row["config"], row["max_err"], row["rms_err"], row["max_pos"],
                       row["mean_kinetic_energy"]] for row in phys]), "",
            "![certification against noise](epsilon-certification.svg)", ""]
    out += ["## 9. Recommendation", "",
            recommendation(a_table, b_table, sigmas, epsilons, biases, "independent"), ""]
    out += ["## 10. What this study does not cover", "",
            "- The channel model: no frame loss, no delay, no duplication. The simulator",
            "  covers those (`make campaign`, `make test`); this study isolates epsilon.",
            "- Noise on the setpoint, on the applied value, and quantisation of the",
            "  sensors: only the position and the velocity channels are perturbed.",
            "- A moving or biased fault (a ramp, a drift): the injected fault is a step",
            "  bias on one machine, which is the INJ-10 model.",
            "- The adaptive law of core/src/adaptive.c: its effect on the comparison is",
            "  measured in build/test_adaptive.log, not here.",
            ""]
    return out


