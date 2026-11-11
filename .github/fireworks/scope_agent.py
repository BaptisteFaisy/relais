#!/usr/bin/env python3
"""Agent de périmètre Fireworks pour la file pr-auto-merge.

Une PR dont le corps n'a pas de `## Scope`, ou dont la section ne couvre pas
tous les fichiers modifiés, est refusée par `couche0` : le balayeur la voit
`BLOCKED` et, sans ce script, la laisse pourrir — c'est exactement ce qui est
arrivé aux PR d'auteur (Gaïa, 29/10).

Ici un modèle Fireworks lit le diff et rédige une puce
``- `chemin` — raison`` par fichier, en français. Le script vérifie le corps
recomposé avec le garde de `main` lui-même (importé, jamais réécrit) avant
de l'écrire : tout fichier non déclaré et toute déclaration inutile restent
refusés. Ce n'est pas un contournement de `couche0`, c'est la déclaration
manquante qui est écrite, avec une raison tirée du diff.

Bibliothèque standard uniquement. Usage :

  FIREWORKS_API_KEY=... python3 scope_agent.py fix \\
      --repo BaptisteFaisy/relais --pr 215 --sha <tête> \\
      --guard /tmp/check_couche0.py

Codes de sortie : 0 = corps complété ; 1 = échec avec compte rendu ;
2 = rien à faire (conforme, autre contrôle rouge, déjà tenté, désactivé).
"""

import argparse
import fnmatch
import importlib.util
import json
import os
import re
import subprocess
import sys
import urllib.request

MODEL_DEFAULT = "accounts/fireworks/models/minimax-m3p1-flash"
API_DEFAULT = "https://api.fireworks.ai/inference/v1/chat/completions"
MAX_FILES = 25
MAX_EXCERPT_CHARS = 600
MAX_EXCERPT_LINES = 5
MAX_TOKENS = 3_000
TEMPERATURE = 0.1
CALL_TIMEOUT_S = 90
COMMENT_PREFIX = "_fireworks · agent de périmètre (MiniMax M3.1 Flash)_"
MARKER = "<!-- automerge-scope sha=%s -->"
LABEL_HUMAN = "intervention-humaine"
PUCE = re.compile(r"^\s*(?:[-*+]|\d+[.)])\s+`?([^`\s]+)`?"
                  r"\s*(?:[—–:-]|-\s)\s+(.+?)\s*$")


class AgentError(Exception):
    """Échec avec message publiable (sans secret)."""


class Skip(Exception):
    """Rien à faire : l'appelant saute la PR sans bruit (exit 2)."""


def log(message):
    print("scope-agent: %s" % message, flush=True)


def env_or(name, default):
    """Variable d'env ou défaut. GitHub injecte une var absente comme ""
    (et non unset) : `os.environ.get(n, d)` rendrait "" → 400 Fireworks."""
    return os.environ.get(name) or default


def sh(args, timeout=90):
    proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if proc.returncode != 0:
        raise AgentError("%s : %s" % (" ".join(args[:3]),
                                      (proc.stderr or "").strip()[:200]))
    return (proc.stdout or "").strip()


def comment(repo, pr, body):
    """Commente la PR ; best-effort, ne lève jamais."""
    try:
        subprocess.run(["gh", "pr", "comment", str(pr), "--repo", repo,
                        "--body", COMMENT_PREFIX + "\n\n" + body],
                       capture_output=True, text=True, timeout=60)
    except Exception as exc:                                   # noqa: BLE001
        log("commentaire impossible : %s" % exc)


def add_label(repo, pr, label):
    """Pose un label ; best-effort, ne lève jamais."""
    try:
        subprocess.run(["gh", "pr", "edit", str(pr), "--repo", repo,
                        "--add-label", label],
                       capture_output=True, text=True, timeout=60)
    except Exception as exc:                                   # noqa: BLE001
        log("label %s impossible : %s" % (label, exc))


def deja_tente(repo, pr, sha):
    """Marqueur déjà posé pour cette tête ? (empreinte dans les commentaires,
    pas sur le disque : les runners changent d'un passage à l'autre)."""
    try:
        out = sh(["gh", "pr", "view", str(pr), "--repo", repo,
                  "--json", "comments", "-q", ".comments[].body"])
    except AgentError:
        return False
    return (MARKER % sha) in out


