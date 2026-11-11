#!/usr/bin/env python3
"""Tests ciblés de scope_agent.py — bibliothèque standard uniquement.

Lance : python3 -m unittest test_scope_agent -v
(depuis .github/fireworks/).
"""
import importlib.util
import os
import unittest
from unittest import mock

import scope_agent

GARDE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "couche0", "check_couche0.py")


def garde():
    spec = importlib.util.spec_from_file_location("couche0", GARDE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fichier(chemin, ajout="+ligne"):
    return {"filename": chemin, "patch": "@@ -1 +1 @@\n-%s\n%s" % (chemin, ajout)}


class ReponseTest(unittest.TestCase):
    """Lecture des puces rendues par le modèle."""

    def test_deux_puces(self):
        texte = ("- `app/A.kt` — ajoute l'écran\n"
                 "- `docs/X.md` — documente le choix\n")
        self.assertEqual(scope_agent.raisons_repondue(texte),
                         {"app/A.kt": "ajoute l'écran",
                          "docs/X.md": "documente le choix"})

    def test_ignore_les_phrases_autour(self):
        texte = ("Voici la section :\n\n"
                 "## Scope\n- `core/B.kt` — corrige le calcul\n\nVoilà.\n")
        self.assertEqual(scope_agent.raisons_repondue(texte),
                         {"core/B.kt": "corrige le calcul"})

    def test_tiret_long_et_espace(self):
        self.assertEqual(scope_agent.raisons_repondue("- `a/b.kt` — x"),
                         {"a/b.kt": "x"})


class RecomposerTest(unittest.TestCase):
    """Le corps recomposé doit passer le garde, ou ne pas être écrit."""

    def setUp(self):
        self.g = garde()

    def test_section_absente(self):
        fichiers = [fichier("app/A.kt"), fichier("core/B.kt")]
        corps = "## Problème\n\nRien ne marche.\n"
        nouveau, ajouts, retires = scope_agent.recomposer(
            corps, fichiers, {"app/A.kt": "corrige A", "core/B.kt": "corrige B"},
            self.g)
        self.assertEqual(sorted(a.split("`")[1] for a in ajouts),
                         ["app/A.kt", "core/B.kt"])
        self.assertEqual(retires, [])
        erreurs = []
        self.g.check_scope(fichiers, nouveau, erreurs)
        self.assertEqual(erreurs, [])
        self.assertIn("## Problème", nouveau)          # prose conservée

    def test_exempte_non_declaree(self):
        fichiers = [fichier("app/A.kt"),
                    fichier("app/build/sortie.bin"),
                    fichier("gradle/wrapper/x.jar")]
        nouveau, ajouts, _ = scope_agent.recomposer(
            "", fichiers, {"app/A.kt": "corrige A"}, self.g)
        self.assertEqual([a.split("`")[1] for a in ajouts], ["app/A.kt"])
        erreurs = []
        self.g.check_scope(fichiers, nouveau, erreurs)
        self.assertEqual(erreurs, [])

    def test_garde_les_puces_valides_et_ajoute_les_manquantes(self):
        fichiers = [fichier("app/A.kt"), fichier("core/B.kt")]
        corps = ("## Scope\n"
                 "- `app/A.kt` — raison de l'auteur\n"
                 "- `vieux/D.kt` — puce morte\n"
                 "\n## Preuves\n- 11/11\n")
        nouveau, ajouts, retires = scope_agent.recomposer(
            corps, fichiers, {"app/A.kt": "jamais utilisée",
                              "core/B.kt": "corrige B"}, self.g)
        self.assertEqual([a.split("`")[1] for a in ajouts], ["core/B.kt"])
        self.assertEqual(retires, ["vieux/D.kt"])
        self.assertIn("raison de l'auteur", nouveau)   # puce de l'auteur intacte
        self.assertIn("## Preuves", nouveau)
        erreurs = []
        self.g.check_scope(fichiers, nouveau, erreurs)
        self.assertEqual(erreurs, [])


class MessageTest(unittest.TestCase):
    """Le prompt contient chaque fichier une fois, avec un extrait du diff."""

    def test_un_bloc_par_fichier(self):
        fichiers = [fichier("app/A.kt", "+val x = 1"),
                    fichier("core/B.kt", "+val y = 2")]
        messages = scope_agent.messages_pour(
            {"title": "t", "body": "b"}, fichiers, ["app/A.kt", "core/B.kt"])
        contenu = messages[1]["content"]
        self.assertEqual(contenu.count("### app/A.kt"), 1)
        self.assertIn("val x = 1", contenu)


class RapprocherTest(unittest.TestCase):
    """Les puces rendues sont rapprochées des fichiers avec la règle du garde.

    Non-régression 29/10 (#215) : le modèle rendait un glob là où le script
    exigeait l'égalité exacte → la puce juste comptait comme absente et la PR
    restait bloquée avec « le modèle n'a rien rendu ».
    """

    def setUp(self):
        self.g = garde()

    def test_chemin_exact(self):
        couverts, manquants = scope_agent.rapprocher(
            {"app/A.kt": "corrige A"}, ["app/A.kt"], self.g)
        self.assertEqual(couverts, {"app/A.kt": "corrige A"})
        self.assertEqual(manquants, [])

    def test_glob_du_modele(self):
        raisons = {"app/src/main/java/*/app/*Settings*.kt": "lit le lien"}
        couverts, manquants = scope_agent.rapprocher(
            raisons,
            ["app/src/main/java/dev/sam3vision/app/SettingsScreen.kt"], self.g)
        self.assertEqual(manquants, [])
        self.assertEqual(couverts["app/src/main/java/dev/sam3vision/app/"
                                  "SettingsScreen.kt"], "lit le lien")

    def test_dossier(self):
        couverts, manquants = scope_agent.rapprocher(
            {"docs/": "documente"}, ["docs/CI-QUEUE.md"], self.g)
        self.assertEqual(manquants, [])
        self.assertIn("docs/CI-QUEUE.md", couverts)

    def test_casse_approximative_du_modele(self):
        # Vu sur #215 le 29/10 : le modèle a rendu `sam3Vision` (V majuscule)
        # pour `sam3vision` → la puce juste comptait comme absente.
        raisons = {"app/src/main/java/dev/sam3Vision/app/ParentShareSettings.kt":
                   "ajoute la section Réglages"}
        couverts, manquants = scope_agent.rapprocher(
            raisons, ["app/src/main/java/dev/sam3vision/app/"
                      "ParentShareSettings.kt"], self.g)
        self.assertEqual(manquants, [])
        self.assertEqual(couverts, {"app/src/main/java/dev/sam3vision/app/"
                                    "ParentShareSettings.kt":
                                    "ajoute la section Réglages"})

    def test_casse_et_glob(self):
        raisons = {"APP/**/Settings*.kt": "lit le lien"}
        _, manquants = scope_agent.rapprocher(
            raisons, ["app/src/main/java/dev/sam3vision/app/SettingsScreen.kt"],
            self.g)
        self.assertEqual(manquants, [])

    def test_manquant_reel(self):
        _, manquants = scope_agent.rapprocher(
            {"app/A.kt": "x"}, ["app/A.kt", "app/B.kt"], self.g)
        self.assertEqual(manquants, ["app/B.kt"])


class RecomposerRecalcitrantTest(unittest.TestCase):
    """Les lignes que le garde lira comme déclarations sont traitées comme telles.

    Non-régression 29/10 (#263) : `- `Parler`` dans la section Scope était
    gardé comme « prose » (PUCE exige une raison) puis compté par le garde
    comme périmètre surdimensionné — corps refusé.
    """

    def setUp(self):
        self.g = garde()

    def test_puce_sans_raison_retiree(self):
        fichiers = [fichier("app/A.kt")]
        corps = "## Scope\n- `Parler`\n- `app/B.kt` — autre chose\n"
        nouveau, ajouts, retires = scope_agent.recomposer(
            corps, fichiers, {"app/A.kt": "corrige A"}, self.g)
        verif = []
        self.g.check_scope(fichiers, nouveau, verif)
        self.assertEqual(verif, [])
        self.assertIn("Parler", retires)
        self.assertNotIn("Parler", nouveau)
        self.assertEqual(len(ajouts), 1)

    def test_puce_utile_conservee(self):
        fichiers = [fichier("app/A.kt")]
        corps = "## Scope\n- `app/A.kt` — corrige A\n"
        nouveau, ajouts, _ = scope_agent.recomposer(
            corps, fichiers, {"app/A.kt": "doublon"}, self.g)
        self.assertEqual(ajouts, [])
        self.assertIn("- `app/A.kt` — corrige A", nouveau)

    def test_prose_vraie_intacte(self):
        fichiers = [fichier("app/A.kt")]
        corps = "## Scope\nVoir aussi le ticket 42.\n"
        nouveau, _, _ = scope_agent.recomposer(
            corps, fichiers, {"app/A.kt": "corrige A"}, self.g)
        self.assertIn("Voir aussi le ticket 42.", nouveau)


class RelanceTest(unittest.TestCase):
    """Une omission du modèle déclenche une relance ciblée, pas un abandon."""

    def test_second_appel_limite_aux_manquants(self):
        fichiers = [fichier("app/A.kt"), fichier("app/B.kt")]
        prompts = []

        def faux_complete(api_url, key, model, messages):
            prompts.append(messages[1]["content"])
            if len(prompts) == 1:
                return "- `app/A.kt` — corrige A"
            return "- `app/B.kt` — corrige B"

        args = mock.Mock(repo="O/R", pr="215", sha="abc", dry_run=False,
                         guard=GARDE)
        ecrits = []
        with mock.patch.object(scope_agent, "deja_tente", return_value=False), \
                mock.patch.object(scope_agent, "verifier_reglages",
                                  return_value=("K", "M", "http://x")), \
                mock.patch.object(scope_agent, "titre_pr", return_value="T"), \
                mock.patch.object(scope_agent, "contexte",
                                  return_value=("Corps sans scope.", fichiers,
                                                ["app/A.kt", "app/B.kt"])), \
                mock.patch.object(scope_agent, "fw_complete", faux_complete), \
                mock.patch.object(scope_agent, "ecrire_corps",
                                  side_effect=lambda r, p, c: ecrits.append(c)), \
                mock.patch.object(scope_agent, "rendre_compte"):
            self.assertEqual(scope_agent.cmd_fix(args), 0)
        self.assertEqual(len(prompts), 2)
        self.assertIn("app/B.kt", prompts[1])
        self.assertNotIn("app/A.kt", prompts[1])
        self.assertIn("app/B.kt", ecrits[0])


class PuceToleranteTest(unittest.TestCase):
    """Formats que le modèle rend réellement (listes numérotées, ./)."""

    def test_liste_numerotee(self):
        self.assertEqual(scope_agent.raisons_repondue("1. app/A.kt — x"),
                         {"app/A.kt": "x"})

    def test_chemin_relatif(self):
        self.assertEqual(scope_agent.raisons_repondue("- `./app/A.kt` — x"),
                         {"app/A.kt": "x"})

    def test_deux_points(self):
        self.assertEqual(scope_agent.raisons_repondue("- `app/A.kt` : x"),
                         {"app/A.kt": "x"})


class InvocationSweepTest(unittest.TestCase):
    """Le sweep doit appeler l'agent avec le PAT, pas le token bot.

    Non-régression v27 (31/10) : l'édition du corps avec le token de
    l'installation ne déclenche aucun workflow (anti-boucle GitHub), donc
    `couche0` ne rejugeait jamais le corps corrigé et la PR restait BLOCKED
    sur un verdict périmé (#688). Le sweep invoque `scope_agent` via
    `fw-scope.py` : cette invocation doit porter `GH_TOKEN="$GH_ECRITURE"`.
    """

    SWEEP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "..", "workflows", "pr-auto-merge.yml")

    def test_scope_agent_appele_avec_pat(self):
        with open(self.SWEEP, encoding="utf-8") as fh:
            contenu = fh.read()
        invocations = [l for l in contenu.splitlines()
                       if "fw-scope.py" in l and "python3" in l]
        self.assertTrue(invocations,
                        "aucune invocation de fw-scope.py dans pr-auto-merge.yml")
        for ligne in invocations:
            self.assertIn('GH_TOKEN="$GH_ECRITURE"', ligne,
                          "l'agent de périmètre doit écrire avec le PAT : %s"
                          % ligne.strip())

    def test_filet_verdict_perime(self):
        with open(self.SWEEP, encoding="utf-8") as fh:
            contenu = fh.read()
        self.assertIn("relancer_couche0", contenu,
                      "le filet de relance couche0 a disparu du sweep")
        self.assertIn("automerge-scope-rerun", contenu,
                      "la marque de relance couche0 a disparu du sweep")


if __name__ == "__main__":
    unittest.main()