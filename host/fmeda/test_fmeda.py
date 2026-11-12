#!/usr/bin/env python3
"""Relais -- tests of the quantified FMEDA (mission M39).

Run with:
  python3 -m unittest discover -s host/fmeda -p 'test_*.py' -v

The tests check the arithmetic, not merely the execution:

1. the shares of an element sum to one, and the four rate classes sum to the
   rate of the element;
2. the metrics are recomputed independently from the classes and their
   identities hold (DC = SFF x (1 - SPF), SPFM = 1 - SPF, ...);
3. every ratio stays in [0, 1];
4. every rate is declared (HYPOTHESIS or a citation) and every diagnostic
   mechanism points at a file that exists in the repository;
5. the failure modes of the generated FMEA are all covered;
6. the sensitivity sweep covers every hypothesis and keeps the conclusion
   stable when every rate is scaled;
7. the generated report is stable and up to date.

Standard library only.
"""

from __future__ import annotations

import io
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import fmeda  # noqa: E402
import model  # noqa: E402
import report  # noqa: E402
import sensitivity  # noqa: E402

TOL = 1.0e-9
REQ_RE = re.compile(r"\b(CMD|MAN|DET|REL|REP|ACT|DEG|PERF|ROB|SEC|OBS|INJ|QNX)-\d+")
PATH_RE = re.compile(
    r"(?:core|host|actuator|target|docs|tools|image|scenarios|site)/"
    r"[A-Za-z0-9_.\-/]*")


def _result():
    return fmeda.compute()


class ArithmeticTest(unittest.TestCase):
    """The classes and the metrics must add up."""

    def test_shares_of_an_element_sum_to_one(self):
        for element in model.ELEMENTS:
            shares = [m["share"] for m in model.MODES
                      if m["element"] == element["id"]]
            self.assertTrue(shares, f"{element['id']} has no failure mode")
            self.assertAlmostEqual(sum(shares), 1.0, delta=TOL,
                                   msg=f"{element['id']} shares do not sum to 1")

    def test_row_classes_sum_to_the_row_rate(self):
        for row in model.expand(model.ELEMENTS, model.MODES):
            total = row["sd"] + row["su"] + row["dd"] + row["du"]
            self.assertAlmostEqual(total, row["lambda_fit"], delta=TOL,
                                   msg=f"{row['element']}/{row['mode']} does not add up")

    def test_row_rate_matches_the_element_formula(self):
        for row in model.expand(model.ELEMENTS, model.MODES):
            if row["rate_kind"] == "combination":
                continue
            element = next(e for e in model.ELEMENTS if e["id"] == row["element"])
            expected = element["lambda_fit"] * row["share"] * element["count"]
            self.assertAlmostEqual(row["lambda_fit"], expected, delta=TOL)

    def test_total_matches_the_elements(self):
        rows = model.expand(model.ELEMENTS, model.MODES)
        m = model.metrics(rows)
        shared = sum(e["lambda_fit"] * e["count"] for e in model.ELEMENTS)
        combined = sum(r["lambda_fit"] for r in rows
                       if r["rate_kind"] == "combination")
        self.assertAlmostEqual(m["total"], shared + combined, delta=TOL)

    def test_combination_rows_are_the_double_faults(self):
        combos = [r for r in model.expand(model.ELEMENTS, model.MODES)
                  if r["rate_kind"] == "combination"]
        self.assertEqual({r["code"] for r in combos}, {"F9", "F10"})
        for row in combos:
            self.assertTrue(row["residual"])
            self.assertEqual(row["effect"], "dangerous")
            self.assertEqual(row["share"], 0.0)

    def test_effect_and_class_agree(self):
        for row in model.expand(model.ELEMENTS, model.MODES):
            if row["effect"] == "safe":
                self.assertEqual(row["dd"], 0.0)
                self.assertEqual(row["du"], 0.0)
            else:
                self.assertEqual(row["sd"], 0.0)
                self.assertEqual(row["su"], 0.0)

    def test_metrics_are_recomputed_from_the_classes(self):
        rows = model.expand(model.ELEMENTS, model.MODES)
        m = model.metrics(rows)
        sd = sum(r["sd"] for r in rows)
        su = sum(r["su"] for r in rows)
        dd = sum(r["dd"] for r in rows)
        du = sum(r["du"] for r in rows)
        total = sd + su + dd + du
        self.assertAlmostEqual(m["dc"], (sd + dd) / total, delta=TOL)
        self.assertAlmostEqual(m["sff"], (sd + dd) / (sd + dd + su), delta=TOL)
        self.assertAlmostEqual(m["spf"], du / total, delta=TOL)
        self.assertAlmostEqual(m["spfm"], 1.0 - du / total, delta=TOL)
        self.assertAlmostEqual(m["lfm"], 1.0 - su / total, delta=TOL)

    def test_metric_identities(self):
        m = model.metrics(model.expand(model.ELEMENTS, model.MODES))
        # DC = SFF x (1 - SPF): the identity of IEC 61508 with these definitions.
        self.assertAlmostEqual(m["dc"], m["sff"] * (1.0 - m["spf"]), delta=TOL)
        self.assertAlmostEqual(m["spfm"] + m["spf"], 1.0, delta=TOL)
        self.assertAlmostEqual(m["lfm"] + m["su"] / m["total"], 1.0, delta=TOL)

    def test_ratios_stay_in_range(self):
        m = model.metrics(model.expand(model.ELEMENTS, model.MODES))
        for name in ("dc", "sff", "spf", "spfm", "lfm"):
            self.assertGreaterEqual(m[name], 0.0, name)
            self.assertLessEqual(m[name], 1.0, name)

    def test_residual_term_is_negligible(self):
        m = model.metrics(model.expand(model.ELEMENTS, model.MODES))
        self.assertLess(m["residual"] / m["total"], 5.0e-3,
                        "the computed double-fault term must stay negligible")
        self.assertLess(m["residual"] / m["pmhf_fit"], 1.0e-2,
                        "the double-fault term must stay negligible in the PMHF")
        self.assertGreaterEqual(m["pmhf_fit"], m["du"])


