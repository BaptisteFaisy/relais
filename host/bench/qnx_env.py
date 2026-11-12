#!/usr/bin/env python3
"""Detection of the QNX 8.0 build/run environment, with a readable report.

Mission M01: turn the move to a real QNX target into a receipt run, not a
discovery. This module answers one question -- "can this host source the SDP,
cross-compile the target, build an image and boot two guests?" -- so that both
a human and `qnx_build.sh` can act on the answer instead of stumbling over a
stack trace.

Checks performed (each with a status, a detail and an actionable fix):
  * is the SDP present?  (`qnxsdp-env.sh`, QNX-01..08, PERF-*)
  * is `qcc -V gcc_ntox86_64` available?  (target/Makefile, SEC-05)
  * is `mkqnximage` available?  (image build, QNX-07/QNX-08)
  * is QEMU >= 8.2 installed?  (host/bench/QEMU.md)
  * can the current user access KVM?  (advisory: TCG fallback exists)

Return codes, matching the mission contract:
  0  READY    -- everything needed to build *and* run the receipt.
  1  PARTIAL  -- the SDP was found but a tool is missing/too old, or QEMU is.
  2  ABSENT   -- no QNX SDP on this host (nothing else matters).

Standard library only (rule 6). The KVM and QEMU probes are reused from
`qemu_config.py` (rule 2: zero duplication); only the SDP probes are new.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import sys
from dataclasses import asdict, dataclass, field
from typing import Optional

try:  # package or flat layout, like the rest of host/bench
    from .qemu_config import KVM_PATH, QEMU_BINARY, detect_kvm, qemu_version
except ImportError:  # pragma: no cover - flat layout
    from qemu_config import KVM_PATH, QEMU_BINARY, detect_kvm, qemu_version

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

SDP_ENV_SCRIPT = "qnxsdp-env.sh"
ENV_SDP_VARS = ("RELAIS_QNX_SDP", "QNX_SDP")
HOST_BIN = os.path.join("host", "linux", "x86_64", "usr", "bin")
MIN_QEMU = (8, 2)
DEFAULT_ROOTS = (os.path.expanduser("~"), "/opt")

# Human-friendly labels for the report.
OK_LABEL, MISS_LABEL, WARN_LABEL = "OK", "MISSING", "WARN"
ABSENT_FIX = (
    "The QNX SDP 8.0 is not on this host. Get it for free through QNX "
    "Everywhere (https://qnx.com/everywhere, needs a myQNX account), install "
    "it (e.g. ~/qnx800), or point RELAIS_QNX_SDP at the directory holding "
    "qnxsdp-env.sh. Until then, `make -C target syntax` and `--mode pc` / "
    "`--mode sim` still work: the SDP is only required for a real QNX boot.")


@dataclass
class Check:
    """One named probe result. `ok is None` marks an advisory (non-blocking)."""
    name: str
    label: str
    ok: Optional[bool]
    detail: str = ""
    fix: str = ""

    def status(self) -> str:
        if self.ok is None:
            return WARN_LABEL
        return OK_LABEL if self.ok else MISS_LABEL

    def as_dict(self) -> dict:
        out = asdict(self)
        out["status"] = self.status()
        return out


@dataclass
class Report:
    """The aggregation a caller reads: verdict + label + checks + fixes."""
    verdict: int = 2
    label: str = "ABSENT"
    sdp: Optional[str] = None
    checks: list = field(default_factory=list)
    fixes: list = field(default_factory=list)

    def as_dict(self) -> dict:
        return {"verdict": self.verdict, "label": self.label,
                "sdp": self.sdp,
                "checks": [c.as_dict() for c in self.checks],
                "fixes": list(self.fixes)}


def parse_version(text: str) -> tuple:
    """Parse `qemu-system-x86_64 --version` into a (major, minor) tuple."""
    for token in text.split():
        if token.count(".") >= 1:
            head = token.rstrip(",")
            parts = head.split(".")
            if len(parts) >= 2 and all(p.isdigit() for p in parts[:2]):
                return tuple(int(p) for p in parts[:2])
    return (0, 0)


def _is_sdp(path: str) -> bool:
    """A directory is an SDP root when it holds the environment script."""
    return bool(path) and os.path.isdir(path) and \
        os.path.exists(os.path.join(path, SDP_ENV_SCRIPT))


def _env_sdp(environ) -> Optional[str]:
    """The SDP root from the environment, if set and valid."""
    for var in ENV_SDP_VARS:
        value = environ.get(var)
        if value and _is_sdp(value):
            return value
    return None


def _glob_sdp(roots) -> Optional[str]:
    """First valid SDP root found under the candidate search roots."""
    seen = []
    for root in roots:
        if not root:
            continue
        for pattern in (os.path.join(root, "qnx800"),
                        os.path.join(root, "qnx*")):
            for candidate in sorted(glob.glob(pattern)):
                if candidate not in seen:
                    seen.append(candidate)
                else:
                    continue
                if _is_sdp(candidate):
                    return candidate
    return None


def find_sdp(search_roots=(), environ=None) -> Optional[str]:
    """Locate the SDP: environment variables first, then the search roots."""
    env = environ if environ is not None else os.environ
    return _env_sdp(env) or _glob_sdp(search_roots)


def _host_bins(sdp: str) -> list:
    """Every host tool directory of the SDP, in lookup order.

    SDP 8.0 splits the host tools: qcc, mkifs and qlicense live in
    host/linux/x86_64/usr/bin, while mkqnximage lives in host/common/bin.
    Probing only the first one reports a missing mkqnximage on a complete
    install -- measured against a real SDP 8.0.0 installation.
    """
    dirs = []
    direct = os.path.join(sdp, HOST_BIN)
    if os.path.isdir(direct):
        dirs.append(direct)
    for pattern in (os.path.join(sdp, "host", "*", "*", "usr", "bin"),
                    os.path.join(sdp, "host", "*", "bin")):
        for found in sorted(glob.glob(pattern)):
            if os.path.isdir(found) and found not in dirs:
                dirs.append(found)
    return dirs


def _tool(tools_dirs, name: str) -> Optional[str]:
    """Executable path of `name` in the first SDP host directory holding it."""
    for tools_dir in tools_dirs or []:
        candidate = os.path.join(tools_dir, name)
        if os.access(candidate, os.X_OK):
            return candidate
    return None


def sdp_checks(sdp: str) -> list:
    """All SDP-side probes for a known SDP root (never raises)."""
    checks = []
    env_script = os.path.join(sdp, SDP_ENV_SCRIPT)
    checks.append(Check("env-script", "qnxsdp-env.sh present",
                        os.path.exists(env_script), env_script,
                        "Re-run the installer; the script must sit at the SDP "
                        "root."))
    tools = _host_bins(sdp)
    qcc = _tool(tools, "qcc")
    checks.append(Check("qcc", "qcc -V gcc_ntox86_64 usable", qcc is not None,
                        qcc or "qcc not found",
                        "Install the x86_64 host tools of the SDP, then run: "
                        "source %s && qcc -V gcc_ntox86_64 -dumpversion"
                        % env_script))
    mkqnx = _tool(tools, "mkqnximage")
    checks.append(Check("mkqnximage", "mkqnximage available",
                        mkqnx is not None,
                        mkqnx or "mkqnximage not found",
                        "Install the SDP host tools; mkqnximage ships with the "
                        "image utilities."))
    # SDP 7.1 names the sysroot x86_64le; SDP 8.0 names it x86_64. Both hold
    # the gcc_ntox86_64 target libraries.
    target = (glob.glob(os.path.join(sdp, "target", "*", "x86_64le"))
              + glob.glob(os.path.join(sdp, "target", "*", "x86_64")))
    checks.append(Check("target", "x86_64 sysroot present", None,
                        target[0] if target else "no target/*/x86_64(le) tree",
                        "The gcc_ntox86_64 sysroot is part of the SDP core "
                        "install."))
    return checks


def qemu_check(version_text: Optional[str] = None) -> Check:
    """QEMU presence and minimum version (8.2, per host/bench/QEMU.md)."""
    if version_text is None:
        version_text = qemu_version(QEMU_BINARY)
    if not version_text:
        return Check("qemu", "qemu-system-x86_64 present", False,
                     "qemu-system-x86_64 not found",
                     "Install it: apt install qemu-system-x86 (or equivalent).")
    version = parse_version(version_text)
    ok = version >= MIN_QEMU
    return Check("qemu", "QEMU >= 8.2 (was %s)" % ".".join(map(str, version)),
                 ok, version_text,
                 "QEMU %s is too old for the bench; install >= 8.2."
                 % ".".join(map(str, version)) if not ok else "")


def kvm_check(info: Optional[dict] = None) -> Check:
    """KVM accessibility (advisory: TCG fallback is documented in QEMU.md)."""
    if info is None:
        info = detect_kvm(KVM_PATH)
    available = bool(info.get("available"))
    return Check("kvm", "KVM usable by this user", None,
                 "%s (%s)" % (info.get("accel", "?"), info.get("reason", "")),
                 "Without KVM the bench falls back to TCG: build works, "
                 "E2/E5 timings are not usable.")


def detect(search_roots=(), environ=None,
           version_text: Optional[str] = None,
           kvm_info: Optional[dict] = None) -> Report:
    """Run every probe and aggregate the verdict (0 ready / 1 partial / 2 absent).

    The bookkeeping values (`version_text`, `kvm_info`) are injectable so the
    tests stay deterministic on any host; leave them None in normal use.
    """
    roots = search_roots or DEFAULT_ROOTS
    env = environ if environ is not None else os.environ
    report = Report()
    report.sdp = find_sdp(roots, environ=env)
    if report.sdp is None:
        report.verdict = 2
        report.label = "ABSENT"
        report.checks.append(Check("sdp", "QNX SDP found", False,
                                   "no " + SDP_ENV_SCRIPT,
                                   ABSENT_FIX))
        report.fixes.append(ABSENT_FIX)
        return report
    report.checks = sdp_checks(report.sdp)
    report.checks.append(qemu_check(version_text))
    report.checks.append(kvm_check(kvm_info))
    report.verdict = _verdict(report.checks)
    report.label = _label(report.verdict)
    for check in report.checks:
        if check.ok is False and check.fix:
            report.fixes.append(check.fix)
    return report


def _verdict(checks) -> int:
    """0 when nothing blocking is missing, else 1 (SDP absence handled before)."""
    for check in checks:
        if check.ok is False and check.name != "kvm":
            return 1
    return 0


def _label(verdict: int) -> str:
    return {0: "READY", 1: "PARTIAL", 2: "ABSENT"}[verdict]


def format_report(report: Report) -> str:
    """The readable, single-glance report printed to a human."""
    lines = ["QNX 8.0 environment report",
             "  verdict : %d (%s)" % (report.verdict, report.label),
             "  sdp     : %s" % (report.sdp or "absent")]
    for check in report.checks:
        lines.append("  [%s] %s" % (check.status(), check.label))
        if check.detail:
            lines.append("        %s" % check.detail)
        if check.fix and check.ok is not True:
            lines.append("        fix: %s" % check.fix)
    if report.fixes:
        lines.append("  next:")
        for fix in report.fixes:
            lines.append("        - %s" % fix)
    return "\n".join(lines)


def _build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="Detect the QNX 8.0 environment; exit 0/1/2 (ready/partial/"
                    "absent).")
    ap.add_argument("--json", action="store_true",
                    help="print the report as one JSON object")
    ap.add_argument("--root", action="append", default=[],
                    help="extra/replacement search root (repeatable)")
    ap.add_argument("--print", choices=("sdp", "env-script", "qcc",
                                        "mkqnximage", "verdict"),
                    help="print a single value for shell scripts and exit")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress output when using --print")
    return ap


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)
    roots = args.root if args.root else DEFAULT_ROOTS
    environ = {} if args.root else os.environ  # --root replaces the defaults
    report = detect(roots, environ=environ)
    if args.print:
        return _print_value(report, args.print, args.quiet)
    if args.json:
        print(json.dumps(report.as_dict(), indent=2))
    else:
        print(format_report(report))
    return report.verdict


def _print_resolver(report: Report, key: str) -> Optional[str]:
    """Resolve one --print key to a path (or None), never raising."""
    if not report.sdp:
        return None
    if key == "sdp":
        return report.sdp
    if key == "env-script":
        script = os.path.join(report.sdp, SDP_ENV_SCRIPT)
        return script if os.path.exists(script) else None
    tool = "qcc" if key == "qcc" else "mkqnximage"
    return _tool(_host_bins(report.sdp), tool)


def _print_value(report: Report, key: str, quiet: bool) -> int:
    """Resolve one --print key; exit != 0 when the requested value is absent."""
    if key == "verdict":
        if not quiet:
            print(report.verdict)
        return report.verdict
    path = _print_resolver(report, key)
    if path:
        if not quiet:
            print(path)
        return 0
    if not quiet:
        print("", file=sys.stderr)
    return 1 if report.verdict != 2 else 2


if __name__ == "__main__":
    sys.exit(main())