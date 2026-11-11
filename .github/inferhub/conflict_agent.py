#!/usr/bin/env python3
"""Agent de conflit InferHub pour la file pr-auto-merge.

Appelé par le sweep DANS son clone quand un rebase bute sur un conflit : il
fait résoudre chaque conflit élémentaire par un modèle InferHub, recopie le
reste du fichier intact (jamais de réécriture complète : un gros fichier serait
tronqué), boucle sur les arrêts (commits vidés sautés), pousse et rend compte.
`union_journal.py` réunit les journaux sans modèle ; `quota.py` borne les
saisines (cooldown, plafond horaire, 3 par PR en 30 min).

Porte de compilation (v3, 31/10) : une résolution qui ne compile pas n'est
**jamais poussée** (`compilation_gate.py`). Motif : #615, réconciliation cassée
classée « échec réel » puis abandonnée. Coupe-circuit : `CONFLICT_COMPILE_GATE=0`.
Usage : `resolve --repo O/R --pr N --branch b --sha s` (`--dry-run` : prompt
seul). Codes : 0 poussé, 1 échec commenté, 2 rien à faire. Aucun secret dedans.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import types
import urllib.request


def _charger(nom):
    """Module voisin : import direct, sinon `origin/main` (branche antérieure)."""
    try:
        return __import__(nom)
    except ImportError:
        pass
    try:
        proc = subprocess.run(["git", "show", "origin/main:.github/inferhub/"
                               "%s.py" % nom], capture_output=True, text=True,
                              timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0 or not proc.stdout:
        return None
    module = types.ModuleType(nom)
    exec(compile(proc.stdout, "%s.py" % nom, "exec"), module.__dict__)
    return module


union_journal = _charger("union_journal")
quota = _charger("quota")
# v3 : porte de compilation à côté (`compilation_gate.py`) : plafond de 500 lignes.
compilation_gate = _charger("compilation_gate")

MODEL_DEFAULT = "combo/cheap"
API_DEFAULT = "https://api.inferhub.dev/v1/chat/completions"
MAX_FILES = 100
MAX_FILE_BYTES = 200_000
MAX_HUNKS = 20
HUNK_MAX_CHARS = 12_000
MAX_ROUNDS = 10
BUDGET_S = 240
CALL_TIMEOUT_S = 90
MAX_TOKENS = 8_000
TEMPERATURE = 0.2
LABEL_OK = "agent-conflit"
LABEL_HUMAN = "intervention-humaine"
COMMENT_PREFIX = "_inferhub · agent de conflit (combo/cheap)_"
REFUSED_PREFIXES = (".github/", ".mergify.yml")


class AgentError(Exception):
    """Échec avec message publiable (sans secret)."""


def sh(args, **kw):
    """Exécute une commande, rend sa sortie texte dépouillée."""
    proc = subprocess.run(args, capture_output=True, text=True, **kw)
    if proc.returncode != 0:
        raise AgentError("%s : %s" % (" ".join(args[:3]),
                                      (proc.stderr or "").strip()[:200]))
    return (proc.stdout or "").strip()


def log(message):
    print("inferhub-agent: %s" % message, flush=True)


def pr_labels(repo, pr):
    """Noms des labels de la PR (liste vide si illisible)."""
    try:
        out = sh(["gh", "pr", "view", str(pr), "--repo", repo,
                  "--json", "labels", "-q", ".labels[].name"])
        return [line for line in out.splitlines() if line]
    except AgentError:
        return []


def comment(repo, pr, body):
    """Commente la PR ; best-effort, ne lève jamais."""
    try:
        proc = subprocess.run(["gh", "pr", "comment", str(pr), "--repo", repo,
                               "--body", COMMENT_PREFIX + "\n\n" + body],
                              capture_output=True, text=True, timeout=60)
        if proc.returncode != 0:
            log("commentaire refusé : %s" % proc.stderr.strip()[:150])
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


def abort_rebase():
    try:
        subprocess.run(["git", "rebase", "--abort"], capture_output=True,
                       timeout=60)
    except Exception:                                          # noqa: BLE001
        pass


def unmerged_files():
    out = sh(["git", "diff", "--name-only", "--diff-filter=U"])
    return [line for line in out.splitlines() if line]


def split_hunks(text):
    """Découpe un fichier marqué : ("clean", txt) / ("hunk", dict).

    Gère diff et diff3 (avec ou sans base). En rebase, le premier bloc
    ("head") est main (destination) et le second ("saved") la branche.
    """
    segments, buf = [], []
    lines = text.split("\n")
    n, i = len(lines), 0
    while i < n:
        if not lines[i].startswith("<<<<<<<"):
            buf.append(lines[i])
            i += 1
            continue
        if buf:
            segments.append(("clean", "\n".join(buf)))
            del buf[:]
        head, base, saved = [], None, []
        i += 1
        while (i < n and lines[i] != "======="
               and not lines[i].startswith("|||||||")):
            head.append(lines[i])
            i += 1
        if i < n and lines[i].startswith("|||||||"):
            base = []
            i += 1
            while i < n and lines[i] != "=======":
                base.append(lines[i])
                i += 1
        if i < n and lines[i] == "=======":
            i += 1
        while i < n and not lines[i].startswith(">>>>>>>"):
            saved.append(lines[i])
            i += 1
        i += 1
        segments.append(("hunk", {"head": head, "base": base, "saved": saved}))
    if buf:
        segments.append(("clean", "\n".join(buf)))
    return segments


def build_hunk_messages(path, hunk):
    """Prompt de résolution pour UN conflit élémentaire (rebase)."""
    system = ("Tu résous UN conflit de fusion git (un seul morceau de "
              "fichier). Tu rends UNIQUEMENT le texte résolu de ce morceau, "
              "sans aucun marqueur, en gardant les deux intentions (jamais "
              "un bord choisi à l'aveugle). Tu ne réponds que par le texte, "
              "dans un seul bloc de code.")
    parts = ["Fichier : %s (extrait)" % path,
             "--- Version de la branche (à garder en priorité en cas de doute) ---",
             "\n".join(hunk["saved"])]
    if hunk["base"] is not None:
        parts += ["--- Version ancêtre commun ---", "\n".join(hunk["base"])]
    parts += ["--- Version de main ---", "\n".join(hunk["head"]),
              "Rends le morceau résolu, dans un seul bloc de code."]
    return [{"role": "system", "content": system},
            {"role": "user", "content": "\n".join(parts)}]


def ih_complete(api_url, key, model, messages):
    """Un appel chat/completions InferHub, rend le texte réponse."""
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
        raise AgentError("appel InferHub : %s" % str(exc)[:200])
    try:
        return data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        raise AgentError("réponse InferHub illisible (pas de choices[0])")


def extract_file(text):
    """Extrait le fichier résolu : bloc de code, sinon texte brut."""
    cleaned = re.sub(r"<think>.*?</think>", "", text, flags=re.S)
    match = re.search(r"```[^\n]*\n(.*?)```", cleaned, flags=re.S)
    body = match.group(1) if match else cleaned
    return body.strip("\n") + "\n"


def still_marked(content):
    """Vrai s'il reste des marqueurs de conflit git."""
    for line in content.splitlines():
        if line.startswith("<<<<<<< ") or line == "<<<<<<<":
            return True
        if line.startswith(">>>>>>> ") or line == ">>>>>>>":
            return True
        if line == "=======":
            return True
    return False


