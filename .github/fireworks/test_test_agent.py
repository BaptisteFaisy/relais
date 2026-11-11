#!/usr/bin/env python3
"""Tests ciblés de test_agent.py — bibliothèque standard uniquement.

Lance : python3 .github/fireworks/test_test_agent.py
"""
import os
import shutil
import tempfile
import unittest
from unittest import mock

import test_agent


class PromptTest(unittest.TestCase):
    """Le prompt porte les erreurs réelles, le dépôt et les règles."""

    def test_porte_extrait_et_regles(self):
        prompt = test_agent.prompt_fix("O/R", "12", "/tmp/clone",
                                       "FAILED FooTest > bar", "ci")
        self.assertIn("O/R", prompt)
        self.assertIn("/tmp/clone", prompt)
        self.assertIn("FAILED FooTest > bar", prompt)
        self.assertIn(".github/", prompt)
        self.assertIn("ne commit pas", prompt)
        self.assertIn("ABANDON", prompt)

    def test_journal_absent_dit_indisponible(self):
        prompt = test_agent.prompt_fix("O/R", "12", "/tmp/c", "")
        self.assertIn("indisponible", prompt)

    def test_porte_verif_python_et_kotlin(self):
        prompt = test_agent.prompt_fix("O/R", "12", "/tmp/clone", "FAILED x")
        self.assertIn("python3 -m unittest", prompt)
        self.assertIn("gradlew", prompt)

    def test_porte_le_triage_quand_il_existe(self):
        """Le triage évite la recherche à l'aveugle (#919 : 2 abandons)."""
        prompt = test_agent.prompt_fix(
            "O/R", "12", "/tmp/clone", "FAILED x", "",
            indice="- ProbeXTest hors périmètre\n- suspect : LiveProbePublish.kt")
        self.assertIn("Triage automatique", prompt)
        self.assertIn("ProbeXTest hors périmètre", prompt)
        self.assertIn("suspect : LiveProbePublish.kt", prompt)

    def test_sans_triage_aucun_bloc_vide(self):
        prompt = test_agent.prompt_fix("O/R", "12", "/tmp/clone", "FAILED x")
        self.assertNotIn("Triage automatique", prompt)


class BinaireTest(unittest.TestCase):
    """Localisation du binaire : PATH, puis les emplacements des runners."""

    def test_absent_partout(self):
        with mock.patch.object(test_agent.shutil, "which", return_value=None), \
                mock.patch.object(test_agent.os.path, "isfile",
                                  return_value=False):
            self.assertIsNone(test_agent.binaire_zeroclaw())

    def test_trouve_dans_le_path(self):
        with mock.patch.object(test_agent.shutil, "which",
                               return_value="/usr/local/bin/zeroclaw"), \
                mock.patch.object(test_agent.os.path, "isfile",
                                  return_value=True), \
                mock.patch.object(test_agent.os, "access", return_value=True):
            self.assertEqual(test_agent.binaire_zeroclaw(),
                             "/usr/local/bin/zeroclaw")


class DiffTest(unittest.TestCase):
    """Le pilote refuse les gardes, le volume et le hors-cadre."""

    def test_garde_refusee(self):
        ok, raison = test_agent.verifier_diff([".github/workflows/ci.yml"])
        self.assertFalse(ok)
        self.assertIn("garde", raison)

    def test_mergify_refuse(self):
        ok, _ = test_agent.verifier_diff([".mergify.yml"])
        self.assertFalse(ok)

    def test_trop_de_fichiers(self):
        fichiers = ["core/f%d.kt" % i for i in range(test_agent.MAX_FICHIERS + 1)]
        ok, raison = test_agent.verifier_diff(fichiers)
        self.assertFalse(ok)
        self.assertIn("plafond", raison)

    def test_diff_normal_accepte(self):
        with mock.patch.object(test_agent, "diff_lines", return_value=12):
            ok, raison = test_agent.verifier_diff(["core/a.kt", "app/b.kt"])
        self.assertTrue(ok)
        self.assertEqual(raison, "")

    def test_tests_seuls_refuses(self):
        ok, raison = test_agent.verifier_diff(
            ["core/src/test/java/dev/x/FooTest.kt"])
        self.assertFalse(ok)
        self.assertIn("tests", raison)

    def test_prod_plus_test_accepte(self):
        with mock.patch.object(test_agent, "diff_lines", return_value=3):
            ok, _ = test_agent.verifier_diff(
                ["core/src/test/java/dev/x/FooTest.kt", "core/src/main/x.kt"])
        self.assertTrue(ok)

    def test_trop_de_lignes(self):
        with mock.patch.object(test_agent, "diff_lines",
                               return_value=test_agent.MAX_LIGNES + 1):
            ok, raison = test_agent.verifier_diff(["core/a.kt"])
        self.assertFalse(ok)
        self.assertIn("lignes", raison)


