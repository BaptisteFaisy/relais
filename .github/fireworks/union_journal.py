#!/usr/bin/env python3
"""Union déterministe des journaux en conflit, sans modèle.

Un journal (`CHANGELOG.md`) s'écrit en tête : deux entrées ajoutées de part et
d'autre ne sont pas un désaccord, c'est le même geste. Le 29/10, #215 a été
rebasée six fois sur `CHANGELOG.md` — une saisine LLM, un push et une CI par
rebase, jusqu'au plafond horaire. Ici, aucun appel : quand la base ne portait
rien dans un bloc de conflit, les deux côtés sont réunis ; un bloc qui remplace
une ligne commune reste au modèle, c'est un vrai désaccord.

Bibliothèque standard uniquement. Importé par `conflict_agent.py` (copié seul
par le sweep) : l'import est facultatif, son absence rend l'ancien chemin.
"""

import os
import subprocess
import tempfile

# Journaux en ajout pur : réunis sans modèle (racine ou sous-dossier).
FICHIERS_AJOUTS = ("CHANGELOG.md", "CHANGELOG")
MAX_FILE_BYTES = 200_000


def log(message):
    print("union-journal: %s" % message, flush=True)


def journal_ajout_pur(path):
    """Chemin d'un journal en ajout pur (racine ou sous-dossier) ?"""
    return any(path == nom or path.endswith("/" + nom) for nom in FICHIERS_AJOUTS)


def stages(path):
    """Trois versions d'un fichier en conflit : (base, nous, eux), ou None."""
    blobs = []
    for stage in (1, 2, 3):
        proc = subprocess.run(["git", "show", ":%d:%s" % (stage, path)],
                              capture_output=True, text=True, timeout=60)
        if proc.returncode != 0:
            return None
        blobs.append(proc.stdout)
    return tuple(blobs)


def _bloc(lignes, debut):
    """Découpe un bloc de conflit diff3 à partir de son marqueur d'ouverture.

    Rend (nous, base, eux, indice suivant), ou None si un marqueur manque.
    """
    i = debut + 1
    sections = []
    for marqueur in ("|||||||", "=======", ">>>>>>>"):
        contenu = []
        while i < len(lignes) and not lignes[i].startswith(marqueur):
            contenu.append(lignes[i])
            i += 1
        if i >= len(lignes):
            return None
        sections.append(contenu)
        i += 1
    return sections[0], sections[1], sections[2], i


def union_sans_perte(texte):
    """Réunit les deux côtés des conflits où la base ne portait rien.

    Un bloc de conflit diff3 dont la section base est vide est un ajout des
    deux côtés. Le réunir ne perd aucune ligne. Si UN bloc remplace une ligne
    commune, on rend None : c'est un vrai désaccord, le modèle tranche.
    """
    lignes = texte.splitlines(keepends=True)
    sortie, i, blocs = [], 0, 0
    while i < len(lignes):
        if not lignes[i].startswith("<<<<<<<"):
            sortie.append(lignes[i])
            i += 1
            continue
        bloc = _bloc(lignes, i)
        if bloc is None:
            return None
        nous, base, eux, i = bloc
        if any(ligne.strip() for ligne in base):
            return None
        sortie.extend(nous)
        sortie.extend(eux)
        blocs += 1
    return "".join(sortie) if blocs else None


def _fusion(base, nous, eux):
    """Sortie de `git merge-file --diff3` sur les trois versions, ou None."""
    temporaires = []
    try:
        for contenu in (nous, base, eux):
            with tempfile.NamedTemporaryFile("w", delete=False,
                                             suffix=".txt") as fh:
                fh.write(contenu)
                temporaires.append(fh.name)
        proc = subprocess.run(["git", "merge-file", "-p", "--diff3"]
                              + temporaires, capture_output=True, text=True,
                              timeout=60)
    finally:
        for nom in temporaires:
            try:
                os.unlink(nom)
            except OSError:
                pass
    if proc.returncode <= 0 or not proc.stdout:
        return None
    return proc.stdout


def resoudre_ajouts(path):
    """Résout sans modèle un conflit d'ajouts purs. Rend True si écrit+indexé."""
    if not journal_ajout_pur(path):
        return False
    blobs = stages(path)
    if blobs is None:
        return False
    base, nous, eux = blobs
    if max(len(base), len(nous), len(eux)) > MAX_FILE_BYTES:
        return False
    fusion = _fusion(base, nous, eux)
    resolu = union_sans_perte(fusion) if fusion else None
    if resolu is None:
        return False
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(resolu)
    indexe = subprocess.run(["git", "add", "--", path], capture_output=True,
                            text=True, timeout=60)
    return indexe.returncode == 0


def resoudre_par_union(files):
    """Union déterministe sur chaque fichier d'ajouts purs ; rend les résolus."""
    faits = []
    for path in files:
        if resoudre_ajouts(path):
            log("%s : ajouts des deux côtés — union sans modèle" % path)
            faits.append(path)
    return faits