def resolve_one(api_url, key, model, path, deadline, dry_run):
    """Résout un fichier conflit par conflit, le reste recopié intact."""
    if time.time() > deadline:
        raise AgentError("budget de %d s dépassé" % BUDGET_S)
    try:
        size = os.path.getsize(path)
    except OSError:
        raise AgentError("fichier illisible : %s" % path)
    if size > MAX_FILE_BYTES:
        raise AgentError("fichier trop gros : %s (%d o)" % (path, size))
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    if "\x00" in text[:8_000]:
        raise AgentError("binaire : %s" % path)
    segments = split_hunks(text)
    hunks = [seg for kind, seg in segments if kind == "hunk"]
    if not hunks:
        raise AgentError("conflit sans marqueurs (suppression ?) : %s" % path)
    if len(hunks) > MAX_HUNKS:
        raise AgentError("%d conflits élémentaires (plafond %d) : %s"
                         % (len(hunks), MAX_HUNKS, path))
    if dry_run:
        for seg in hunks:
            messages = build_hunk_messages(path, seg)
            print("===== %s =====" % path)
            print(messages[1]["content"][:4_000])
        return
    log("résolution de %s (%d conflits)" % (path, len(hunks)))
    out = []
    for kind, seg in segments:
        if kind == "clean":
            out.append(seg)
            continue
        if time.time() > deadline:
            raise AgentError("budget de %d s dépassé" % BUDGET_S)
        total = sum(len("\n".join(seg[k])) for k in ("head", "saved"))
        if seg["base"] is not None:
            total += len("\n".join(seg["base"]))
        if total > HUNK_MAX_CHARS:
            raise AgentError("conflit élémentaire trop gros : %s" % path)
        messages = build_hunk_messages(path, seg)
        resolved = extract_file(ih_complete(api_url, key, model, messages))
        if still_marked(resolved):
            raise AgentError("le modèle a rendu des marqueurs : %s" % path)
        out.append(resolved.rstrip("\n"))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))
    sh(["git", "add", "--", path])


