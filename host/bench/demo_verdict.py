#!/usr/bin/env python3
"""How many invariant violations did the last demo run report?

    python3 host/bench/demo_verdict.py [fichier]

Reads docs/evidence/qnx-demo-qnx-demo.json by default. The count comes from
the `verdict` field ONLY: that file also holds `verdict_disturbed_channel`,
whose verdict stays clean even when the run is not — reading the wrong field
would call a dirty run clean.
"""
import json
import pathlib
import re
import sys

DEFAULT = "docs/evidence/qnx-demo-qnx-demo.json"


def main(argv=None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    path = pathlib.Path(args[0] if args else DEFAULT)
    try:
        demo = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        print("?")
        return 0
    verdict = demo.get("verdict", "") if isinstance(demo, dict) else ""
    verdict = verdict if isinstance(verdict, str) else ""
    found = re.search(r"violations=(\d+)", verdict)
    print(found.group(1) if found else "?")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
