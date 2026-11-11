#!/usr/bin/env python3
"""Garde-fou « budget CI » : tout job GitHub Actions doit être borné à 5 min.

Règle user 2026-10-24. Bibliothèque standard uniquement (les runners
self-hosted n'ont pas PyYAML garanti). Analyse volontairement simple :
on lit les workflows ligne à ligne, on repère la section `jobs:` et, pour
chaque job (clé à 2 espaces), le `timeout-minutes:` à 4 espaces.

Erreurs (exit 1) : job sans `timeout-minutes`, ou timeout > MAX_MINUTES
sur un workflow qui tourne sur pull_request. Les workflows hors PR (push sur
la branche protégée, tags, déclenchement manuel) peuvent dépasser, avec un
timeout explicite et un commentaire qui justifie le chiffre.
Avertissements (exit 0) : `concurrency` absent, checkout sans fetch-depth: 1.
"""

import glob
import os
import re
import sys

MAX_MINUTES = 5
WORKFLOWS = ".github/workflows"
SKIP_FILES = set()  # le garde-fou se contrôle lui-même

TOP_KEY = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*:")
JOB_KEY = re.compile(r"^  ([A-Za-z0-9_.-]+):\s*(#.*)?$")
TIMEOUT = re.compile(r"^    timeout-minutes:\s*(\d+)\s*(#.*)?$")


def read_lines(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read().splitlines()


def scan_jobs(lines):
    """Retourne [(nom, timeout|None)] pour la section jobs: du fichier."""
    jobs = []
    in_jobs = False
    for line in lines:
        if line.startswith("jobs:"):
            in_jobs = True
            continue
        if in_jobs and TOP_KEY.match(line):
            in_jobs = False
        if not in_jobs:
            continue
        match = JOB_KEY.match(line)
        if match:
            jobs.append([match.group(1), None])
            continue
        match = TIMEOUT.match(line)
        if match and jobs:
            jobs[-1][1] = int(match.group(1))
    return jobs


def runs_on_pr(lines):
    """Vrai si le workflow se déclenche sur pull_request (donc soumis au budget)."""
    for line in lines:
        if line.startswith("jobs:"):
            break
        code = line.split("#", 1)[0]
        if re.search(r"\bpull_request\b", code):
            return True
    return False


def check_file(path, errors, warnings, display=None):
    rel = display if display is not None else path
    lines = read_lines(path)
    jobs = scan_jobs(lines)
    if not jobs:
        warnings.append("%s: aucun job détecté (workflow vide ?)" % rel)
        return
    on_pr = runs_on_pr(lines)
    for name, timeout in jobs:
        if timeout is None:
            errors.append(
                "%s: job '%s' sans timeout-minutes (défaut 360 min, interdit)"
                % (rel, name))
        elif timeout > MAX_MINUTES and on_pr:
            errors.append(
                "%s: job '%s' timeout-minutes: %d > %d (budget CI)"
                % (rel, name, timeout, MAX_MINUTES))
    if not any(line.startswith("concurrency:") for line in lines):
        warnings.append("%s: pas de concurrency (annulation des runs obsolètes)"
                        % rel)
    if "actions/checkout" in "\n".join(lines) and "fetch-depth: 1" not in "\n".join(lines):
        warnings.append("%s: checkout sans fetch-depth: 1" % rel)


def main():
    root = os.environ.get("CI_BUDGET_ROOT", ".")
    paths = sorted(glob.glob(os.path.join(root, WORKFLOWS, "*.yml")))
    paths += sorted(glob.glob(os.path.join(root, WORKFLOWS, "*.yaml")))
    paths = [p for p in paths if os.path.basename(p) not in SKIP_FILES]

    errors, warnings = [], []
    if not paths:
        print("::warning::Aucun workflow dans %s" % WORKFLOWS)
    for path in paths:
        # On lit le chemin réel (CI_BUDGET_ROOT peut viser ailleurs que le
        # répertoire courant) mais on affiche le chemin relatif, stable.
        check_file(path, errors, warnings,
                   display=os.path.relpath(path, root))

    for message in warnings:
        print("::warning::%s" % message)
    for message in errors:
        print("::error::%s" % message)

    print("%d workflow(s) contrôlé(s) : %d erreur(s), %d avertissement(s)"
          % (len(paths), len(errors), len(warnings)))
    if errors:
        print("Règle : CI ≤ %d min par job — voir ~/Bureau/CI-BUDGET-5MIN.md"
              % MAX_MINUTES)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())