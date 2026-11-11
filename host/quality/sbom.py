#!/usr/bin/env python3
"""CycloneDX 1.4 SBOM for the Relais repository + anti-dependency guard.

The honest particularity of this project is that it has ZERO external runtime
dependency: core/ is a pure C11 library (no clock, no file, no socket, no
allocation) and every host/target tool uses only the C standard library, the
POSIX surface and the platform headers of the QNX toolchain.  The SBOM
therefore lists the *internal* components and the toolchain, and a guard fails
cleanly if any system include outside that surface is introduced (e.g.
<openssl/ssl.h>).

Usage: python3 host/quality/sbom.py [--out sbom.json]
Exit codes: 0 ok, 1 dependency guard failed, 2 usage/other error.
"""

import glob
import json
import os
import re
import subprocess
import sys

SPEC_VERSION = "1.4"
SERIAL = "urn:uuid:relais-sbom-00000000-0000-0000-0000-000000000000"
VERSION = "1.0.0"

# C11 standard headers (libc/libm) + the POSIX/QNX surface actually used here.
# Anything included in angle brackets outside this set is an external
# dependency and fails the guard.
C_STD = {
    "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h",
    "inttypes.h", "iso646.h", "limits.h", "locale.h", "math.h", "setjmp.h",
    "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h", "stdbool.h",
    "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "stdnoreturn.h",
    "string.h", "tgmath.h", "threads.h", "time.h", "uchar.h", "wchar.h",
    "wctype.h",
}
POSIX_QNX = {
    "arpa/inet.h", "fcntl.h", "netdb.h", "netinet/in.h", "poll.h", "unistd.h",
    "sys/mman.h", "sys/neutrino.h", "sys/slog2.h", "sys/socket.h",
    "sys/stat.h", "sys/time.h", "sys/trace.h", "sys/types.h", "sys/ioctl.h",
    "sys/select.h", "sys/wait.h", "sys/resource.h", "sys/utsname.h",
}
ALLOWED_SYSTEM = C_STD | POSIX_QNX

INCLUDE_RE = re.compile(r"^\s*#\s*include\s*<([^>]+)>\s*(//.*)?$")

# Internal components: (directory, type, description).
COMPONENTS = [
    ("core", "library", "Core protocol library (frames, arbiter, state "
     "machines, invariants, control law, plant, journal) — pure C11."),
    ("host", "application", "Host-side tools (simulator, explorer, verifier, "
     "bench, report, mutants, quality)."),
    ("target", "application", "QNX 8.0 target layer (nodes and actuator)."),
    ("actuator", "application", "Actuator process (the arbiter and the clock)."),
    ("docs", "documentation", "Specification and proof documents."),
    ("scenarios", "documentation", "Fault-scenario data files (JSON)."),
]


def source_files(root):
    """All C sources/headers under root, excluding build artifacts and .git."""
    paths = []
    for ext in ("*.c", "*.h"):
        paths.extend(glob.glob(os.path.join(root, "**", ext), recursive=True))
    return sorted(p for p in paths
                  if "/.git/" not in p and "/build/" not in p)


def check_no_external_includes(root):
    """Return [(path, header)] for any include outside libc/libm/POSIX/QNX."""
    violations = []
    for path in source_files(root):
        try:
            with open(path, encoding="utf-8") as handle:
                lines = handle.readlines()
        except OSError:
            continue
        for line in lines:
            match = INCLUDE_RE.match(line)
            if match and match.group(1) not in ALLOWED_SYSTEM:
                violations.append((os.path.relpath(path, root),
                                   match.group(1)))
    return violations


def toolchain_version(argv):
    try:
        proc = subprocess.run(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, text=True)
        return proc.stdout.splitlines()[0]
    except (OSError, IndexError):
        return "unknown"


def generate(root):
    """Return the SBOM as a dict (CycloneDX 1.4)."""
    components = []
    for directory, kind, description in COMPONENTS:
        short = directory.rstrip("/")
        components.append({
            "type": kind,
            "name": "relais-" + short,
            "bom-ref": "relais-" + short,
            "version": VERSION,
            "description": description,
            "properties": [{"name": "external-dependencies",
                            "value": "none"}],
        })
    tool_entries = [{"vendor": "ZeroClaw", "name": "sbom.py", "version": VERSION}]
    for argv in (["gcc", "--version"], ["make", "--version"],
                 ["python3", "--version"], ["ar", "--version"]):
        name = argv[0]
        tool_entries.append({"vendor": "GNU" if argv[0] in ("gcc", "make", "ar")
                             else "Python", "name": name,
                             "version": toolchain_version(argv)})
    return {
        "bomFormat": "CycloneDX",
        "specVersion": SPEC_VERSION,
        "serialNumber": SERIAL,
        "version": 1,
        "metadata": {
            "tools": tool_entries,
            "component": {"type": "application", "name": "relais",
                          "bom-ref": "relais-root", "version": VERSION},
            "properties": [{"name": "external-dependencies",
                            "value": "none — see components"}],
        },
        "components": components,
    }


def main():
    out = None
    argv = sys.argv[1:]
    if len(argv) == 2 and argv[0] == "--out":
        out = argv[1]
    elif argv:
        print("usage: python3 host/quality/sbom.py [--out sbom.json]",
              file=sys.stderr)
        return 2

    root = os.environ.get("RELAIS_ROOT", ".")

    violations = check_no_external_includes(root)
    if violations:
        for path, header in violations:
            print("::error::external include introduced: %s -> <%s>"
                  % (path, header), file=sys.stderr)
        print("anti-dependency guard FAILED: %d external include(s)"
              % len(violations), file=sys.stderr)
        return 1

    bom = generate(root)
    text = json.dumps(bom, indent=2, sort_keys=True) + "\n"
    if out:
        with open(out, "w", encoding="utf-8") as handle:
            handle.write(text)
        print("wrote %s (%d internal components)" % (out, len(bom["components"])))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())