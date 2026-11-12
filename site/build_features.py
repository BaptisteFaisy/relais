#!/usr/bin/env python3
"""Build the public feature catalogue of the site (stdlib only).

Why a generator and not a hand-written list: every merged mission must appear on
the public page without anyone remembering to edit it. The curated entries in
site/features.json give the readable part; anything present in the tree that no
entry mentions is discovered and listed anyway, so nothing can hide.

Usage:
    python3 site/build_features.py --repo-root . --site-dir _site
"""
import argparse
import html
import json
import pathlib
import sys

NL = chr(10)

# scenarios/ n'est pas scanné fichier par fichier : il est résumé en une ligne
# (dix scénarios de faute) plus bas.
SCAN_ROOTS = ("core", "actuator", "host", "target", "ports", "transports",
              "tools", "profiles", "codelab")
SELF_PAGES = ("site/index.html", "site/features.html")
SCAN_GLOBS = ("docs/*.md", "site/*.html", "site/*.js", ".github/workflows/*.yml")
SKIP_DIRS = {".git", "build", "runs", "__pycache__", "states", "node_modules",
             "figures", "guest-build"}
SOURCE_SUFFIX = (".c", ".h", ".py", ".rs", ".sh", ".js", ".css", ".json", ".tla")


def load_catalog(path):
    """Read the curated catalogue; an absent file is not an error."""
    if not path.is_file():
        return {"repo": "", "themes": []}
    return json.loads(path.read_text(encoding="utf-8"))


def escape(text):
    return html.escape(str(text or ""), quote=True)


def github_url(repo, path):
    if not repo:
        return ""
    return "%s/tree/main/%s" % (repo.rstrip("/"), path) if is_dir(repo, path) \
        else "%s/blob/main/%s" % (repo.rstrip("/"), path)


def is_dir(repo, path):
    return "." not in pathlib.Path(path).name


def count_files(root, path):
    """Files and source lines under a path, for the discovered entries."""
    target = root / path
    files = [target] if target.is_file() else [
        p for p in target.rglob("*")
        if p.is_file() and not (set(p.parts) & SKIP_DIRS)]
    lines = 0
    for f in files:
        if f.suffix in SOURCE_SUFFIX:
            try:
                lines += len(f.read_text(encoding="utf-8", errors="replace").splitlines())
            except OSError:
                pass
    return len(files), lines


def render_links(repo, paths, root, label_none="—"):
    """One link per path; a path that does not exist is skipped (mission not merged yet)."""
    out = []
    for p in paths or []:
        if not (root / p).exists():
            continue
        out.append('<a href="%s">%s</a>' % (escape(github_url(repo, p)), escape(p)))
    return " · ".join(out) if out else label_none


def render_badges(items):
    return "".join('<span class="req">%s</span>' % escape(i) for i in items or [])


def render_feature(repo, root, f):
    """One feature card. Returns None when nothing it points at exists yet."""
    docs = [p for p in (f.get("docs") or []) if (root / p).exists()]
    code = [p for p in (f.get("code") or []) if (root / p).exists()]
    if not docs and not code:
        return None
    proof = f.get("proof")
    proof_html = ('<p class="proof">reproduce: <code>%s</code></p>' % escape(proof)
                  if proof else "")
    return (
        '<article class="feature">\n'
        '<h4>%s</h4>\n<p>%s</p>\n'
        '<p class="reqrow">%s</p>\n%s%s\n</article>' % (
            escape(f.get("title")), escape(f.get("summary")),
            render_badges(f.get("requirements")), proof_html,
            '<p class="paths">%s</p>' % render_links(repo, docs + code, root)))


def render_theme(repo, root, theme):
    cards = [c for c in (render_feature(repo, root, f) for f in theme.get("features", []))
             if c]
    if not cards:
        return "", 0
    hook = ('<p class="hook">%s</p>' % escape(theme["hook"]) if theme.get("hook") else "")
    body = ('<section class="theme" id="%s">\n<h3>%s</h3>\n%s\n<div class="grid g2">\n%s\n'
            '</div>\n</section>' % (escape(theme.get("id", "theme")),
                                    escape(theme.get("title")), hook, "\n".join(cards)))
    return body, len(cards)


def covered_paths(catalog):
    """Every path a curated entry mentions (a directory covers what is below it)."""
    out = set()
    for theme in catalog.get("themes", []):
        for f in theme.get("features", []):
            for p in (f.get("docs") or []) + (f.get("code") or []):
                out.add(p.rstrip("/"))
    return out


def is_covered(rel, covered):
    return any(rel == c or rel.startswith(c + "/") or c.startswith(rel + "/")
               for c in covered)


