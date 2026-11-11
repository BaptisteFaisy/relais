#!/usr/bin/env python3
"""Couche 0 anti-régression — périmètre déclaré + motifs interdits.

Deux contrôles seulement, bibliothèque standard uniquement.

  scope      tout fichier modifié doit être déclaré dans la section `## Scope`
             du corps de la PR — et toute déclaration doit servir.
  forbidden  motifs refusés dans les lignes AJOUTÉES du diff, quel que soit le
             périmètre déclaré ; les fichiers de garde sont gelés.

Usage : check_couche0.py check --pr N [--repo owner/name] [--control scope|forbidden]
Codes de sortie : 0 = OK, 1 = contrôle(s) en échec, 2 = erreur interne.
"""

import argparse
import fnmatch
import json
import os
import re
import sys
import urllib.request

API = os.environ.get("GITHUB_API_URL", "https://api.github.com")
MAX_PAGES = 20
MAX_ERREURS_AFFICHEES = 12

# Fichiers de garde : les modifier exige une mention explicite dans le corps.
GUARD_FILES = (".github/ci-budget/", ".github/couche0/",
               ".github/workflows/couche0.yml",
               # Fermé le 26/10 : sans ça une PR pouvait retirer les tests du job
               # `ci` sans le déclarer — la porte ouverte derrière la garde.
               # Même motif le 29/10 pour le natif hors PR et la file de merge :
               # vider `lint`/`apk-release` ou l'auto-merge passerait inaperçu.
               # (`ci-budget.yml` retiré le 31/10 : le garde-fou est un job de
               # `ci.yml` depuis le 29/10, le fichier n'existe plus.)
               ".github/workflows/ci.yml", ".github/workflows/ci-native.yml",
               ".github/workflows/pr-auto-merge.yml")
GUARD_FLAG = "SAMVISION_ALLOW_GUARD_BUMP"

# Exemptés de déclaration de périmètre : régénérable ou non signé.
EXEMPT_SUBSTR = ("/build/", "gradle/wrapper/", ".gradle/")
EXEMPT_SUFFIX = (".lock", ".png", ".webp", ".jpg", ".xml.bak")

# Troisième élément : sous-chaîne qui exempte la ligne (faux positif documenté).
FORBIDDEN_PATTERNS = [
    (r"-X\s+(ours|theirs)\b", "prise de côté forcée (-X ours/theirs)", None),
    (r"--force(?!-with-lease)\b", "push forcé sans --force-with-lease",
     "modal secret create"),  # faux positif : écrasement idempotent du secret Modal
    (r"pull_request_target", "déclencheur à privilèges pull_request_target", None),
    (r"sk-[A-Za-z0-9_\-]{24,}", "clé API sk-… en clair", None),
    (r"sk_[a-f0-9]{32}", "clé API sk_… en clair", None),
    (r"ghp_[A-Za-z0-9]{36}", "jeton GitHub ghp_… en clair", None),
    (r"^sdk\.dir=", "sdk.dir en dur (local.properties ne se versionne pas)", None),
    (r"2026-10-2[0-5]\b|\b2[0-5]/10\b",
     "date antérieure au 26/10 19 h (citer « relevé antérieur, hors de cet arbre »)", None),
]


def api_get(path, token):
    """GET sur l'API GitHub, retourne du JSON décodé."""
    req = urllib.request.Request(API + path)
    req.add_header("Authorization", "bearer " + token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "couche0")
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def pr_context(repo, number, token):
    """Récupère (corps de la PR, liste des fichiers modifiés)."""
    pr = api_get("/repos/%s/pulls/%s" % (repo, number), token)
    files, page = [], 1
    while page <= MAX_PAGES:
        chunk = api_get("/repos/%s/pulls/%s/files?per_page=100&page=%d"
                        % (repo, number, page), token)
        files.extend(chunk)
        if len(chunk) < 100:
            break
        page += 1
    return (pr.get("body") or ""), files


def parse_scope(body):
    r"""Motifs déclarés dans `## Scope` (puces `- \`motif\` — raison`)."""
    patterns, inside = [], False
    for line in body.splitlines():
        if line.strip().lower().startswith("## scope"):
            inside = True
            continue
        if inside and line.strip().startswith("## "):
            break
        if not inside:
            continue
        match = re.match(r"^\s*[-*]\s+`?([^`\s]+)`?", line)
        if match:
            patterns.append(match.group(1).strip().strip("`"))
    return patterns


def matches(pattern, path):
    """Un motif désigne un fichier : exact, dossier (préfixe), ou glob."""
    if pattern == path:
        return True
    if pattern.endswith("/"):
        return path.startswith(pattern) or ("/" + pattern) in ("/" + path)
    if not re.search(r"[*?\[]", pattern):
        return path.startswith(pattern + "/")
    return fnmatch.fnmatch(path, pattern.replace("**", "*"))


def is_exempt(path):
    """Fichiers régénérables : pas besoin de les déclarer."""
    return any(s in path for s in EXEMPT_SUBSTR) or path.endswith(EXEMPT_SUFFIX)