class HonestyTest(unittest.TestCase):
    """Every number must be declared, and every mechanism must exist."""

    def test_every_element_rate_is_declared(self):
        for element in model.ELEMENTS:
            self.assertIn(element["source"], ("HYPOTHESIS", "CITED"),
                          f"{element['id']} has no declared source")
            self.assertTrue(element["rationale"],
                            f"{element['id']} has no rationale")

    def test_every_share_is_declared(self):
        for mode in model.MODES:
            self.assertIn("HYPOTHESIS", mode["share_source"],
                          f"{mode['element']}/{mode['mode']} has no declared share")

    def test_every_mechanism_cites_a_requirement(self):
        for mode in model.MODES:
            self.assertTrue(REQ_RE.search(mode["requirements"]),
                            f"{mode['element']}/{mode['mode']} cites no requirement")

    def test_every_mechanism_points_at_an_existing_file(self):
        for mode in model.MODES:
            paths = PATH_RE.findall(mode["evidence"])
            self.assertTrue(paths,
                            f"{mode['element']}/{mode['mode']} points at no file")
            for token in paths:
                path = os.path.join(ROOT, token)
                self.assertTrue(os.path.exists(path),
                                f"{token} does not exist: no mechanism may be "
                                "credited without a file")

    def test_no_coverage_without_a_mechanism(self):
        for mode in model.MODES:
            if mode["dc"] == "none":
                self.assertTrue(mode["mechanism"].startswith("none"),
                                f"{mode['element']}/{mode['mode']} has no coverage "
                                "but claims a mechanism")
            else:
                self.assertFalse(mode["mechanism"].startswith("none"),
                                 f"{mode['element']}/{mode['mode']} claims a "
                                 "coverage without a mechanism")

    def test_diagnostic_coverage_values_are_the_cited_ones(self):
        self.assertEqual(model.DC_TARGETS["high"], 0.99)
        self.assertEqual(model.DC_TARGETS["medium"], 0.90)
        self.assertEqual(model.DC_TARGETS["low"], 0.60)
        self.assertIn("ISO 26262-5", model.DC_SOURCE)

    def test_report_states_the_hypothesis_marker(self):
        markdown = fmeda.build_document(_result())
        self.assertIn("HYPOTHESIS", markdown)
        self.assertIn("ISO 26262-5", markdown)
        self.assertIn("ISO 26262-2:2018", markdown)


