#!/usr/bin/env python3
"""Tests ciblés de union_journal.py — bibliothèque standard uniquement.

Lance : python3 -m unittest test_union_journal -v
(depuis .github/inferhub/).
"""
import os
import shutil
import subprocess
import time
import tempfile
import unittest

import union_journal


def _scratch_repo(test):
    """Dépôt git réel en temp, cwd dedans, nettoyé après. Rend run()."""
    repo = tempfile.mkdtemp(prefix="fw-union-")
    test.addCleanup(shutil.rmtree, repo, True)
    old = os.getcwd()
    test.addCleanup(os.chdir, old)
    os.chdir(repo)

    def run(*args):
        return subprocess.run(["git", *args], capture_output=True, text=True)
    run("init", "-q")
    run("config", "user.email", "t@t")
    run("config", "user.name", "t")
    run("config", "commit.gpgsign", "false")
    return run


def _non_fusionnes():
    out = subprocess.run(["git", "diff", "--name-only", "--diff-filter=U"],
                         capture_output=True, text=True).stdout
    return [ligne for ligne in out.splitlines() if ligne]


def _commit(run, files, message):
    for path, content in files.items():
        with open(path, "w") as fh:
            fh.write(content)
    run("add", ".")
    assert run("commit", "-qm", message).returncode == 0


class UnionAjoutsTest(unittest.TestCase):
    """Journaux : deux entrées écrites en tête se réunissent sans modèle."""

    def test_deux_ajouts_en_tete(self):
        texte = ("# Journal\n\n"
                 "<<<<<<< HEAD\n## Entrée main\n- fait A\n"
                 "||||||| base\n"
                 "=======\n## Entrée branche\n- fait B\n"
                 ">>>>>>> 3e0e33b\n\n## Plus ancien\n")
        self.assertEqual(union_journal.union_sans_perte(texte),
                         "# Journal\n\n## Entrée main\n- fait A\n"
                         "## Entrée branche\n- fait B\n\n## Plus ancien\n")

    def test_ligne_commune_modifiee_refusee(self):
        # Une ligne de base remplacée des deux côtés : vrai désaccord.
        texte = ("<<<<<<< HEAD\nversion main\n"
                 "||||||| base\nversion commune\n"
                 "=======\nversion branche\n>>>>>>> x\n")
        self.assertIsNone(union_journal.union_sans_perte(texte))

    def test_marqueur_incomplet_refuse(self):
        self.assertIsNone(union_journal.union_sans_perte("<<<<<<< HEAD\na\n"))

    def test_chemin_de_journal(self):
        self.assertTrue(union_journal.journal_ajout_pur("CHANGELOG.md"))
        self.assertTrue(union_journal.journal_ajout_pur("docs/CHANGELOG.md"))
        self.assertFalse(union_journal.journal_ajout_pur("CHANGELOG-OLD.md"))
        self.assertFalse(union_journal.journal_ajout_pur("app/A.kt"))


class ResoudreAjoutsTest(unittest.TestCase):
    """Bout en bout sur un vrai rebase : CHANGELOG.md réuni, code au modèle."""

    def _conflit(self, chemin, base, nous, eux):
        run = _scratch_repo(self)
        _commit(run, {chemin: base}, "base")
        base_branch = run("branch", "--show-current").stdout.strip()
        run("checkout", "-qb", "br")
        _commit(run, {chemin: nous}, "br")
        run("checkout", "-q", base_branch)
        _commit(run, {chemin: eux}, "m")
        run("checkout", "-q", "br")
        failed = subprocess.run(["git", "rebase", base_branch],
                                capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        return run

    def test_changelog_reuni_sans_modele(self):
        # Non-régression 29/10 : #215 rebasée six fois sur CHANGELOG.md, une
        # saisine LLM (et son quota) par rebase.
        self._conflit("CHANGELOG.md", "# Journal\n\n## Ancien\n- vieux\n",
                      "# Journal\n\n## Branche\n- B\n\n## Ancien\n- vieux\n",
                      "# Journal\n\n## Master\n- M\n\n## Ancien\n- vieux\n")
        self.assertTrue(union_journal.resoudre_ajouts("CHANGELOG.md"))
        self.assertEqual(_non_fusionnes(), [])
        with open("CHANGELOG.md") as fh:
            contenu = fh.read()
        self.assertIn("## Master", contenu)
        self.assertIn("## Branche", contenu)
        self.assertIn("- vieux", contenu)
        self.assertNotIn("<<<<<<<", contenu)

    def test_fichier_hors_journal_rend_la_main(self):
        self._conflit("app.txt", "base\n", "branche\n", "main\n")
        self.assertFalse(union_journal.resoudre_ajouts("app.txt"))
        self.assertEqual(_non_fusionnes(), ["app.txt"])


if __name__ == "__main__":
    unittest.main()
