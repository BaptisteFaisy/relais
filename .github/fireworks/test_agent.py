#!/usr/bin/env python3
"""Agent de réparation des échecs de CI (zeroclaw `fw`) pour pr-auto-merge.

Quand une PR est `BLOCKED` sur un échec **réel** (un test rouge, une garde
rouge — pas une panne d'infra), le balayeur appelle ce pilote dans son clone :

1. il lit le journal des runs en échec de la tête (check-runs en REST) ;
2. il lance l'agent **zeroclaw `fw`** (`zeroclaw agent --agent fw -m`) sur le
   clone, avec un prompt qui porte les erreurs réelles ;
3. l'agent modifie des fichiers, **il ne commit pas et ne pousse pas** : le
   pilote garde le contrôle. Il refuse tout changement dans `.github/`, dans
   `.mergify.yml`, au-delà de 20 fichiers ou 1 500 lignes ;
4. il compile les modules touchés (`compilation_gate.py`) — un correctif qui
   ne compile pas n'est jamais poussé ;
5. il commit avec l'identité du propriétaire et pousse avec bail.

Une seule tentative par tête (`<!-- test-agent sha=<tête> -->`) : un agent ne
boucle pas. Coupe-circuit : variable de dépôt `TEST_AGENT=0`.

L'agent tourne dans un `--config-dir` isolé (copie de `config.toml` et de
`.secret_key`) : le démon ZeroClaw du runner tient le verrou de son propre
état, un agent concurrent refuserait de démarrer. Sur un runner sans
`zeroclaw`, le pilote le dit et rend 2 (rien à faire) — la PR reste visible
avec son compte rendu.

Bibliothèque standard uniquement. Usage :

  python3 test_agent.py fix --repo O/R --pr 12 --branch feat/x --sha <sha>

Codes : 0 = correctif poussé ; 1 = échec (compte rendu sur la PR) ; 2 = rien
à faire (désactivé, déjà tenté, aucun changement proposé) ; 3 = outillage
absent sur le runner (`zeroclaw` ou sa config) — **aucune tentative consommée**,
le balayeur réessaiera au passage suivant.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import types

BUDGET_S = 150            # temps laissé à l'agent, dans un job de 5 min
MAX_FICHIERS = 20
MAX_LIGNES = 1_500
MAX_JOURNAL_LIGNES = 120
REFUSES = (".github/", ".mergify.yml")
# Sorties d'outils (Gradle notamment) : elles apparaissent dans le clone quand
# l'agent a lancé une compilation, et n'ont rien à faire dans le correctif
# (vécu : `.kotlin/` commité sur la PR de test #652).
JAMAIS_COMMITES = (".kotlin/", "build/", ".gradle/", ".idea/", ".git/")
# Mêmes exclusions côté git (pathspec) : le commit ne prend que du code.
EXCLUSIONS_GIT = ("--", ":(exclude).kotlin/**", ":(exclude)**/build/**",
                  ":(exclude).gradle/**", ":(exclude).idea/**")
MARK = "<!-- test-agent sha=%s -->"
# Préfixe des sorties d'infrastructure : un runner non provisionné n'est pas un
# échec de l'agent, donc on ne pose **aucune marque** et l'essai de la tête
# reste ouvert. Vécu le 31/10 sur #652 : `zeroclaw introuvable` a brûlé l'unique
# tentative deux minutes avant la pose du binaire sur r630.
INFRA = "OUTILLAGE: "
LABEL_OK = "agent-test"
LABEL_HUMAN = "intervention-humaine"
PREFIXE = "_fireworks · agent de test (zeroclaw `fw`)_"
MOTIFS_JOURNAL = ("FAILED", "AssertionError", "Expected", "expected:",
                  "e: file", "error:", "Error:", "Caused by", "Caused by:",
                  "Tests failed", "> Task", "Traceback", "File \"",
                  "ERROR:", "FAIL:")


def _charger(nom):
    """Module voisin : import direct, sinon `origin/main` (branche antérieure)."""
    try:
        return __import__(nom)
    except ImportError:
        pass
    try:
        proc = subprocess.run(["git", "show", "origin/main:.github/fireworks/"
                               "%s.py" % nom], capture_output=True, text=True,
                              timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0 or not proc.stdout:
        return None
    module = types.ModuleType(nom)
    exec(compile(proc.stdout, "%s.py" % nom, "exec"), module.__dict__)
    return module


compilation_gate = _charger("compilation_gate")


def log(message):
    print("fw-test-agent: %s" % message, flush=True)


def sh(args, **kw):
    """Exécute une commande, rend sa sortie dépouillée ; lève sinon."""
    proc = subprocess.run(args, capture_output=True, text=True, **kw)
    if proc.returncode != 0:
        raise RuntimeError("%s : %s" % (" ".join(args[:3]),
                                        (proc.stderr or "").strip()[:200]))
    return (proc.stdout or "").strip()


def jeton_ecriture():
    """Token des écritures : le PAT quand il est là (identité du propriétaire)."""
    return os.environ.get("GH_ECRITURE") or os.environ.get("GH_TOKEN") or ""


def _gh(args, jeton=None, saisie=None):
    env = dict(os.environ)
    if jeton:
        env["GH_TOKEN"] = jeton
    return subprocess.run(["gh"] + args, capture_output=True, text=True,
                          env=env, timeout=saisie or 120)


def comment(repo, pr, body):
    """Commente la PR ; best-effort, ne lève jamais."""
    try:
        proc = _gh(["pr", "comment", str(pr), "--repo", repo, "--body",
                    PREFIXE + "\n\n" + body], jeton=jeton_ecriture())
        if proc.returncode != 0:
            log("commentaire refusé : %s" % (proc.stderr or "").strip()[:150])
    except Exception as exc:                                   # noqa: BLE001
        log("commentaire impossible : %s" % exc)


def add_label(repo, pr, label):
    try:
        _gh(["pr", "edit", str(pr), "--repo", repo, "--add-label", label],
            jeton=jeton_ecriture())
    except Exception as exc:                                   # noqa: BLE001
        log("label %s impossible : %s" % (label, exc))


def labels(repo, pr):
    proc = _gh(["pr", "view", str(pr), "--repo", repo, "--json", "labels",
                "-q", ".labels[].name"], jeton=os.environ.get("GH_TOKEN"))
    if proc.returncode != 0:
        return []
    return [ligne for ligne in (proc.stdout or "").splitlines() if ligne]


def marque_presente(repo, pr, marque):
    """Vrai si un commentaire de la PR porte déjà cette marque."""
    proc = _gh(["api", "repos/%s/issues/%s/comments?per_page=100" % (repo, pr),
                "--jq", ".[].body"], jeton=os.environ.get("GH_TOKEN"))
    return proc.returncode == 0 and marque in (proc.stdout or "")


def runs_en_echec(repo, sha):
    """Ids des runs en échec de la tête (checks requis et moitiés)."""
    proc = _gh(["api", "repos/%s/commits/%s/check-runs?per_page=100"
                % (repo, sha), "--jq",
                '[.check_runs[] | select(.conclusion == "failure") | .html_url]'
                ' | .[]'], jeton=os.environ.get("GH_TOKEN"))
    ids = []
    for ligne in (proc.stdout or "").splitlines():
        morceau = ligne.strip().split("/runs/")[-1].split("/")[0]  # .../runs/<run>/job/<job>
        if morceau.isdigit() and morceau not in ids:
            ids.append(morceau)
    return ids


def extrait_journal(runs, limite=MAX_JOURNAL_LIGNES):
    """Lignes utiles des journaux en échec (best-effort, jamais d'échec)."""
    garde = []
    for run in runs[:3]:
        proc = _gh(["run", "view", str(run), "--log-failed"],
                   jeton=os.environ.get("GH_TOKEN"))
        for ligne in (proc.stdout or "").splitlines():
            texte = ligne.split("\t")[-1].rstrip()
            if any(motif in texte for motif in MOTIFS_JOURNAL):
                garde.append(texte)
    # Les lignes les plus proches de la fin portent le verdict : on garde la queue.
    return "\n".join(garde[-limite:])


def prompt_fix(repo, pr, dossier, extrait, etapes="", indice=""):
    """Prompt de réparation : le dépôt, les erreurs, les règles, le verdict.

    [indice] = verdict de `tools/triage_test.py` (workflow `triage-rouge`) :
    sans lui, l'agent cherche à l'aveugle et rend « budget dépassé » (#919).
    """
    if indice.strip():
        etapes = etapes or "après triage"
    return "\n".join([
        "Tu es l'agent de réparation de la CI de %s (application Android "
        "Kotlin/KMP, code et commentaires en français)." % repo,
        "Le clone git est %s, sur la branche de la PR #%s." % (dossier, pr),
        "La CI y échoue%s." % (" sur : %s" % etapes if etapes else ""),
        "",
        "Extrait du journal d'échec :",
        "```",
        extrait or "(journal indisponible)",
        "```",
    ] + (["", "Triage automatique (déjà fait, ne le refais pas) :", "",
          indice.strip()] if indice.strip() else []) + [
        "",
        "Mission : corriger la CAUSE, au plus petit diff possible.",
        "Règles strictes :",
        "- ne touche jamais .github/, .mergify.yml, ni un fichier de garde ;",
        "- ne commit pas, ne pousse pas, ne change pas de branche ;",
        "- au plus %d fichiers et %d lignes modifiées ;" % (MAX_FICHIERS,
                                                            MAX_LIGNES),
        "- garde le style du dépôt (commentaires en français, phrases utiles) ;",
        "- si la correction propre sort du cadre (produit, contrat), ne modifie "
        "rien et réponds ABANDON.",
        "- tu peux vérifier en local (`python3 -m unittest tools/test_X.py` "
        "ou `./gradlew :core:testDebugUnitTest --tests \"*Nom*\"`), mais le "
        "temps est compté : la CI rejugera.",
        "",
        "Termine par une seule ligne : `CORRIGÉ <fichiers>` ou "
        "`ABANDON <raison>`.",
    ])


def config_isolee(source=None):
    """`--config-dir` isolé : copie config.toml + .secret_key, mode 700.

    Le démon ZeroClaw du runner tient le verrou de son état ; un `zeroclaw
    agent` concurrent refuserait de démarrer. Copie locale, supprimée après.
    """
    source = source or os.path.expanduser("~/.zeroclaw")
    conf = os.path.join(source, "config.toml")
    cle = os.path.join(source, ".secret_key")
    if not (os.path.isfile(conf) and os.path.isfile(cle)):
        return None
    dossier = tempfile.mkdtemp(prefix="zc-agent-")
    os.chmod(dossier, 0o700)
    try:
        # `data/` doit exister en 700 AVANT le lancement : ZeroClaw le crée au
        # umask du processus (002 sur les runners) et refuse un état partagé.
        os.makedirs(os.path.join(dossier, "data"), mode=0o700, exist_ok=True)
        os.chmod(os.path.join(dossier, "data"), 0o700)
        shutil.copyfile(conf, os.path.join(dossier, "config.toml"))
        shutil.copyfile(cle, os.path.join(dossier, ".secret_key"))
        os.chmod(os.path.join(dossier, "config.toml"), 0o600)
        os.chmod(os.path.join(dossier, ".secret_key"), 0o600)
    except OSError as exc:
        log("config isolée impossible : %s" % exc)
        shutil.rmtree(dossier, ignore_errors=True)
        return None
    return dossier


def binaire_zeroclaw():
    """Chemin de `zeroclaw` : PATH, puis les emplacements connus des runners."""
    candidats = [shutil.which("zeroclaw")] + [
        os.path.join(maison, ".cargo", "bin", "zeroclaw")
        for maison in (os.path.expanduser("~"), "/home/baptiste")]
    candidats.append("/usr/local/bin/zeroclaw")
    for chemin in candidats:
        if chemin and os.path.isfile(chemin) and os.access(chemin, os.X_OK):
            return chemin
    return None


def _umask_prive():
    """Umask 077 dans le processus fils (état ZeroClaw non partagé)."""
    os.umask(0o077)


def lancer_agent(prompt, dossier, budget=BUDGET_S):
    """Lance `zeroclaw agent --agent fw -m <prompt>`. Rend (ok, sortie)."""
    binaire = binaire_zeroclaw()
    if binaire is None:
        return False, INFRA + ("zeroclaw introuvable sur ce runner (PATH, "
                               "~/.cargo/bin, /home/baptiste/.cargo/bin, "
                               "/usr/local/bin)")
    conf = config_isolee()
    if conf is None:
        return False, INFRA + "config ZeroClaw absente (config.toml + .secret_key)"
    try:
        proc = subprocess.run([binaire, "--config-dir", conf, "agent",
                               "--agent", "fw", "-m", prompt],
                              capture_output=True, text=True, timeout=budget,
                              cwd=dossier, preexec_fn=_umask_prive)
    except subprocess.TimeoutExpired:
        return False, "budget de %d s dépassé" % budget
    except (OSError, subprocess.SubprocessError) as exc:
        return False, "lancement impossible : %s" % exc
    finally:
        shutil.rmtree(conf, ignore_errors=True)
    sortie = ((proc.stdout or "") + "\n" + (proc.stderr or "")).strip()
    return proc.returncode == 0, sortie[-2_000:]


def sortie_outil(chemin):
    """Vrai pour un répertoire de sortie d'outil (Gradle, IDE)."""
    return chemin.startswith(JAMAIS_COMMITES) or "/build/" in chemin


def changements():
    """Fichiers modifiés ou ajoutés dans le clone (hors suivis ignorés)."""
    proc = subprocess.run(["git", "status", "--porcelain"], capture_output=True,
                          text=True, timeout=120)
    if proc.returncode != 0:
        return []
    fichiers = []
    for ligne in (proc.stdout or "").splitlines():
        chemin = ligne[3:].strip()
        if " -> " in chemin:
            chemin = chemin.split(" -> ", 1)[1]
        chemin = chemin.strip('"')
        if chemin and not sortie_outil(chemin):
            fichiers.append(chemin)
    return fichiers


def diff_lines():
    """Lignes ajoutées + supprimées du diff de travail (0 si illisible)."""
    proc = subprocess.run(["git", "diff", "--numstat", "HEAD"],
                          capture_output=True, text=True, timeout=120)
    total = 0
    for ligne in (proc.stdout or "").splitlines():
        morceaux = ligne.split("\t")
        if len(morceaux) >= 2 and morceaux[0].isdigit():
            total += int(morceaux[0]) + int(morceaux[1])
    return total


def est_un_test(chemin):
    """Vrai pour un fichier de test (Kotlin, JVM)."""
    return "/src/test/" in chemin or chemin.endswith("Test.kt")


def verifier_diff(fichiers):
    """(ok, raison) : refus des gardes, du volume et du test affaibli."""
    refuses = [f for f in fichiers if f.startswith(REFUSES)]
    if refuses:
        return False, "touche un fichier de garde (%s)" % ", ".join(refuses[:3])
    # Un agent qui « corrige » en réécrivant le test ne prouve rien : il faut
    # au moins un fichier de production dans le correctif.
    if fichiers and all(est_un_test(f) for f in fichiers):
        return False, ("ne touche que des tests — un test réécrit ne prouve "
                       "rien, correctif refusé")
    if len(fichiers) > MAX_FICHIERS:
        return False, "%d fichiers (plafond %d)" % (len(fichiers), MAX_FICHIERS)
    lignes = diff_lines()
    if lignes > MAX_LIGNES:
        return False, "%d lignes modifiées (plafond %d)" % (lignes, MAX_LIGNES)
    return True, ""


def commiter(pr):
    """Commit d'identité propriétaire (règle du dépôt), sorties d'outils exclues."""
    subprocess.run(["git", "add", "-A"] + list(EXCLUSIONS_GIT),
                   capture_output=True, text=True, timeout=120)
    proc = subprocess.run(
        ["git", "-c", "user.name=BaptisteFaisy",
         "-c", "user.email=119856854+BaptisteFaisy@users.noreply.github.com",
         "commit", "-q", "-m",
         "fix(isee): correctif CI proposé par l'agent fw (PR #%s)" % pr],
        capture_output=True, text=True, timeout=120)
    return proc.returncode == 0, (proc.stderr or "").strip()[:200]


def pousser(repo, branch, sha):
    """Push avec bail : jamais d'écrasement d'un push arrivé entre-temps."""
    url = ("https://x-access-token:%s@github.com/%s.git"
           % (jeton_ecriture(), repo)) if jeton_ecriture() else "origin"
    proc = subprocess.run(["git", "push", "--force-with-lease=%s:%s"
                           % (branch, sha), url, "HEAD:%s" % branch],
                          capture_output=True, text=True, timeout=180)
    return proc.returncode == 0, (proc.stderr or "").strip()[:200]


def preparer_clone(repo, pr):
    """Récupère la tête de la PR et s'y place. Rend (ok, message)."""
    url = ("https://x-access-token:%s@github.com/%s.git"
           % (jeton_ecriture(), repo)) if jeton_ecriture() else "origin"
    try:
        sh(["git", "fetch", "-q", url, "pull/%s/head:pr-%s" % (pr, pr)],
           timeout=300)
        sh(["git", "checkout", "-q", "pr-%s" % pr], timeout=120)
    except (RuntimeError, subprocess.SubprocessError) as exc:
        return False, str(exc)
    return True, ""


def preflight(repo, pr, sha):
    """(runs en échec, extrait du journal), ou None si rien à faire."""
    if os.environ.get("TEST_AGENT", "1") == "0":
        log("désactivé (TEST_AGENT=0)")
        return None
    presents = set(labels(repo, pr))
    evites = presents & {"hold", LABEL_HUMAN}
    if evites:
        log("labels %s : main humaine" % ",".join(sorted(evites)))
        return None
    if marque_presente(repo, pr, MARK % sha):
        log("déjà tenté pour cette tête")
        return None
    runs = runs_en_echec(repo, sha)
    if not runs:
        log("aucun run en échec sur %s" % sha[:7])
        return None
    return runs, extrait_journal(runs)


def lancer_sur_clone(repo, pr, extrait, indice=""):
    """Fait travailler l'agent fw dans le clone (rend fichiers, sortie)."""
    prompt = prompt_fix(repo, pr, os.getcwd(), extrait, indice=indice)
    log("agent fw lancé (%d lignes de journal)" % extrait.count("\n"))
    debut = time.time()
    ok, sortie = lancer_agent(prompt, os.getcwd())
    log("agent rendu en %d s (ok=%s)" % (time.time() - debut, ok))
    if not ok and sortie.startswith(INFRA):
        # Runner non provisionné : rien n'a été tenté, rien ne doit être marqué.
        return None, sortie[len(INFRA):]
    return changements(), sortie


def refuser(repo, pr, marque, detail, raison):
    """Refus motivé : rien n'est poussé, la main humaine est nommée."""
    comment(repo, pr, "%s\n\nCorrectif **refusé** : %s.\n\n%s"
            % (marque, raison, detail))
    add_label(repo, pr, LABEL_HUMAN)
    return 1


def livrer(repo, pr, args, fichiers, sortie):
    """Garde-fous, compilation, commit, push. Rend 0 ou 1."""
    marque = MARK % args.sha
    if not fichiers:
        comment(repo, pr, "%s\n\nL'agent **n'a proposé aucun changement**"
                " (%s). La main humaine reste nécessaire.\n\n```\n%s\n```"
                % (marque, "échec de l'agent" if not sortie else "ABANDON",
                   (sortie or "")[-800:]))
        add_label(repo, pr, LABEL_HUMAN)
        return 2
    valide, raison = verifier_diff(fichiers)
    if not valide:
        return refuser(repo, pr, marque, "La branche reste intacte.", raison)
    if compilation_gate is not None:
        # La porte juge les fichiers que l'agent a VRAIMENT touchés, pas le
        # diff PR/main (un test jamais modifié par la PR y est invisible).
        ok, detail = compilation_gate.verifier_compilation(fichiers)
        if not ok:
            return refuser(repo, pr, marque, detail, "il ne compile pas")
    commit_ok, erreur = commiter(pr)
    if not commit_ok:
        return refuser(repo, pr, marque, erreur, "commit impossible")
    pousse, erreur = pousser(repo, args.branch, args.sha)
    if not pousse:
        return refuser(repo, pr, marque, erreur, "push refusé (bail)")
    liste = ", ".join("`%s`" % f for f in sorted(fichiers)[:10])
    comment(repo, pr, "%s\n\nCorrectif proposé et poussé sur `%s` : %s.\n\n"
            "Compilation vérifiée avant le push ; la CI rejuge le comportement."
            % (marque, args.branch, liste))
    add_label(repo, pr, LABEL_OK)
    log("correctif poussé (%d fichiers)" % len(fichiers))
    return 0


def cmd_fix(args):
    """Pilote : préflight, agent dans le clone, puis livraison gardée."""
    pre = preflight(args.repo, args.pr, args.sha)
    if pre is None:
        return 2
    runs, extrait = pre
    ok, message = preparer_clone(args.repo, args.pr)
    if not ok:
        comment(args.repo, args.pr, "Clone de la PR impossible : %s." % message)
        return 1
    indice = ""  # triage : jamais d'exception, il ne fait qu'aider
    if isinstance(args.indice, str) and args.indice:
        try:
            with open(args.indice, encoding="utf-8", errors="replace") as f:
                indice = f.read()[:4000]
        except OSError:  # indice disparu : l'agent travaille sans, comme avant
            pass
    fichiers, sortie = lancer_sur_clone(args.repo, args.pr, extrait, indice)
    if fichiers is None:
        # Outillage absent : aucune marque, aucun commentaire (le balayeur le dit
        # dans son journal), l'unique tentative de la tête reste ouverte.
        log("outillage absent, rien tenté : %s" % sortie)
        return 3
    return livrer(args.repo, args.pr, args, fichiers, sortie)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="test_agent")
    ap.add_argument("cmd", choices=["fix"])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--pr", required=True)
    ap.add_argument("--branch", required=True)
    ap.add_argument("--sha", required=True)
    ap.add_argument("--indice", default="", help="triage (tools/triage_test.py)")
    args = ap.parse_args(argv)
    return cmd_fix(args)


if __name__ == "__main__":
    sys.exit(main())
