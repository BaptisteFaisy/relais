#!/usr/bin/env python3
"""Tests of the feature-catalogue generator (stdlib only).

The page is published, so the generator is guarded like the other CI guards:
a card whose files do not exist yet must not appear, a module nobody listed must
still appear, and the injection point must be honoured.
"""
import importlib.util
import json
import pathlib
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
CATALOG = {
    "repo": "https://example.invalid/owner/repo",
    "themes": [{
        "id": "theme-one", "title": "Theme one", "hook": "A hook.",
        "features": [
            {"title": "A shipped feature", "summary": "It does something.",
             "requirements": ["SEC-01", "CMD-02"], "docs": ["docs/DEMO.md"],
             "code": ["host/demo/demo.py"], "proof": "make test"},
            {"title": "Not merged yet", "summary": "Its files are absent.",
             "requirements": ["INJ-99"], "docs": ["docs/ABSENT.md"],
             "code": ["host/absent/x.c"]},
        ],
    }],
}


def load_generator():
    spec = importlib.util.spec_from_file_location("bf", HERE / "build_features.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeTree(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "host" / "demo").mkdir(parents=True)
        (self.root / "host" / "demo" / "demo.py").write_text("VALUE = 1\n")
        (self.root / "host" / "unlisted").mkdir(parents=True)
        (self.root / "host" / "unlisted" / "tool.py").write_text("print(1)\n")
        (self.root / "docs").mkdir()
        (self.root / "docs" / "DEMO.md").write_text("# Demo\n")
        (self.root / "docs" / "UNLISTED.md").write_text("# Unlisted\n")
        (self.root / "site").mkdir()
        (self.root / "site" / "features.json").write_text(json.dumps(CATALOG))
        self.gen = load_generator()

    def fragment(self):
        catalog = self.gen.load_catalog(self.root / "site" / "features.json")
        return self.gen.render_fragment(catalog["repo"], self.root, catalog)

    def test_shipped_feature_is_rendered(self):
        f = self.fragment()
        self.assertIn("A shipped feature", f)
        self.assertIn("SEC-01", f)
        self.assertIn("make test", f)
        self.assertIn("example.invalid/owner/repo/blob/main/docs/DEMO.md", f)

    def test_feature_without_any_file_is_skipped(self):
        self.assertNotIn("Not merged yet", self.fragment())

    def test_unlisted_module_and_doc_are_discovered(self):
        f = self.fragment()
        self.assertIn("host/unlisted", f)
        self.assertIn("docs/UNLISTED.md", f)
        self.assertIn("Everything else in the tree", f)

    def test_listed_paths_are_not_duplicated_in_discovery(self):
        f = self.fragment()
        discovered = f.split("Everything else in the tree", 1)[1]
        self.assertNotIn("host/demo", discovered)
        self.assertNotIn("docs/DEMO.md", discovered)

    def test_table_of_contents_links_every_theme(self):
        self.assertIn('href="#theme-one"', self.fragment())

    def test_injection_honours_the_marker(self):
        site = self.root / "_site"
        site.mkdir()
        (site / "index.html").write_text("<html><body><!--FEATURES--></body></html>")
        self.run_main(["--repo-root", str(self.root), "--site-dir", str(site)])
        text = (site / "index.html").read_text()
        self.assertIn("A shipped feature", text)
        self.assertNotIn("<!--FEATURES-->", text)

    def test_without_marker_a_standalone_page_is_written(self):
        site = self.root / "_site2"
        self.run_main(["--repo-root", str(self.root), "--site-dir", str(site)])
        page = site / "features.html"
        self.assertTrue(page.is_file())
        self.assertIn("<!doctype html>", page.read_text().lower())

    def run_main(self, argv):
        import sys
        old = sys.argv
        sys.argv = ["build_features.py"] + argv
        try:
            self.assertEqual(self.gen.main(), 0)
        finally:
            sys.argv = old


if __name__ == "__main__":
    unittest.main(verbosity=2)