class FmeaCoverageTest(unittest.TestCase):
    """The FMEDA must cover the failure modes of the generated FMEA."""

    def test_every_fmea_code_is_covered(self):
        codes = {mode["code"] for mode in model.MODES}
        self.assertEqual(codes, set(model.safety_codes()),
                         "the FMEDA must cover every F-code of the safety model")

    def test_every_mode_is_unique(self):
        seen = set()
        for mode in model.MODES:
            key = (mode["element"], mode["code"], mode["mode"])
            self.assertNotIn(key, seen, "duplicated failure mode")
            seen.add(key)


class AsilTest(unittest.TestCase):
    """The ASIL conclusion must follow the cited criteria."""

    def test_nominal_conclusion_is_derived(self):
        m = model.metrics(model.expand(model.ELEMENTS, model.MODES))
        level, _limiting = model.reachable_asil(
            m, model.PMHF_TARGETS[sensitivity.EDITION_2018])
        criteria = model.asil_criteria(m, model.PMHF_TARGETS[sensitivity.EDITION_2018])
        for lvl in ("D", "C", "B"):
            failed = [n for n, _v, _t, ok in criteria[lvl] if not ok]
            self.assertTrue(failed, f"ASIL {lvl} must not be met by the nominal model")
        self.assertEqual(level, "A")

    def test_the_conclusion_does_not_depend_on_the_edition(self):
        m = model.metrics(model.expand(model.ELEMENTS, model.MODES))
        level_2018, _ = model.reachable_asil(
            m, model.PMHF_TARGETS[sensitivity.EDITION_2018])
        level_2009, _ = model.reachable_asil(
            m, model.PMHF_TARGETS[sensitivity.EDITION_2009])
        self.assertEqual(level_2018, level_2009)

    def test_the_limiting_criterion_is_the_spfm(self):
        m = model.metrics(model.expand(model.ELEMENTS, model.MODES))
        criteria = model.asil_criteria(m, model.PMHF_TARGETS[sensitivity.EDITION_2018])
        for lvl in ("D", "C", "B"):
            failed = [n for n, _v, _t, ok in criteria[lvl] if not ok]
            self.assertIn("SPFM", failed)


