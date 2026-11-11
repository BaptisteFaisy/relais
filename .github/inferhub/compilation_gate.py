#!/usr/bin/env python3
"""Porte de compilation de l'agent de conflit (pr-auto-merge).

Une résolution de conflit **ne doit jamais atteindre la branche sans compiler**.
Motif mesuré le 31/10 (PR #615) : la réconciliation avait poussé une tête
cassée — paramètre `motion` manquant, variable `hushed` hors portée, un test
aux arguments inversés —, le balayeur a classé l'échec « réel » (« aucun agent
n'écrit de correctif de code »), et la PR est morte en silence jusqu'à une
reprise humaine.

Ici : on compile les modules touchés (`:core:compileDebugKotlin`,
`:app:compileDebugKotlin`, `py_compile` pour le serveur) AVANT le push. Rouge
→ rien n'est poussé, la branche reste intacte, la PR reste `DIRTY` (le tour
suivant rappelle l'agent, dans la limite du quota) et le compte rendu cite les
erreurs du compilateur.

Une porte qui ne peut pas s'exécuter (pas de `gradlew`, pas de SDK Android,
délai dépassé) ne bloque rien et le dit : le bail de la CI reste la preuve.
Coupe-circuit : `CONFLICT_COMPILE_GATE=0`.

Bibliothèque standard uniquement, aucune horloge implicite, aucun secret.
"""

import os
import subprocess
import sys

# Un `.kt` hors de ces arbres ne se compile pas seul (`settings.gradle.kts`).
MODULES_GRADLE = ("core", "app")
GATE_ENV = "CONFLICT_COMPILE_GATE"
GATE_TIMEOUT_S = 150
GATE_EXCERPT = 1_500
# Marqueur de la tête posée par l'agent : le balayeur le cherche mot pour mot
# pour attribuer un échec à la résolution (label `intervention-humaine`) au
# lieu de laisser la PR mourir en silence.
MARK_AGENT = "<!-- agent-conflit sha=%s -->"


def _log(message):
    print("inferhub-agent: %s" % message, flush=True)


def fichiers_modifies(base="origin/main"):
    """Fichiers que la branche change par rapport à `base` (best-effort)."""
    try:
        proc = subprocess.run(["git", "diff", "--name-only", "%s...HEAD" % base],
                              capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        _log("diff illisible, rien à compiler : %s" % exc)
        return []
    if proc.returncode != 0:
        _log("diff illisible (%s), rien à compiler" % base)
        return []
    return [line for line in (proc.stdout or "").splitlines() if line.strip()]


def taches_compilation(fichiers):
    """(tâches Gradle, fichiers Python) à vérifier pour ces chemins.

    Un `.kt` sous `core/` ou `app/` demande la compilation de son module ; un
    `.py` se vérifie seul (`py_compile`). Tout le reste (docs, JSON, `.kts`,
    arbres non Gradle) ne prouve rien et n'est pas compilé ici.
    """
    gradle, python = [], []
    for path in fichiers:
        if path.endswith(".py"):
            python.append(path)
            continue
        if not path.endswith(".kt") or "/" not in path:
            continue
        module = path.split("/", 1)[0]
        tache = ":%s:compileDebugKotlin" % module
        if module in MODULES_GRADLE and tache not in gradle:
            gradle.append(tache)
    return gradle, python


def sdk_android():
    """SDK Android, ou `None` : sans lui Gradle refuse de se configurer."""
    for var in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        chemin = os.environ.get(var)
        if chemin and os.path.isdir(chemin):
            return chemin
    maison = os.path.join(os.path.expanduser("~"), "Android", "Sdk")
    return maison if os.path.isdir(maison) else None


def ecrire_local_properties(sdk):
    """`local.properties` (gitignoré) : le clone du balayeur n'en a pas."""
    try:
        with open("local.properties", "w", encoding="utf-8") as fichier:
            fichier.write("sdk.dir=%s\n" % sdk)
    except OSError as exc:
        _log("local.properties non écrit : %s" % exc)


def extrait_compilateur(etape, texte):
    """Extrait publiable : les lignes d'erreur, sinon la fin du journal."""
    lignes = [ligne.strip() for ligne in texte.splitlines() if ligne.strip()]
    fautes = [ligne for ligne in lignes
              if ligne.startswith("e: ") or "error:" in ligne
              or ligne.startswith("FAILURE:")]
    garde = fautes[:8] or lignes[-8:]
    return "`%s` :\n```\n%s\n```" % (etape, "\n".join(garde)[:GATE_EXCERPT])


def verifier_compilation(fichiers=None):
    """Compile les modules touchés avant le push. Rend (ok, message).

    `ok` faux n'est pas une panne : c'est la preuve que la résolution est
    cassée, donc le push est refusé et la branche reste intacte.
    """
    if os.environ.get(GATE_ENV, "1") == "0":
        return True, "porte de compilation désactivée (%s=0)" % GATE_ENV
    if not os.path.exists("gradlew"):
        return True, "porte de compilation sautée : pas de `gradlew` ici"
    fichiers = fichiers_modifies() if fichiers is None else fichiers
    gradle, python = taches_compilation(fichiers)
    if not gradle and not python:
        return True, "porte de compilation : aucun module touché"
    for path in python:
        try:
            proc = subprocess.run([sys.executable, "-m", "py_compile", path],
                                  capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.SubprocessError) as exc:
            return True, "porte de compilation sautée (Python) : %s" % exc
        if proc.returncode != 0:
            return False, extrait_compilateur("Python %s" % path,
                                              proc.stderr or proc.stdout)
    if not gradle:
        return True, "porte de compilation : Python OK"
    sdk = sdk_android()
    if sdk:
        ecrire_local_properties(sdk)
    commande = ["./gradlew", "--console=plain", "--quiet",
                "-Dorg.gradle.jvmargs=-Xmx2g", "--max-workers=2"] + gradle
    try:
        proc = subprocess.run(commande, capture_output=True, text=True,
                              timeout=GATE_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return True, ("porte de compilation non concluante : %d s dépassées "
                      "(%s)" % (GATE_TIMEOUT_S, " ".join(gradle)))
    except (OSError, subprocess.SubprocessError) as exc:
        return True, "porte de compilation sautée : %s" % exc
    if proc.returncode == 0:
        return True, "porte de compilation : %s OK" % " ".join(gradle)
    return False, extrait_compilateur(" ".join(gradle),
                                      (proc.stdout or "") + "\n"
                                      + (proc.stderr or ""))


def tete_git():
    """SHA de la tête (le push ne la change pas : marqueur exact)."""
    try:
        proc = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                              text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return ""
    return (proc.stdout or "").strip() if proc.returncode == 0 else ""


def prepare_push(files, branch, fichiers=None):
    """(ok, message, corps) : décide du push et prépare son compte rendu.

    Le corps porte le marqueur de tête en première ligne ; l'appelant ne le
    poste qu'après un push réussi.
    """
    ok, message = verifier_compilation(fichiers)
    if not ok:
        return False, message, ""
    unique = sorted(set(files))
    corps = ("%s\n\nConflit réconcilié par l'agent : %d fichier(s) (%s), "
             "rebase poussé sur `%s` (%s). La file reprend."
             % (MARK_AGENT % tete_git(), len(unique), ", ".join(unique),
                branch, message))
    return True, message, corps