def fail(repo, pr, reason):
    """Compte rendu d'échec : rebase annulé, commentaire + label humain."""
    abort_rebase()
    comment(repo, pr, "Conflit **non résolu** : %s\n\nÀ l'auteur de rebaser." % reason)
    add_label(repo, pr, LABEL_HUMAN)
    log("échec : %s" % reason)
    return 1


class _SaisineLocale(Exception):
    """Repli si `quota.py` n'est pas joignable (déploiement ancien)."""


Skip = quota.Saisine if quota else _SaisineLocale


def env_or(name, default):
    """Variable d'env ou défaut. GitHub injecte une var absente comme ""
    (et non unset) : `os.environ.get(n, d)` rendrait "" → 400 InferHub."""
    return os.environ.get(name) or default


def preflight(repo, pr, sha, dry_run, now):
    """Garde-fous et lecture des fichiers en conflit. Rend (clé, modèle, url, fichiers)."""
    if os.environ.get("INFERHUB_AGENT_RESOLVE", "1") == "0":
        raise Skip("désactivé (INFERHUB_AGENT_RESOLVE=0)")
    key = os.environ.get("INFERHUB_API_KEY", "")
    if not key and not dry_run:
        raise Skip("pas de clé INFERHUB_API_KEY")
    if quota is not None:
        quota.gc_stamps(now)
    labels = pr_labels(repo, pr)
    if "hold" in labels or LABEL_HUMAN in labels:
        raise Skip("labels %s : main humaine" % ",".join(labels))
    # Cooldown et plafonds à l'instant de la saisine : un conflit d'ajouts purs
    # se règle sans modèle, même plafond épuisé.
    try:
        files = unmerged_files()
    except AgentError as exc:
        raise Skip("git illisible : %s" % exc)
    if not files:
        raise Skip("aucun fichier en conflit")
    check_round_files(files)
    model = env_or("INFERHUB_MODEL", MODEL_DEFAULT)
    api_url = env_or("INFERHUB_BASE_URL", API_DEFAULT)
    return key, model, api_url, files


def push_url(repo):
    """URL push authentifiée (GH_TOKEN) ; "origin" en repli local."""
    token = os.environ.get("GH_TOKEN", "")
    if not token:
        return "origin"
    return "https://x-access-token:%s@github.com/%s.git" % (token, repo)


def rebase_in_progress():
    """Vrai si un rebase est en cours (répertoire d'état présent)."""
    try:
        gitdir = sh(["git", "rev-parse", "--absolute-git-dir"])
    except AgentError:
        return False
    return os.path.isdir(os.path.join(gitdir, "rebase-merge")) or \
        os.path.isdir(os.path.join(gitdir, "rebase-apply"))


def continue_or_skip(env):
    """Un pas de rebase : --continue, --skip si commit vidé. Rend (ok, erreur)."""
    cont = subprocess.run(["git", "rebase", "--continue"], env=env,
                          capture_output=True, text=True, timeout=120)
    if cont.returncode == 0:
        return True, ""
    err = (((cont.stderr or "") + "\n" + (cont.stdout or "")).strip())[:200]
    if "empty" in err.lower():
        try:
            leftover = unmerged_files()
        except AgentError:
            leftover = ["?"]
        if not leftover:
            skip = subprocess.run(["git", "rebase", "--skip"], env=env,
                                  capture_output=True, text=True, timeout=120)
            if skip.returncode == 0:
                log("commit vidé par la résolution, --skip")
                return True, ""
    return False, err


def check_round_files(files):
    """Garde-fous d'une passe : chemins refusés et plafond de fichiers."""
    refused = [p for p in files if p.startswith(REFUSED_PREFIXES)]
    if refused:
        raise AgentError("conflit dans %s (main humaine requise)"
                         % ", ".join(refused))
    if len(files) > MAX_FILES:
        raise AgentError("%d fichiers en conflit (plafond %d)"
                         % (len(files), MAX_FILES))