def discover(root, covered):
    """Modules, docs and workflows of the tree that NO curated entry mentions.

    This is what makes the page complete by construction: a merged mission that
    nobody added to the catalogue still shows up here, with its file count.
    """
    out = []
    for base in SCAN_ROOTS:
        top = root / base
        if not top.is_dir():
            continue
        for sub in sorted(top.iterdir()):
            if sub.name in SKIP_DIRS or sub.name.startswith("."):
                continue
            rel = sub.relative_to(root).as_posix()
            if is_covered(rel, covered):
                continue
            if sub.is_dir():
                files, lines = count_files(root, rel)
                if files:
                    out.append((rel, files, lines))
            elif sub.suffix in SOURCE_SUFFIX:
                out.append((rel, 1, len(sub.read_text(errors="replace").splitlines())))
    for pattern in SCAN_GLOBS:
        for f in sorted(root.glob(pattern)):
            rel = f.relative_to(root).as_posix()
            if is_covered(rel, covered) or rel in SELF_PAGES \
                    or f.name.startswith(("build_features", "test_build_features")):
                continue
            out.append((rel, 1, len(f.read_text(errors="replace").splitlines())))
    scen = [] if is_covered("scenarios", covered) else (
        sorted((root / "scenarios").glob("*.json")) if (root / "scenarios").is_dir() else [])
    scen = [s for s in scen if not is_covered(s.relative_to(root).as_posix(), covered)]
    if scen:
        out.append(("scenarios/ (%d fault scenarios)" % len(scen), len(scen), 0))
    return out


def render_discovered(repo, root, items):
    if not items:
        return ""
    rows = []
    for rel, files, lines in items:
        path = rel.split(" (")[0]
        link = ('<a href="%s">%s</a>' % (escape(github_url(repo, path)), escape(rel))
                if (root / path).exists() else escape(rel))
        size = ("%d file%s" % (files, "s" if files > 1 else "")) + \
               (", %d lines" % lines if lines else "")
        rows.append("<tr><td>%s</td><td class=\"num\">%s</td></tr>" % (link, escape(size)))
    return ('<section class="theme" id="discovered">\n<h3>Everything else in the tree</h3>\n'
            '<p class="hook">Discovered automatically at deploy time: nothing that is '
            'merged can stay invisible, even if nobody added it to the catalogue '
            'above.</p>\n<table class="discovered">\n%s\n</table>\n</section>'
            % "\n".join(rows))


def render_toc(pairs):
    """Table of contents: one link per theme, so the page can be skimmed."""
    items = []
    for _body, theme in pairs:
        items.append('<a href="#%s">%s</a>' % (escape(theme.get("id", "theme")),
                                     escape(theme.get("title", ""))))
    if not items:
        return ""
    return ('<nav class="toc"><p class="toc-title">Jump to</p><p>%s</p></nav>'
            % " · ".join(items))


def render_fragment(repo, root, catalog):
    themes, total, pairs = [], 0, []
    for theme in catalog.get("themes", []):
        body, n = render_theme(repo, root, theme)
        if body:
            themes.append(body)
            pairs.append((body, theme))
            total += n
    found = discover(root, covered_paths(catalog))
    head = ('<h2 id="features">Every feature, in one page</h2>\n'
            '<p>%d capabilities in %d themes, plus %d item%s discovered in the tree. '
            'Each card names the requirements it answers, the document that argues '
            'it, the code that implements it, and the command that reproduces the '
            'number.</p>' % (total, len(themes), len(found), "s" if len(found) != 1 else ""))
    return NL.join([head, render_toc(pairs)] + themes + [render_discovered(repo, root, found)])


PAGE_SHELL = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Relais — feature catalogue</title>
<link rel="stylesheet" href="style.css"></head>
<body><main class="wrap">
<h1>Relais — feature catalogue</h1>
%s
</main></body></html>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--site-dir", default="_site")
    ap.add_argument("--catalog", default=None)
    args = ap.parse_args()
    root = pathlib.Path(args.repo_root)
    site = pathlib.Path(args.site_dir)
    catalog = load_catalog(pathlib.Path(args.catalog or (root / "site/features.json")))
    repo = catalog.get("repo", "")
    fragment = render_fragment(repo, root, catalog)
    site.mkdir(parents=True, exist_ok=True)
    index = site / "index.html"
    marker = "<!--FEATURES-->"
    if index.is_file() and marker in index.read_text(encoding="utf-8"):
        text = index.read_text(encoding="utf-8")
        index.write_text(text.replace(marker, fragment, 1), encoding="utf-8")
        print("catalogue injected into %s" % index)
    else:
        (site / "features.html").write_text(PAGE_SHELL % fragment, encoding="utf-8")
        print("no %s marker in index.html -> standalone %s written"
              % (marker, site / "features.html"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
