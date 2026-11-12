#!/usr/bin/env python3
"""Relais — assemble the nightly report from the per-job JSON artifacts.

The final CI job downloads every artifact (one directory per job), this script
turns them into a dated markdown report, writes a `results/verdict` marker the
workflow reads to decide red/green, and never exits non-zero on red: opening
the issue is the workflow's job, not this script's.

Requirement IDs: OBS-05 (the report cites what was run and when), REP-06 (the
same numbers, re-run and dated).
"""
import argparse
import datetime
import json
import os
import sys


def paris_now():
    try:
        from zoneinfo import ZoneInfo
        return datetime.datetime.now(ZoneInfo("Europe/Paris")).strftime(
            "%Y-%m-%d %H:%M %Z")
    except Exception:
        return datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y-%m-%d %H:%M UTC")


def load(results_dir):
    artifacts = []
    for root, _dirs, files in os.walk(results_dir):
        for name in files:
            if not name.endswith(".json"):
                continue
            path = os.path.join(root, name)
            try:
                with open(path, encoding="utf-8") as fh:
                    data = json.load(fh)
            except (ValueError, OSError):
                continue
            if isinstance(data, dict) and "scale" in data:
                artifacts.append(data)
    return sorted(artifacts, key=lambda a: (a.get("scale"), a.get("shard", 0)))


def render(artifacts, run_url, repo):
    lines = ["# Relais — nightly replay", "",
             "- Repository: `%s`" % repo,
             "- Paris date: %s" % paris_now(),
             "- Run: %s" % run_url,
             ""]
    header = "| Probe | Scope | Violations | Duration (s) |"
    lines += [header, "|---|---|---|---|"]
    total_violations = 0
    for a in artifacts:
        scope = a.get("scale", "?")
        if a.get("kind") == "campaign":
            scope = "campaign-%s" % a.get("kind")
        if a.get("shards"):
            scope += " (%d/%d)" % (a.get("shard", 0) + 1, a.get("shards"))
        v = int(a.get("violations", 0) or 0)
        total_violations += v
        extra = ""
        if a.get("scale") == "mutation-score" and a.get("score_percent") is not None:
            extra = " — score %.1f %% (survivors %s)" % (
                a["score_percent"] * 100, a.get("survived"))
        lines.append("| %s | %s | %d | %s |%s" % (
            scope, a.get("ticks", a.get("states", a.get("checks",
            a.get("runs", a.get("flagged", ""))))),
            v, a.get("duration"), extra))
    verdict = "FAIL" if total_violations > 0 else "PASS"
    lines += ["", "**Verdict: %s** (%d violation(s) across %d artifact(s))." % (
        verdict, total_violations, len(artifacts)), ""]
    lines += ["Full replay commands: `docs/RESULTS.md` §7b; the mutation score "
              "of the core is defined in `docs/MUTATION-SCORE.md`.",
              ""]
    return "\n".join(lines), verdict


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--results-dir", default="results")
    ap.add_argument("--out", default="results/report.md")
    ap.add_argument("--run-url", default="(manual)")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", "relais"))
    args = ap.parse_args(argv)

    artifacts = load(args.results_dir)
    markdown, verdict = render(artifacts, args.run_url, args.repo)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(markdown + "\n")
    with open("results/verdict", "w", encoding="utf-8") as fh:
        fh.write(verdict + "\n")
    print(markdown)
    print("VERDICT=%s (%d artifacts)" % (verdict, len(artifacts)))
    return 0


if __name__ == "__main__":
    sys.exit(main())