def load_guard(path):
    """Importe le garde de main, jamais une copie locale réécrite."""
    spec = importlib.util.spec_from_file_location("couche0", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def excerpt(fichier):
    """Quelques lignes ajoutées : assez pour une raison, pas tout le diff."""
    lignes = []
    for ligne in (fichier.get("patch") or "").splitlines():
        if ligne.startswith("+") and not ligne.startswith("+++"):
            lignes.append(ligne[1:])
        if len(lignes) >= MAX_EXCERPT_LINES:
            break
    texte = "\n".join(lignes)[:MAX_EXCERPT_CHARS]
    return texte or "(binaire, renommage ou vidage)"


def messages_pour(pr, fichiers, necessaires, relance=False):
    system = (
        "Tu rédiges la section `## Scope` du corps d'une pull request : elle "
        "déclare, fichier par fichier, ce que la PR change et pourquoi. "
        "Pour CHAQUE fichier listé, rends exactement une ligne de la forme\n"
        "- `chemin/exact` — raison en français, 12 mots maximum\n"
        "Règles : reprends le chemin tel quel, sans glob ni dossier ; une "
        "raison concrète tirée du diff (ce qui change, pas « modification du "
        "fichier ») ; aucune ligne pour un fichier non listé ; aucun titre, "
        "aucune phrase autour ; uniquement les puces, dans l'ordre donné.")
    if relance:
        system += (" Relance : tu as omis certains fichiers. Donne une puce "
                   "pour CHACUN, chemin complet recopié caractère par "
                   "caractère, sans astérisque, sans raccourci, sans exception.")
    blocs = ["Titre : %s" % pr["title"]]
    corps = (pr.get("body") or "").strip()
    if corps:
        blocs.append("Corps actuel (extrait) :\n" + corps[:2_500])
    par_nom = {f["filename"]: f for f in fichiers}
    for chemin in necessaires:
        blocs.append("### %s\n%s" % (chemin, excerpt(par_nom[chemin])))
    blocs.append("Rends une puce par fichier ci-dessus, rien d'autre.")
    return [{"role": "system", "content": system},
            {"role": "user", "content": "\n\n".join(blocs)}]


def fw_complete(api_url, key, model, messages):
    """Un appel chat/completions Fireworks, rend le texte réponse."""
    payload = json.dumps({"model": model, "messages": messages,
                          "temperature": TEMPERATURE,
                          "max_tokens": MAX_TOKENS}).encode("utf-8")
    req = urllib.request.Request(api_url, data=payload, method="POST")
    req.add_header("Authorization", "Bearer " + key)
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=CALL_TIMEOUT_S) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as exc:                                   # noqa: BLE001
        raise AgentError("appel Fireworks : %s" % str(exc)[:200])
    try:
        return data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        raise AgentError("réponse Fireworks illisible (pas de choices[0])")


def raisons_repondue(texte):
    """Puces du modèle → {motif: raison}, sans les blocs de code vides."""
    balise = "think"                       # raisonnement interne éventuel
    texte = re.sub(r"<%s>.*?</%s>" % (balise, balise), "", texte, flags=re.S)
    raisons = {}
    for ligne in texte.splitlines():
        match = PUCE.match(ligne)
        if not match:
            continue
        chemin = match.group(1).strip().strip("`").strip()
        raison = match.group(2).strip().strip("`").strip()
        if chemin.startswith("./"):
            chemin = chemin[2:]
        if chemin and raison:
            raisons[chemin] = raison
    return raisons


def designe(motif, chemin, garde):
    """Ce motif désigne-t-il ce fichier ? (règle du garde, puis souplesse)

    Le modèle rend un glob, un dossier, ou recopie le chemin avec une casse
    approximative (`sam3Vision` pour `sam3vision`, vu sur #215 le 29/10). On
    cherche le chemin exact, la règle du garde, puis l'égalité sans casse : le
    fichier déclaré reste, lui, toujours écrit avec son vrai chemin.
    """
    if motif == chemin or garde.matches(motif, chemin):
        return True
    bas = motif.lower()
    if bas == chemin.lower():
        return True
    if re.search(r"[*?\[]", bas):
        return fnmatch.fnmatch(chemin.lower(), bas.replace("**", "*"))
    return chemin.lower().startswith(bas.rstrip("/") + "/")