class SensitivityTest(unittest.TestCase):
    """The hypotheses must be bounded by the sweep."""

    def test_every_element_is_swept_both_ways(self):
        results = sensitivity.sweep(model.ELEMENTS, model.MODES)
        names = [r["case"] for r in results]
        for element in model.ELEMENTS:
            self.assertIn(f"lambda({element['id']}) x0.1", names)
            self.assertIn(f"lambda({element['id']}) x10", names)
        self.assertIn("all lambda x0.1", names)
        self.assertIn("all lambda x10", names)

    def test_global_scale_keeps_the_ratios(self):
        results = {r["case"]: r for r in
                   sensitivity.sweep(model.ELEMENTS, model.MODES)}
        base = results["all lambda x0.1"]
        for case in ("all lambda x0.1", "all lambda x10"):
            row = results[case]
            # The double-fault rows scale with the square of the rates, so the
            # ratios are stable up to that (negligible) residual term.
            self.assertAlmostEqual(row["dc"], base["dc"], delta=0.02)
            self.assertAlmostEqual(row["sff"], base["sff"], delta=0.02)
            self.assertAlmostEqual(row["spf"], base["spf"], delta=0.02)
            self.assertAlmostEqual(row["spfm"], base["spfm"], delta=0.02)
        self.assertAlmostEqual(results["all lambda x10"]["pmhf"],
                               100.0 * results["all lambda x0.1"]["pmhf"],
                               delta=0.1 * results["all lambda x10"]["pmhf"])

    def test_global_scale_keeps_the_conclusion(self):
        results = {r["case"]: r for r in
                   sensitivity.sweep(model.ELEMENTS, model.MODES)}
        self.assertEqual(results["all lambda x0.1"]["asil"], "A")
        self.assertEqual(results["all lambda x10"]["asil"], "A")

    def test_more_actuator_failures_lower_the_spfm(self):
        results = {r["case"]: r for r in
                   sensitivity.sweep(model.ELEMENTS, model.MODES)}
        low = results["lambda(ACT-PWR) x0.1"]
        high = results["lambda(ACT-PWR) x10"]
        self.assertGreater(low["spfm"], high["spfm"])
        self.assertLess(low["pmhf"], high["pmhf"])

    def test_inversion_is_a_real_boundary(self):
        factor, rate, _spfm = sensitivity.max_rate_for_asil(
            model.ELEMENTS, model.MODES, fmeda.ACTUATOR, "B")
        self.assertLess(rate, sum(e["lambda_fit"] for e in model.ELEMENTS
                                  if e["id"] in fmeda.ACTUATOR))
        below = sensitivity.max_rate_for_asil(
            model.ELEMENTS, model.MODES, fmeda.ACTUATOR, "B")
        above = sensitivity.max_rate_for_asil(
            model.ELEMENTS, model.MODES, fmeda.ACTUATOR, "B", None)
        self.assertAlmostEqual(below[0], above[0], delta=TOL)

    def test_inversion_is_respected(self):
        factor, _rate, _spfm = sensitivity.max_rate_for_asil(
            model.ELEMENTS, model.MODES, fmeda.ACTUATOR, "B")
        elements = [dict(e) for e in model.ELEMENTS]
        for element in elements:
            if element["id"] in fmeda.ACTUATOR:
                element["lambda_fit"] = element["lambda_fit"] * factor
        rows = model.expand(elements, model.MODES)
        m = model.metrics(rows)
        self.assertGreaterEqual(m["spfm"], model.SPFM_TARGET["B"] - 1.0e-9)

    def test_inversion_is_tight(self):
        factor, _rate, _spfm = sensitivity.max_rate_for_asil(
            model.ELEMENTS, model.MODES, fmeda.ACTUATOR, "B")
        elements = [dict(e) for e in model.ELEMENTS]
        for element in elements:
            if element["id"] in fmeda.ACTUATOR:
                element["lambda_fit"] = element["lambda_fit"] * factor * 1.001
        rows = model.expand(elements, model.MODES)
        m = model.metrics(rows)
        self.assertLess(m["spfm"], model.SPFM_TARGET["B"])


class StabilityTest(unittest.TestCase):
    """The generated report must be stable and up to date."""

    def test_markdown_is_stable(self):
        result = _result()
        self.assertEqual(fmeda.build_document(result),
                         fmeda.build_document(result))

    def test_csv_is_stable(self):
        rows = model.expand(model.ELEMENTS, model.MODES)
        self.assertEqual(report.render_csv(rows), report.render_csv(rows))

    def test_report_on_disk_is_up_to_date(self):
        result = _result()
        for path, text in ((fmeda.DEFAULT_MD, fmeda.build_document(result)),
                           (fmeda.DEFAULT_CSV,
                            report.render_csv(result["rows"]))):
            self.assertTrue(os.path.exists(path), f"{path} is missing")
            with io.open(path, "r", encoding="utf-8") as fh:
                self.assertEqual(fh.read(), text, f"{path} is out of date")

    def test_check_mode_passes(self):
        self.assertEqual(fmeda.main(["--check"]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