def suggerer_scope(files):
    """Motifs prêts à coller dans `## Scope`, groupés quand le glob est évident.

    Un refus sans rien à copier coûte des minutes (aller-retour humain ou agent
    Fireworks, puis un nouveau run) : ici le garde ne s'assouplit pas, il rend
    la déclaration faisable en un copier-coller. Trois fichiers ou plus du même
    dossier et de même extension deviennent `dossier/*.ext` — `matches()` sait
    déjà le lire.
    """
    groupes, ordre = {}, []
    for f in files:
        path = f["filename"]
        if is_exempt(path):
            continue
        dossier, _, base = path.rpartition("/")
        extension = "." + base.rsplit(".", 1)[1] if "." in base.lstrip(".") else ""
        cle = (dossier, extension)
        if cle not in groupes:
            groupes[cle] = []
            ordre.append(cle)
        groupes[cle].append(path)
    motifs = []
    for cle in ordre:
        dossier, extension = cle
        membres = groupes[cle]
        if len(membres) >= 3 and dossier and extension:
            motifs.append("%s/*%s" % (dossier, extension))
        else:
            motifs.extend(membres)
    return motifs


def bloc_scope(files):
    """Le bloc markdown à coller dans le corps de la PR."""
    motifs = suggerer_scope(files)
    if not motifs:
        return ""
    lignes = ["## Scope", ""]
    lignes += ["- `%s` — à préciser" % m for m in motifs]
    return "\n".join(lignes)


def publier_suggestion(files):
    """Écrit le bloc dans le résumé du job (et dans le log) : copiable."""
    bloc = bloc_scope(files)
    if not bloc:
        return
    print("Périmètre déclarable tel quel dans le corps de la PR :")
    print(bloc)
    resume = os.environ.get("GITHUB_STEP_SUMMARY")
    if not resume:
        return
    try:
        with open(resume, "a", encoding="utf-8") as fh:
            fh.write("### Périmètre à déclarer\n\n"
                     "Ce bloc, collé dans le corps de la PR, rend `couche0` vert "
                     "(relancer le workflow sans nouveau commit suffit ensuite).\n\n"
                     "```markdown\n%s\n```\n" % bloc)
    except OSError:
        pass


def check_scope(files, body, errors):
    """Tout fichier modifié déclaré, toute déclaration servie."""
    declared = parse_scope(body)
    paths = [f["filename"] for f in files if not is_exempt(f["filename"])]
    if not declared:
        if paths:
            errors.append("scope : aucune section `## Scope` dans le corps de la "
                          "PR, alors que %d fichier(s) sont modifiés." % len(paths))
        return
    used = set()
    for path in paths:
        hit = [p for p in declared if matches(p, path)]
        if hit:
            used.update(hit)
        else:
            errors.append("scope : `%s` modifié mais non déclaré dans `## Scope`." % path)
    unused = [p for p in declared if p not in used]
    if unused:
        errors.append("scope : déclaration(s) inutilisée(s) — périmètre surdimensionné : %s"
                      % ", ".join("`%s`" % p for p in unused))


def added_lines(files):
    """Lignes ajoutées du diff, par fichier (en-têtes de fichier exclus)."""
    for f in files:
        for line in (f.get("patch") or "").splitlines():
            if line.startswith("+") and not line.startswith("+++"):
                yield f["filename"], line[1:]


def check_forbidden(files, body, errors):
    """Motifs interdits dans les lignes ajoutées + fichiers de garde gelés."""
    compiled = [(re.compile(p, re.M), m, x) for p, m, x in FORBIDDEN_PATTERNS]
    for path, line in added_lines(files):
        # Le garde se cite lui-même (motifs, exemples) : il est déjà gelé
        # plus bas (GUARD_FLAG), donc hors du scan de motifs.
        if path.startswith(".github/couche0/"):
            continue
        for rx, message, exempt in compiled:
            if exempt and exempt in line:
                continue
            if rx.search(line):
                errors.append("forbidden : %s — `%s` dans %s" % (message, line.strip()[:90], path))
    frozen = [f["filename"] for f in files if f["filename"].startswith(GUARD_FILES)]
    if frozen and GUARD_FLAG not in body:
        errors.append("forbidden : fichier(s) de garde modifié(s) sans la mention `%s` "
                      "dans le corps : %s" % (GUARD_FLAG, ", ".join(frozen)))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="check_couche0")
    ap.add_argument("cmd", choices=["check"])
    ap.add_argument("--pr", required=True, type=int)
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    ap.add_argument("--control", choices=["scope", "forbidden"], default=None)
    args = ap.parse_args(argv)
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not args.repo or not token:
        print("::error::GITHUB_REPOSITORY et GITHUB_TOKEN sont requis")
        return 2
    try:
        body, files = pr_context(args.repo, args.pr, token)
    except Exception as exc:                         # noqa: BLE001
        print("::error::API GitHub : %s" % exc)
        return 2
    errors = []
    if args.control in (None, "scope"):
        check_scope(files, body, errors)
        if any(e.startswith("scope :") for e in errors):
            publier_suggestion(files)
    if args.control in (None, "forbidden"):
        check_forbidden(files, body, errors)
    # Un refus peut lister 38 fichiers : le log devient illisible et noie la
    # cause. Les douze premiers suffisent à comprendre ; le résumé du job
    # porte le bloc complet à coller (publier_suggestion).
    for message in errors[:MAX_ERREURS_AFFICHEES]:
        print("::error::%s" % message)
    if len(errors) > MAX_ERREURS_AFFICHEES:
        print("::error::… et %d autre(s) — bloc `## Scope` complet dans le "
              "résumé du job." % (len(errors) - MAX_ERREURS_AFFICHEES))
    print("%s — %d fichier(s) controlé(s), %d erreur(s)"
          % ("OK" if not errors else "REFUSÉ", len(files), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
