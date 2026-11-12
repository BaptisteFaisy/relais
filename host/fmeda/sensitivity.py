#!/usr/bin/env python3
"""Relais -- sensitivity of the FMEDA conclusion (mission M39).

Every failure rate of the model is a HYPOTHESIS, so the conclusion must be
bounded. This module does three things:

1. **Sweep** -- every hypothesis parameter is multiplied by 10 and by 1/10, and
   the whole metric set plus the reachable ASIL is recomputed. The table that
   comes out says which assumption decides the conclusion and which one does
   not.
2. **Global scale** -- all rates multiplied by 10 and by 1/10 at once. The
   ratios (DC, SFF, SPF, SPFM, LFM) do not move; only the absolute PMHF moves.
   It is the cheapest proof that the conclusion does not hide inside the
   absolute numbers.
3. **Inversion** -- for a given ASIL level, the largest failure rate the
   dominant element may carry and still meet the criteria. That answers "what
   would have to be true" instead of "what is true".

The ASIL level is evaluated against the PMHF targets of both editions of the
standard (2018 and 2009), so that the conclusion does not depend on the copy
of the standard in hand.

Standard library only, deterministic.
"""

from __future__ import annotations

import copy

import model

FACTORS = (0.1, 10.0)
EDITION_2018 = "ISO 26262-2:2018 Table 4"
EDITION_2009 = "ISO 26262-4:2009 Table 4"


def _scale_element(elements, element_id, factor):
    """Return a copy of the elements with one element's rate scaled."""
    out = copy.deepcopy(elements)
    for element in out:
        if element["id"] == element_id:
            element["lambda_fit"] = element["lambda_fit"] * factor
    return out


def _scale_all(elements, factor):
    """Return a copy of the elements with every rate scaled."""
    out = copy.deepcopy(elements)
    for element in out:
        element["lambda_fit"] = element["lambda_fit"] * factor
    return out


def _shift_f8_split(modes, factor):
    """Move the F8 share between 'shadow exists' and 'no shadow'."""
    out = copy.deepcopy(modes)
    for mode in out:
        if mode["element"] != "ECU" or mode["code"] != "F8":
            continue
        if "no certified shadow" in mode["mode"]:
            mode["share"] = mode["share"] * factor
        else:
            mode["share"] = mode["share"] - (mode["share"] * (factor - 1.0) / factor)
    return out


def _degrade_dc(modes):
    """Every 'high' diagnostic class falls back to 'medium'."""
    out = copy.deepcopy(modes)
    for mode in out:
        if mode["dc"] == "high":
            mode["dc"] = "medium"
    return out


def _raise_dc(modes):
    """Every 'medium' diagnostic class is raised to 'high'."""
    out = copy.deepcopy(modes)
    for mode in out:
        if mode["dc"] == "medium":
            mode["dc"] = "high"
    return out


def lift_actuator(modes):
    """Scenario: the actuator gains a runtime diagnostic (watchdog, plausibility)."""
    out = copy.deepcopy(modes)
    for mode in out:
        if mode["element"] in ("ACT-CTRL", "ACT-PWR") and mode["effect"] == "dangerous":
            mode["dc"] = "medium"
    return out


def evaluate(elements, modes):
    """Expand, compute the metrics and the reachable ASIL for both editions."""
    rows = model.expand(elements, modes)
    m = model.metrics(rows)
    level_2018, _l1 = model.reachable_asil(m, model.PMHF_TARGETS[EDITION_2018])
    level_2009, _l2 = model.reachable_asil(m, model.PMHF_TARGETS[EDITION_2009])
    return m, level_2018, level_2009


def _element_cases(elements):
    """Cases that scale the rate of one element."""
    cases = []
    for element in elements:
        for factor in FACTORS:
            cases.append((
                f"lambda({element['id']}) x{factor:g}",
                f"failure rate of {element['id']} multiplied by {factor:g}",
                lambda e, m, el=element["id"], f=factor: (_scale_element(e, el, f), m),
            ))
    return cases


def _global_cases():
    """Cases that scale every rate at once, or move one share."""
    cases = []
    for factor in FACTORS:
        cases.append((
            f"all lambda x{factor:g}",
            "every failure rate multiplied by the same factor",
            lambda e, m, f=factor: (_scale_all(e, f), m),
        ))
    for factor in FACTORS:
        cases.append((
            f"share(F8 without shadow) x{factor:g}",
            "the wrong-value cases that happen with no certified shadow",
            lambda e, m, f=factor: (e, _shift_f8_split(m, f)),
        ))
    return cases


def _coverage_cases():
    """Cases that move the diagnostic coverage, or add a diagnostic."""
    cases = [
        ("DC: high -> medium",
         "every 'high' diagnostic class falls back to 'medium' (90 %)",
         lambda e, m: (e, _degrade_dc(m))),
        ("DC: medium -> high",
         "every 'medium' diagnostic class is raised to 'high' (99 %)",
         lambda e, m: (e, _raise_dc(m))),
        ("scenario: runtime diagnostic on the actuator",
         "watchdog and plausibility check on the actuator (DC 90 % on its "
         "dangerous modes)",
         lambda e, m: (e, lift_actuator(m))),
    ]
    return cases


def sweep(elements, modes):
    """Return the sensitivity table: one entry per hypothesis and per factor."""
    cases = _element_cases(elements) + _global_cases() + _coverage_cases()
    results = []
    for name, description, transform in cases:
        elements_c, modes_c = transform(copy.deepcopy(elements), copy.deepcopy(modes))
        m, level_2018, level_2009 = evaluate(elements_c, modes_c)
        results.append({
            "case": name,
            "description": description,
            "dc": m["dc"],
            "sff": m["sff"],
            "spf": m["spf"],
            "spfm": m["spfm"],
            "lfm": m["lfm"],
            "pmhf": m["pmhf_fit"],
            "total": m["total"],
            "asil": level_2018,
            "asil_2009": level_2009,
        })
    return results


def max_rate_for_asil(elements, modes, element_ids, level, modes_transform=None):
    """Largest rate of a group of elements that still meets the ASIL level.

    Returns (factor, lambda_fit, spfm). The search is a bisection: the SPFM
    decreases monotonically when the rate of the group grows, because the
    share of that group in the dangerous-undetected total is constant. An
    infinite factor means the criterion does not constrain this group (it is
    then the PMHF criterion that decides).
    """
    base = sum(e["lambda_fit"] for e in elements if e["id"] in element_ids)
    modes_c = modes_transform(modes) if modes_transform else modes

    def spfm_at(factor):
        elements_c = copy.deepcopy(elements)
        for element in elements_c:
            if element["id"] in element_ids:
                element["lambda_fit"] = element["lambda_fit"] * factor
        rows = model.expand(elements_c, modes_c)
        return model.metrics(rows)["spfm"]

    target = model.SPFM_TARGET[level]
    lo, hi = 1.0e-6, 1.0e6
    if spfm_at(hi) >= target:
        return float("inf"), float("inf"), spfm_at(hi)
    for _ in range(200):
        mid = (lo + hi) / 2.0
        if spfm_at(mid) >= target:
            lo = mid
        else:
            hi = mid
    return lo, base * lo, spfm_at(lo)
