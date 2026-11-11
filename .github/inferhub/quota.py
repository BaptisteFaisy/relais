#!/usr/bin/env python3
"""Quota de saisines du modèle : empreintes, cooldown, plafonds.

Les runners changent d'un passage à l'autre : les empreintes vivent dans
`~/.cache/inferhub-agent`, jamais dans le clone. Deux plafonds : global
(`SPAWN_CAP` par heure) et par PR (`SAISINES_PR_MAX` en 30 min). Le second est
né du 29/10 : #215, rebasée en boucle, a mangé tout le quota horaire et l'agent
s'est tu pour les autres PR.

Bibliothèque standard uniquement. Importé par `conflict_agent.py`, qui le
charge depuis `origin/main` (voir `_charger`) : le module peut manquer sur un
déploiement ancien, l'appelant gère alors l'absence.
"""

import os
import time

COOLDOWN_S = 20 * 60
SPAWN_WINDOW_S = 3_600
SPAWN_CAP = 10
SAISINES_PR_MAX = 3
SAISINES_PR_FENETRE_S = 30 * 60


class Saisine(Exception):
    """Rien à faire : l'appelant saute la PR sans bruit (exit 2)."""


def cache_dir():
    chemin = os.path.join(os.path.expanduser("~"), ".cache", "inferhub-agent")
    os.makedirs(chemin, exist_ok=True)
    return chemin


def stamp_path(pr, sha):
    return os.path.join(cache_dir(), "pr-%s-%s.stamp" % (pr, sha[:12]))


def gc_stamps(now):
    """Purge les empreintes de plus d'un jour."""
    jour_avant = now - 86_400
    for name in os.listdir(cache_dir()):
        if not name.endswith(".stamp"):
            continue
        full = os.path.join(cache_dir(), name)
        try:
            if os.path.getmtime(full) < jour_avant:
                os.unlink(full)
        except OSError:
            continue


def _recentes(prefixe, cutoff):
    """Empreintes dont le préfixe commence par `prefixe` et récentes."""
    total = 0
    for name in os.listdir(cache_dir()):
        if not name.startswith(prefixe) or not name.endswith(".stamp"):
            continue
        try:
            if os.path.getmtime(os.path.join(cache_dir(), name)) >= cutoff:
                total += 1
        except OSError:
            continue
    return total


def recent_runs(now):
    """Nombre de saisines dans la fenêtre glissante."""
    return _recentes("pr-", now - SPAWN_WINDOW_S)


def saisines_pr(pr, now):
    """Saisines récentes pour CETTE PR, toutes têtes confondues (fenêtre)."""
    return _recentes("pr-%s-" % pr, now - SAISINES_PR_FENETRE_S)


def has_fresh_stamp(pr, sha, now):
    try:
        return os.path.getmtime(stamp_path(pr, sha)) >= now - COOLDOWN_S
    except OSError:
        return False


def touch_stamp(pr, sha):
    with open(stamp_path(pr, sha), "w") as fh:
        fh.write(str(int(time.time())))


def release_stamp(pr, sha):
    """Rend le cooldown quand le modèle n'a jamais été atteint.

    `autoriser` estampille AVANT l'appel, ce qui est le bon choix pour borner le
    coût : un modèle qui répond mal ne doit pas être ré-interrogé en boucle. Mais
    une erreur de transport (mauvaise URL, 404, fournisseur injoignable) n'a
    consommé aucun jeton : sans cette libération, une variable mal renseignée
    immobilise la PR pendant tout le cooldown, et le conflit attend un humain
    pour une raison purement administrative.
    """
    try:
        os.remove(stamp_path(pr, sha))
    except OSError:
        pass


def autoriser(pr, sha):
    """Lève Saisine si le modèle ne doit pas être appelé maintenant.

    Séparé de la lecture du conflit pour que l'union déterministe des journaux
    (gratuite) ne soit jamais bloquée par un plafond d'appels.
    """
    maintenant = time.time()
    if has_fresh_stamp(pr, sha, maintenant):
        raise Saisine("saisine récente pour %s, cooldown" % sha[:12])
    if recent_runs(maintenant) >= SPAWN_CAP:
        raise Saisine("plafond de %d saisines/h atteint" % SPAWN_CAP)
    if saisines_pr(pr, maintenant) >= SAISINES_PR_MAX:
        raise Saisine("%d saisines en %d min pour cette PR : désaccord de fond, "
                      "main humaine"
                      % (SAISINES_PR_MAX, SAISINES_PR_FENETRE_S // 60))
    touch_stamp(pr, sha)