class ChangementsTest(unittest.TestCase):
    """Lecture de `git status --porcelain` (renommages compris)."""

    def test_lit_modifies_ajoutes_renommes(self):
        sortie = (" M core/a.kt\n"
                  "A  app/b.kt\n"
                  'R  core/vieux.kt -> core/nouveau.kt\n'
                  "?? docs/note.md\n")
        faux = mock.Mock(returncode=0, stdout=sortie, stderr="")
        with mock.patch.object(test_agent.subprocess, "run", return_value=faux):
            fichiers = test_agent.changements()
        self.assertEqual(fichiers, ["core/a.kt", "app/b.kt",
                                    "core/nouveau.kt", "docs/note.md"])

    def test_sorties_doutils_ignorees(self):
        sortie = (" M core/a.kt\n"
                  "?? .kotlin/sessions/s1.bin\n"
                  "?? build/tmp/x\n"
                  "?? app/build/y\n")
        faux = mock.Mock(returncode=0, stdout=sortie, stderr="")
        with mock.patch.object(test_agent.subprocess, "run", return_value=faux):
            self.assertEqual(test_agent.changements(), ["core/a.kt"])

    def test_git_muet_rend_vide(self):
        faux = mock.Mock(returncode=1, stdout="", stderr="boom")
        with mock.patch.object(test_agent.subprocess, "run", return_value=faux):
            self.assertEqual(test_agent.changements(), [])


class JournalTest(unittest.TestCase):
    """L'extrait garde les lignes de verdict, dont les tracebacks Python."""

    def test_garde_traceback_fichier_et_erreur(self):
        journal = "\n".join([
            "ok   svg cases noires",
            "ERROR: test_format_exact (tools.test_solve_glasses_mire.TestBloc)",
            "Traceback (most recent call last):",
            '  File "/home/r/tools/test_solve_glasses_mire.py", line 49, in x',
            "KeyError: 'refW'",
            "Ran 8 tests in 0.002s",
            "FAILED (errors=1)",
        ])
        faux = mock.Mock(stdout=journal)
        with mock.patch.object(test_agent, "_gh", return_value=faux):
            extrait = test_agent.extrait_journal(["42"])
        self.assertIn("Traceback (most recent call last):", extrait)
        self.assertIn("line 49, in x", extrait)
        self.assertIn("KeyError: 'refW'", extrait)
        self.assertIn("FAILED (errors=1)", extrait)
        self.assertNotIn("svg cases noires", extrait)
        self.assertNotIn("Ran 8 tests", extrait)


class RunsTest(unittest.TestCase):
    """Les ids extraits sont des ids de run, pas de job (`/runs/<run>/job/<job>`)."""

    def test_prend_id_run(self):
        faux = mock.Mock(stdout="https://github.com/O/R/actions/runs/42701983653"
                                "/job/129448774833\n")
        with mock.patch.object(test_agent, "_gh", return_value=faux):
            self.assertEqual(test_agent.runs_en_echec("O/R", "abc"),
                             ["42701983653"])

    def test_lignes_vides_ignorees(self):
        faux = mock.Mock(stdout="\n")
        with mock.patch.object(test_agent, "_gh", return_value=faux):
            self.assertEqual(test_agent.runs_en_echec("O/R", "abc"), [])