def rapprocher(raisons, necessaires, garde):
    """Associe les puces rendues aux fichiers à déclarer.

    Une puce juste ne doit jamais être comptée comme absente : c'est ce qui
    laissait les PR bloquées sur « le modèle n'a rien rendu » (29/10, #215).
    Rend (couverts {chemin: raison}, manquants).
    """
    couverts = {}
    for chemin in necessaires:
        for motif, raison in raisons.items():
            if designe(motif, chemin, garde):
                couverts[chemin] = raison
                break
    manquants = [chemin for chemin in necessaires if chemin not in couverts]
    return couverts, manquants


def motif_declare(ligne):
    """Motif qu'un garde lit comme une déclaration dans `## Scope`.

    Même expression que `parse_scope` : un garde lit `- `Parler`` même sans
    raison, alors que `PUCE` (qui exige une raison) le prenait pour de la
    prose. Ces lignes-là ne doivent pas survivre si elles ne servent aucun
    fichier : le garde les compterait comme périmètre surdimensionné (#263).
    """
    match = re.match(r"^\s*[-*]\s+`?([^`\s]+)`?", ligne)
    return match.group(1).strip().strip("`") if match else None


def recomposer(corps, fichiers, raisons, garde):
    """Corps recomposé : puces conservées si elles servent, manquantes ajoutées,
    inutiles retirées. Rend (nouveau corps, ajouts, retraits)."""
    necessaires = [f["filename"] for f in fichiers
                   if not garde.is_exempt(f["filename"])]
    lignes = corps.splitlines()
    debut = next((i for i, ligne in enumerate(lignes)
                  if ligne.strip().lower().startswith("## scope")), None)
    if debut is None:
        ajouts = ["- `%s` — %s" % (chemin, raisons[chemin])
                  for chemin in necessaires]
        nouveau = (corps.rstrip() + "\n\n## Scope\n" + "\n".join(ajouts) + "\n"
                   if corps.strip() else "## Scope\n" + "\n".join(ajouts) + "\n")
        return nouveau, ajouts, []
    fin = next((i for i in range(debut + 1, len(lignes))
                if lignes[i].strip().startswith("## ")), len(lignes))
    gardees, couverts, retires = [], set(), []
    for ligne in lignes[debut + 1:fin]:
        motif = motif_declare(ligne)
        if motif is None:
            if ligne.strip():
                gardees.append(ligne)          # prose de l'auteur : intacte
            continue
        servis = [chemin for chemin in necessaires
                  if designe(motif, chemin, garde)]
        if servis:
            gardees.append(ligne)
            couverts.update(servis)
        else:
            retires.append(motif)
    ajouts = ["- `%s` — %s" % (chemin, raisons[chemin])
              for chemin in necessaires if chemin not in couverts]
    nouveau = "\n".join(lignes[:debut + 1] + gardees + ajouts
                        + lignes[fin:]).rstrip() + "\n"
    return nouveau, ajouts, retires


def ecrire_corps(repo, pr, corps):
    chemin = "/tmp/scope-agent-corps.md"
    with open(chemin, "w", encoding="utf-8") as fh:
        fh.write(corps)
    sh(["gh", "pr", "edit", str(pr), "--repo", repo, "--body-file", chemin])


def verifier_reglages():
    """Clés et coupe-circuit ; lève Skip si l'agent doit se taire."""
    key = env_or("FIREWORKS_API_KEY", "")
    if env_or("FIREWORKS_AGENT_SCOPE", "1") == "0":
        raise Skip("agent de périmètre coupé (FIREWORKS_AGENT_SCOPE=0)")
    if not key:
        raise Skip("FIREWORKS_API_KEY absent : périmètre laissé à l'auteur")
    return (key, env_or("FIREWORKS_MODEL", MODEL_DEFAULT),
            env_or("FIREWORKS_BASE_URL", API_DEFAULT))


def contexte(args, garde):
    """Corps, fichiers de la PR et fichiers à déclarer. Lève Skip si le garde
    est rouge pour autre chose que le périmètre : on ne réécrit pas un corps
    pour un échec que la réécriture ne corrige pas."""
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN") or ""
    if not token:
        raise AgentError("aucun jeton GitHub dans l'environnement")
    try:
        corps, fichiers = garde.pr_context(args.repo, args.pr, token)
    except Exception as exc:                                   # noqa: BLE001
        raise AgentError("API GitHub : %s" % str(exc)[:200])
    erreurs = []
    garde.check_scope(fichiers, corps, erreurs)
    if not erreurs:
        raise Skip("périmètre déjà conforme")
    autres = [e for e in erreurs if not e.startswith("scope")]
    if autres:
        raise Skip("couche0 rouge sur autre chose que le périmètre : %s"
                   % " ; ".join(autres)[:300])
    necessaires = [f["filename"] for f in fichiers
                   if not garde.is_exempt(f["filename"])]
    if not necessaires:
        raise Skip("aucun fichier à déclarer")
    if len(necessaires) > MAX_FILES:
        raise Skip("%d fichiers (plafond %d) : main humaine"
                   % (len(necessaires), MAX_FILES))
    return corps, fichiers, necessaires