def autorisation_saisine(pr, sha):
    """Cooldown et plafonds, à l'instant d'appeler le modèle (quota.py).

    Séparé de `preflight` pour que l'union déterministe des journaux
    (gratuite) ne soit jamais bloquée par un plafond d'appels.
    """
    if quota is not None:
        quota.autoriser(pr, sha)


def finish_loop(api_url, key, model, deadline, autorisation=None):
    """Boucle multi-passes : résout chaque arrêt, continue/skip. Rend fichiers."""
    resolved = []
    env = dict(os.environ, GIT_EDITOR="true")
    autorise = autorisation is None
    for round_no in range(1, MAX_ROUNDS + 1):
        if time.time() > deadline:
            raise AgentError("budget de %d s dépassé" % BUDGET_S)
        files = unmerged_files()
        if files:
            check_round_files(files)
            log("passe %d : %d fichier(s)" % (round_no, len(files)))
            faits = (union_journal.resoudre_par_union(files)
                     if union_journal else [])
            reste = [path for path in files if path not in faits]
            if reste:
                if not autorise:
                    autorisation()
                    autorise = True
                for path in reste:
                    resolve_one(api_url, key, model, path, deadline, False)
            resolved.extend(files)
        done, err = continue_or_skip(env)
        if not rebase_in_progress():
            if done:
                return resolved
            raise AgentError("rebase interrompu : %s" % err)
        if not unmerged_files():
            raise AgentError("rebase bloqué sans conflit : %s" % err)
    raise AgentError("rebase à plus de %d arrêts, main humaine" % MAX_ROUNDS)


def porte_compilation(files, branch):
    """(ok, message, corps) ; neutre si le module voisin n'est pas joignable."""
    if compilation_gate is None:
        return True, "", "Conflit réconcilié, rebase poussé sur `%s`." % branch
    return compilation_gate.prepare_push(files, branch)


def finish(repo, pr, branch, sha, files):
    """Pousse le rebase terminé avec bail, rend compte. Rend 0 ou 1."""
    ok, message, corps = porte_compilation(files, branch)
    if not ok:
        # Rien n'est poussé : branche intacte, PR DIRTY (retour au tour suivant).
        comment(repo, pr, "Résolution **non compilable** : rien n'est poussé, "
                "la branche reste intacte.\n\n%s" % message)
        add_label(repo, pr, LABEL_HUMAN)
        log("porte de compilation rouge : rien n'est poussé")
        return 1
    push = subprocess.run(["git", "push",
                           "--force-with-lease=%s:%s" % (branch, sha),
                           push_url(repo), "HEAD:%s" % branch],
                          capture_output=True, text=True, timeout=120)
    if push.returncode != 0:
        log("push refusé (bail, prochain passage) : %s"
            % push.stderr.strip()[:150])
        return 1
    comment(repo, pr, corps)   # corps porte le marqueur de tête (blayeur)
    add_label(repo, pr, LABEL_OK)
    log("PR #%s rebasée et poussée (%d fichiers)" % (pr, len(set(files))))
    return 0


def cmd_resolve(args):
    repo, pr = args.repo, args.pr
    now = time.time()
    try:
        key, model, api_url, files = preflight(repo, pr, args.sha,
                                               args.dry_run, now)
    except Skip as exc:
        log(str(exc))
        return 2
    except AgentError as exc:
        return fail(repo, pr, str(exc))
    deadline = now + BUDGET_S
    if args.dry_run:
        try:
            for path in files:
                resolve_one(api_url, key, model, path, deadline, True)
        except AgentError as exc:
            return fail(repo, pr, str(exc))
        return 0
    # Le dry-run ne touche jamais l'empreinte (ni cooldown ni plafond horaire).
    try:
        resolved = finish_loop(api_url, key, model, deadline,
                               lambda: autorisation_saisine(pr, args.sha))
    except Skip as exc:
        # Union faite, saisine refusée : rien n'est poussé, l'appelant abandonne
        # le rebase et la PR repart au prochain tour.
        log(str(exc))
        return 2
    except AgentError as exc:
        return fail(repo, pr, str(exc))
    return finish(repo, pr, args.branch, args.sha, resolved)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="conflict_agent")
    ap.add_argument("cmd", choices=["resolve"])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--pr", required=True)
    ap.add_argument("--branch", default="")
    ap.add_argument("--sha", default="")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    if args.cmd == "resolve" and not args.dry_run and not (args.branch and args.sha):
        ap.error("--branch et --sha sont requis hors --dry-run")
    return cmd_resolve(args)


if __name__ == "__main__":
    sys.exit(main())