class FixTest(unittest.TestCase):
    """Le pilote : borné, jamais de push d'un arbre cassé, jamais silencieux."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.ancien = os.getcwd()
        os.chdir(self.tmp)
        self.args = mock.Mock(repo="O/R", pr="1", branch="br", sha="abc123")

    def tearDown(self):
        os.chdir(self.ancien)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _patchs(self, **remplacements):
        base = {
            "labels": mock.patch.object(test_agent, "labels", return_value=[]),
            "marque_presente": mock.patch.object(test_agent, "marque_presente",
                                                 return_value=False),
            "runs_en_echec": mock.patch.object(test_agent, "runs_en_echec",
                                               return_value=["42"]),
            "extrait_journal": mock.patch.object(test_agent, "extrait_journal",
                                                 return_value="FAILED x"),
            "preparer_clone": mock.patch.object(test_agent, "preparer_clone",
                                                return_value=(True, "")),
            "lancer_agent": mock.patch.object(test_agent, "lancer_agent",
                                              return_value=(True, "CORRIGÉ")),
            "changements": mock.patch.object(test_agent, "changements",
                                             return_value=["core/a.kt"]),
            "comment": mock.patch.object(test_agent, "comment"),
            "add_label": mock.patch.object(test_agent, "add_label"),
            "pousser": mock.patch.object(test_agent, "pousser",
                                         return_value=(True, "")),
            "commiter": mock.patch.object(test_agent, "commiter",
                                          return_value=(True, "")),
        }
        base.update(remplacements)
        return base

    def _lancer(self, patchs):
        actifs = {nom: p.start() for nom, p in patchs.items()}
        self.addCleanup(lambda: [p.stop() for p in patchs.values()])
        with mock.patch.object(test_agent, "diff_lines", return_value=5):
            rc = test_agent.cmd_fix(self.args)
        return rc, actifs

    def test_desactive(self):
        with mock.patch.dict(os.environ, {"TEST_AGENT": "0"}):
            self.assertEqual(test_agent.cmd_fix(self.args), 2)

    def test_label_humain_saute(self):
        patchs = self._patchs()
        patchs["labels"] = mock.patch.object(test_agent, "labels",
                                             return_value=["intervention-humaine"])
        rc, _ = self._lancer(patchs)
        self.assertEqual(rc, 2)

    def test_deja_tente_pour_cette_tete(self):
        patchs = self._patchs()
        patchs["marque_presente"] = mock.patch.object(
            test_agent, "marque_presente", return_value=True)
        rc, _ = self._lancer(patchs)
        self.assertEqual(rc, 2)

    def test_aucun_changement_dit_abandon(self):
        patchs = self._patchs()
        patchs["changements"] = mock.patch.object(test_agent, "changements",
                                                  return_value=[])
        rc, actifs = self._lancer(patchs)
        self.assertEqual(rc, 2)
        self.assertIn("aucun changement", actifs["comment"].call_args[0][2])
        actifs["add_label"].assert_called_with("O/R", "1",
                                               test_agent.LABEL_HUMAN)

    def test_correctif_non_compilable_nest_pas_pousse(self):
        patchs = self._patchs()
        gate = mock.Mock()
        gate.verifier_compilation.return_value = (False, "e: x")
        patchs["gate"] = mock.patch.object(test_agent, "compilation_gate", gate)
        rc, actifs = self._lancer(patchs)
        self.assertEqual(rc, 1)
        actifs["pousser"].assert_not_called()
        gate.verifier_compilation.assert_called_once_with(["core/a.kt"])

    def test_correctif_garde_refuse(self):
        patchs = self._patchs()
        patchs["changements"] = mock.patch.object(
            test_agent, "changements", return_value=[".github/workflows/ci.yml"])
        rc, actifs = self._lancer(patchs)
        self.assertEqual(rc, 1)
        actifs["pousser"].assert_not_called()

    def test_outillage_absent_ne_consomme_pas_l_essai(self):
        """Runner non provisionné : code 3, aucune marque, aucun label, aucun push."""
        patchs = self._patchs()
        patchs["lancer_agent"] = mock.patch.object(
            test_agent, "lancer_agent",
            return_value=(False, test_agent.INFRA + "zeroclaw introuvable"))
        rc, actifs = self._lancer(patchs)
        self.assertEqual(rc, 3)
        actifs["comment"].assert_not_called()
        actifs["add_label"].assert_not_called()
        actifs["pousser"].assert_not_called()

    def test_echec_agent_hors_infra_reste_livre(self):
        """Un vrai échec de l'agent (pas l'outillage) garde son compte rendu."""
        patchs = self._patchs()
        patchs["lancer_agent"] = mock.patch.object(
            test_agent, "lancer_agent", return_value=(False, "ABANDON raison"))
        patchs["changements"] = mock.patch.object(test_agent, "changements",
                                                  return_value=[])
        rc, actifs = self._lancer(patchs)
        self.assertEqual(rc, 2)
        actifs["comment"].assert_called()

    def test_correctif_pousse(self):
        patchs = self._patchs()
        gate = mock.Mock()
        gate.verifier_compilation.return_value = (True, "ok")
        patchs["gate"] = mock.patch.object(test_agent, "compilation_gate", gate)
        rc, actifs = self._lancer(patchs)
        self.assertEqual(rc, 0)
        actifs["pousser"].assert_called_once_with("O/R", "br", "abc123")
        actifs["add_label"].assert_called_with("O/R", "1", test_agent.LABEL_OK)


if __name__ == "__main__":
    unittest.main()