def titre_pr(repo, pr):
    return sh(["gh", "pr", "view", str(pr), "--repo", repo,
               "--json", "title", "-q", ".title"])


def rendre_compte(args, ajouts, retires):
    """Commentaire unique : ce qui a été écrit, et la marque par tête."""
    lignes = ["Périmètre complété depuis le diff : **%d fichier(s)** déclaré(s)."
              % len(ajouts)]
    if retires:
        lignes.append("Déclaration(s) sans fichier correspondant retirée(s) : %s."
                      % ", ".join("`%s`" % m for m in retires))
    lignes.append("Vérifié avec le garde de `main` avant écriture : tout fichier "
                  "touché est déclaré, aucune déclaration inutile. `couche0` rejuge "
                  "sur ce nouveau corps.")
    lignes.append(MARKER % args.sha)
    comment(args.repo, args.pr, "\n\n".join(lignes))


def cmd_fix(args):
    key, model, api_url = verifier_reglages()
    if deja_tente(args.repo, args.pr, args.sha):
        raise Skip("périmètre déjà tenté pour cette tête")
    garde = load_guard(args.guard)
    corps, fichiers, necessaires = contexte(args, garde)
    titre = titre_pr(args.repo, args.pr)
    if args.dry_run:
        messages = messages_pour({"title": titre, "body": corps},
                                 fichiers, necessaires)
        print("===== prompt =====")
        print(messages[1]["content"][:6_000])
        return 0
    log("rédaction du périmètre (%d fichiers)" % len(necessaires))
    texte = fw_complete(api_url, key, model,
                        messages_pour({"title": titre, "body": corps},
                                      fichiers, necessaires))
    couverts, manquants = rapprocher(raisons_repondue(texte), necessaires, garde)
    if manquants:
        # Relance ciblée : les fichiers oubliés, seuls, avec le diff sous les
        # yeux. Une omission du modèle ne doit pas coûter la PR.
        log("relance ciblée : %d fichier(s) sans puce" % len(manquants))
        texte2 = fw_complete(api_url, key, model,
                             messages_pour({"title": titre, "body": corps},
                                           fichiers, manquants, relance=True))
        trouves, manquants = rapprocher(raisons_repondue(texte2), manquants, garde)
        couverts.update(trouves)
    if manquants:
        apercu = " ".join((texte or "").split())[:200]
        log("réponse du modèle : %s" % (apercu or "(vide)"))
        raise AgentError("le modèle n'a rien rendu pour : %s"
                         % ", ".join(manquants)[:300])
    nouveau, ajouts, retires = recomposer(corps, fichiers, couverts, garde)
    verif = []
    garde.check_scope(fichiers, nouveau, verif)
    if verif:
        raise AgentError("le garde refuse encore le corps recomposé : %s"
                         % " ; ".join(verif)[:300])
    ecrire_corps(args.repo, args.pr, nouveau)
    rendre_compte(args, ajouts, retires)
    log("corps réécrit (%d ajout(s), %d retrait(s))" % (len(ajouts), len(retires)))
    return 0


def fail(repo, pr, sha, raison):
    """Compte rendu d'échec : une fois par tête, jamais de boucle."""
    comment(repo, pr, "Périmètre **non complété** : %s\n\n"
                      "À l'auteur de déclarer `## Scope`.\n\n%s"
                      % (raison, MARKER % sha))
    add_label(repo, pr, LABEL_HUMAN)
    log("échec : %s" % raison)
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(prog="scope_agent")
    ap.add_argument("cmd", choices=["fix"])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--pr", required=True)
    ap.add_argument("--sha", required=True)
    ap.add_argument("--guard", required=True,
                    help="chemin du check_couche0.py téléchargé depuis main")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    try:
        return cmd_fix(args)
    except Skip as exc:
        log(str(exc))
        return 2
    except AgentError as exc:
        return fail(args.repo, args.pr, args.sha, str(exc))


if __name__ == "__main__":
    sys.exit(main())