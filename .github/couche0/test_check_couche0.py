#!/usr/bin/env python3
"""Tests ciblés de check_couche0.py — bibliothèque standard uniquement.

Lance : python3 .github/couche0/test_check_couche0.py
(depuis la racine du dépôt).
"""
import unittest

import check_couche0


def fichier(nom, patch=""):
    return {"filename": nom, "patch": patch}


class ParseScopeTest(unittest.TestCase):
    """Extraction des motifs déclarés dans `## Scope`."""

    def test_puces_extraites(self):
        corps = "Titre\n\n## Scope\n\n- `app/Foo.kt` — raison\n- `core/` — raison\n"
        self.assertEqual(check_couche0.parse_scope(corps),
                         ["app/Foo.kt", "core/"])

    def test_arret_section_suivante(self):
        corps = "## Scope\n\n- `a.kt` — x\n\n## Autre\n\n- `b.kt` — y\n"
        self.assertEqual(check_couche0.parse_scope(corps), ["a.kt"])

    def test_sans_scope_rend_vide(self):
        self.assertEqual(check_couche0.parse_scope("pas de section"), [])


class MatchesTest(unittest.TestCase):
    """Un motif désigne un fichier : exact, dossier, ou glob."""

    def test_exact(self):
        self.assertTrue(check_couche0.matches("a/b.kt", "a/b.kt"))
        self.assertFalse(check_couche0.matches("a/b.kt", "a/c.kt"))

    def test_dossier(self):
        self.assertTrue(check_couche0.matches("core/", "core/net/Http.kt"))

    def test_glob(self):
        self.assertTrue(check_couche0.matches("*.md", "NOTES.md"))


class ExemptTest(unittest.TestCase):
    """Fichiers régénérables : pas besoin de les déclarer."""

    def test_build_exempte(self):
        self.assertTrue(check_couche0.is_exempt("app/build/rapport.xml"))

    def test_png_exempte(self):
        self.assertTrue(check_couche0.is_exempt("docs/capture.png"))

    def test_source_non_exemptee(self):
        self.assertFalse(check_couche0.is_exempt("app/Foo.kt"))


class ScopeTest(unittest.TestCase):
    """Tout fichier modifié déclaré, toute déclaration servie."""

    def test_non_declare_signale(self):
        erreurs = []
        corps = "## Scope\n\n- `docs/` — x\n"
        check_couche0.check_scope([fichier("app/Foo.kt")], corps, erreurs)
        non_declare = [e for e in erreurs if "non déclaré" in e]
        self.assertEqual(len(non_declare), 1)
        self.assertIn("app/Foo.kt", non_declare[0])

    def test_scope_absent_signale(self):
        erreurs = []
        check_couche0.check_scope([fichier("app/Foo.kt")], "sans section", erreurs)
        self.assertEqual(len(erreurs), 1)
        self.assertIn("aucune section", erreurs[0])

    def test_declaration_inutilisee_signalee(self):
        erreurs = []
        check_couche0.check_scope([fichier("app/Foo.kt")],
                                  "## Scope\n\n- `app/` — x\n- `docs/` — y\n",
                                  erreurs)
        self.assertEqual(len(erreurs), 1)
        self.assertIn("docs/", erreurs[0])

    def test_perimetre_juste_silencieux(self):
        erreurs = []
        check_couche0.check_scope([fichier("app/Foo.kt")],
                                  "## Scope\n\n- `app/` — x\n", erreurs)
        self.assertEqual(erreurs, [])


class ForbiddenTest(unittest.TestCase):
    """Motifs interdits dans les lignes ajoutées + gardes gelées."""

    def test_garde_sans_mention_refusee(self):
        for garde in (".github/workflows/ci.yml",
                      ".github/workflows/ci-native.yml",
                      ".github/workflows/pr-auto-merge.yml"):
            erreurs = []
            check_couche0.check_forbidden(
                [fichier(garde)], "## Scope\n\n- `%s` — x\n" % garde, erreurs)
            self.assertEqual(len(erreurs), 1, garde)
            self.assertIn(check_couche0.GUARD_FLAG, erreurs[0])

    def test_garde_avec_mention_acceptee(self):
        erreurs = []
        corps = "## Scope\n\n- `.github/` — x\n\n" + check_couche0.GUARD_FLAG
        check_couche0.check_forbidden(
            [fichier(".github/workflows/ci-native.yml")], corps, erreurs)
        self.assertEqual(erreurs, [])

    def test_date_ancienne_refusee(self):
        # Fragmenté exprès : la garde scanne les lignes ajoutées du diff,
        # le cas de test ne doit pas se faire refuser lui-même.
        vieux = "2026-10-2" + "4"
        erreurs = []
        check_couche0.check_forbidden(
            [fichier("docs/N.md", "+ relevé du " + vieux)], "corps", erreurs)
        self.assertEqual(len(erreurs), 1)

    def test_date_recente_acceptee(self):
        erreurs = []
        check_couche0.check_forbidden(
            [fichier("docs/N.md", "+ relevé du 2026-10-29")], "corps", erreurs)
        self.assertEqual(erreurs, [])

    def test_cle_api_refusee(self):
        fausse = "sk-" + "a" * 24
        erreurs = []
        check_couche0.check_forbidden(
            [fichier("app/K.kt", "+ cle = " + fausse)], "corps", erreurs)
        self.assertEqual(len(erreurs), 1)

    def test_garde_qui_se_cite_ignoree(self):
        # La garde est gelée elle-même : le corps porte la mention, et le
        # motif cité dans son propre code ne doit pas la faire refuser.
        fausse = "sk-" + "a" * 24
        erreurs = []
        check_couche0.check_forbidden(
            [fichier(".github/couche0/check_couche0.py",
                     "+ motif : " + fausse)],
            "corps " + check_couche0.GUARD_FLAG, erreurs)
        self.assertEqual(erreurs, [])


class SuggestionScopeTest(unittest.TestCase):
    """Le refus rend un bloc `## Scope` copiable (même règle, moins d'attente)."""

    def test_trois_fichiers_meme_dossier_devinent_un_glob(self):
        fichiers = [fichier("app/res/raw/vp_%d.mp3" % i) for i in range(38)]
        self.assertEqual(check_couche0.suggerer_scope(fichiers),
                         ["app/res/raw/*.mp3"])

    def test_deux_fichiers_restent_nommes(self):
        fichiers = [fichier("app/a.kt"), fichier("app/b.kt")]
        self.assertEqual(sorted(check_couche0.suggerer_scope(fichiers)),
                         ["app/a.kt", "app/b.kt"])

    def test_exemptes_absents_de_la_suggestion(self):
        fichiers = [fichier("app/build/x.class"), fichier("app/a.kt")]
        self.assertEqual(check_couche0.suggerer_scope(fichiers), ["app/a.kt"])

    def test_bloc_scope_collable(self):
        bloc = check_couche0.bloc_scope([fichier("core/net/Http.kt")])
        self.assertTrue(bloc.startswith("## Scope"))
        self.assertIn("- `core/net/Http.kt`", bloc)
        # Le garde doit accepter ce qu'il vient de suggérer.
        self.assertEqual(check_couche0.parse_scope(bloc), ["core/net/Http.kt"])

    def test_le_bloc_suggere_satisfait_le_controle(self):
        fichiers = [fichier("app/res/raw/vp_%d.mp3" % i) for i in range(5)]
        erreurs = []
        check_couche0.check_scope(fichiers, check_couche0.bloc_scope(fichiers), erreurs)
        self.assertEqual(erreurs, [])


if __name__ == "__main__":
    unittest.main()
