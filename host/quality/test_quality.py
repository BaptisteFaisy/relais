#!/usr/bin/env python3
"""Tests for the SBOM generator and the anti-dependency guard (stdlib only).

Run with: python3 -m unittest discover -s host/quality -p 'test_*.py' -v
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sbom  # noqa: E402


class SbomTests(unittest.TestCase):
    def test_cyclonedx_structure(self):
        bom = sbom.generate(".")
        self.assertEqual(bom["bomFormat"], "CycloneDX")
        self.assertEqual(bom["specVersion"], "1.4")
        self.assertIn("components", bom)
        self.assertGreaterEqual(len(bom["components"]), 4)
        names = {component["name"] for component in bom["components"]}
        for expected in ("relais-core", "relais-host", "relais-target"):
            self.assertIn(expected, names)

    def test_no_external_dependency_declared(self):
        bom = sbom.generate(".")
        for component in bom["components"]:
            props = {p["name"]: p["value"]
                     for p in component.get("properties", [])}
            self.assertEqual(props.get("external-dependencies"), "none")

    def test_json_serializable(self):
        # json.dumps must succeed and round-trip the mandatory fields.
        text = json.dumps(sbom.generate("."))
        data = json.loads(text)
        self.assertEqual(data["specVersion"], "1.4")

    def test_clean_source_has_no_violation(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "ok.c"), "w", encoding="utf-8") as h:
                h.write("#include <stdio.h>\n#include <math.h>\n"
                        "#include <stdint.h>\nint main(void){return 0;}\n")
            self.assertEqual(sbom.check_no_external_includes(tmp), [])

    def test_guard_detects_openssl(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "bad.c"), "w", encoding="utf-8") as h:
                h.write("#include <openssl/ssl.h>\nint main(void){return 0;}\n")
            violations = sbom.check_no_external_includes(tmp)
            self.assertTrue(any("openssl/ssl.h" in header
                                for _, header in violations))


if __name__ == "__main__":
    unittest.main()