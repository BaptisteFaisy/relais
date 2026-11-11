#!/usr/bin/env python3
"""Tests ciblés de conflict_agent.py — bibliothèque standard uniquement.

Lance : python3 -m unittest test_conflict_agent -v
(depuis .github/inferhub/).
"""
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest import mock

import compilation_gate
import conflict_agent


class EnvOrTest(unittest.TestCase):
    """Résolution des réglages : une var GitHub absente arrive comme ""."""

    def test_var_absente_rend_defaut(self):
        with mock.patch.dict(os.environ):
            os.environ.pop("INFERHUB_MODEL", None)
            self.assertEqual(
                conflict_agent.env_or("INFERHUB_MODEL", "DEFAUT"), "DEFAUT")

    def test_var_vide_rend_defaut(self):
        # Régression 28/10 : modèle "" → 400 InferHub sur un vrai conflit.
        with mock.patch.dict(os.environ, {"INFERHUB_MODEL": ""}):
            self.assertEqual(
                conflict_agent.env_or("INFERHUB_MODEL", "DEFAUT"), "DEFAUT")

    def test_var_renseignee_gagne(self):
        with mock.patch.dict(os.environ, {"INFERHUB_MODEL": "M"}):
            self.assertEqual(
                conflict_agent.env_or("INFERHUB_MODEL", "DEFAUT"), "M")


class PushUrlTest(unittest.TestCase):
    """URL push : PAT du CI, repli "origin" en local."""

    def test_avec_token_rend_url_authentifiee(self):
        with mock.patch.dict(os.environ, {"GH_TOKEN": "T"}):
            url = conflict_agent.push_url("O/R")
            self.assertEqual(
                url, "https://x-access-token:T@github.com/O/R.git")

    def test_sans_token_rend_origin(self):
        with mock.patch.dict(os.environ):
            os.environ.pop("GH_TOKEN", None)
            self.assertEqual(conflict_agent.push_url("O/R"), "origin")


class SplitHunksTest(unittest.TestCase):
    """Découpage des marqueurs : le propre recopié, les bords nommés."""

    def test_sans_marqueur_tout_propre(self):
        segs = conflict_agent.split_hunks("a\nb\n")
        self.assertEqual(segs, [("clean", "a\nb\n")])

    def test_diff_simple(self):
        text = ("A\n<<<<<<< HEAD\nm1\n=======\nb1\n>>>>>>> br\nZ\n")
        segs = conflict_agent.split_hunks(text)
        self.assertEqual(len(segs), 3)
        self.assertEqual(segs[0], ("clean", "A"))
        kind, hunk = segs[1]
        self.assertEqual(kind, "hunk")
        self.assertEqual(hunk["head"], ["m1"])
        self.assertIsNone(hunk["base"])
        self.assertEqual(hunk["saved"], ["b1"])
        self.assertEqual(segs[2], ("clean", "Z\n"))

    def test_diff3_avec_base(self):
        text = ("<<<<<<< HEAD\nm\n||||||| base\nb\n=======\ns\n>>>>>>> br\n")
        segs = conflict_agent.split_hunks(text)
        self.assertEqual(len(segs), 2)
        kind, hunk = segs[0]
        self.assertEqual(hunk["head"], ["m"])
        self.assertEqual(hunk["base"], ["b"])
        self.assertEqual(hunk["saved"], ["s"])

    def test_rebase_reel_second_bloc_est_branche(self):
        # Non-régression 28/10 : en rebase, "saved" = branche rejouée.
        repo = tempfile.mkdtemp(prefix="fw-sides-")
        self.addCleanup(shutil.rmtree, repo, True)
        old = os.getcwd()
        self.addCleanup(os.chdir, old)
        os.chdir(repo)
        run = lambda *a: subprocess.run(  # noqa: E731
            ["git", *a], capture_output=True, text=True)
        run("init", "-q")
        run("config", "user.email", "t@t")
        run("config", "user.name", "t")
        with open("f.txt", "w") as fh:
            fh.write("base\n")
        run("add", ".")
        run("commit", "-qm", "base")
        main = run("branch", "--show-current").stdout.strip()
        run("checkout", "-qb", "br")
        with open("f.txt", "w") as fh:
            fh.write("BRANCHE\n")
        run("commit", "-qam", "br")
        run("checkout", "-q", main)
        with open("f.txt", "w") as fh:
            fh.write("MASTER\n")
        run("commit", "-qam", "m")
        run("checkout", "-q", "br")
        failed = subprocess.run(["git", "rebase", main],
                                capture_output=True)
        self.assertNotEqual(failed.returncode, 0)  # conflit attendu
        with open("f.txt") as fh:
            segs = conflict_agent.split_hunks(fh.read())
        hunks = [s for k, s in segs if k == "hunk"]
        self.assertEqual(len(hunks), 1)
        self.assertEqual(hunks[0]["saved"], ["BRANCHE"])
        self.assertEqual(hunks[0]["head"], ["MASTER"])


def _scratch_repo(test):
    """Dépôt git réel en temp, cwd dedans, nettoyé après. Rend run()."""
    repo = tempfile.mkdtemp(prefix="fw-loop-")
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


def _commit(run, files, message):
    for path, content in files.items():
        with open(path, "w") as fh:
            fh.write(content)
    run("add", ".")
    assert run("commit", "-qm", message).returncode == 0


class RebaseLoopTest(unittest.TestCase):
    """Boucle multi-passes : chaque arrêt est résolu, puis poussé."""

    def test_rebase_in_progress_suit_le_rebase(self):
        run = _scratch_repo(self)
        _commit(run, {"f.txt": "base\n"}, "base")
        base_branch = run("branch", "--show-current").stdout.strip()
        self.assertFalse(conflict_agent.rebase_in_progress())
        run("checkout", "-qb", "br")
        _commit(run, {"f.txt": "BR\n"}, "br")
        run("checkout", "-q", base_branch)
        _commit(run, {"f.txt": "M\n"}, "m")
        run("checkout", "-q", "br")
        failed = subprocess.run(["git", "rebase", base_branch],
                                capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertTrue(conflict_agent.rebase_in_progress())
        run("rebase", "--abort")
        self.assertFalse(conflict_agent.rebase_in_progress())

    def test_boucle_deux_arrets(self):
        # Non-régression 28/10 : #125 a deux commits en conflit, l'ancien
        # code échouait au premier --continue ("Resolve all conflicts").
        run = _scratch_repo(self)
        _commit(run, {"f.txt": "base\n", "g.txt": "base\n"}, "base")
        base_branch = run("branch", "--show-current").stdout.strip()
        run("checkout", "-qb", "br")
        _commit(run, {"f.txt": "BR-F\n"}, "br1")
        _commit(run, {"g.txt": "BR-G\n"}, "br2")
        run("checkout", "-q", base_branch)
        _commit(run, {"f.txt": "M-F\n"}, "m1")
        _commit(run, {"g.txt": "M-G\n"}, "m2")
        run("checkout", "-q", "br")
        failed = subprocess.run(["git", "rebase", base_branch],
                                capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        with mock.patch.object(conflict_agent, "ih_complete",
                               return_value="```\nRESOLU\n```"):
            resolved = conflict_agent.finish_loop(
                "http://x", "K", "M", time.time() + 60)
        self.assertEqual(sorted(resolved), ["f.txt", "g.txt"])
        self.assertFalse(conflict_agent.rebase_in_progress())
        log = run("log", "--format=%s").stdout
        self.assertIn("br1", log)
        self.assertIn("br2", log)

    def test_commit_vide_skip(self):
        # La résolution reprend le bord main : le commit rejoué est vide,
        # --continue échoue ("empty") et la boucle le saute.
        run = _scratch_repo(self)
        _commit(run, {"f.txt": "X\n"}, "base")
        base_branch = run("branch", "--show-current").stdout.strip()
        run("checkout", "-qb", "br")
        _commit(run, {"f.txt": "Y\n"}, "br")
        run("checkout", "-q", base_branch)
        _commit(run, {"f.txt": "Z\n"}, "m")
        run("checkout", "-q", "br")
        failed = subprocess.run(["git", "rebase", base_branch],
                                capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        with mock.patch.object(conflict_agent, "ih_complete",
                               return_value="```\nZ\n```"):
            conflict_agent.finish_loop("http://x", "K", "M",
                                       time.time() + 60)
        self.assertFalse(conflict_agent.rebase_in_progress())
        log = run("log", "--format=%s").stdout
        self.assertNotIn("br", log.splitlines())
        with open("f.txt") as fh:
            self.assertEqual(fh.read().strip(), "Z")

    def test_borne_max_rounds(self):
        # Un conflit qui ne part pas (résolution sans effet) abandonne
        # après MAX_ROUNDS au lieu de boucler.
        run = _scratch_repo(self)
        _commit(run, {"f.txt": "base\n"}, "base")
        base_branch = run("branch", "--show-current").stdout.strip()
        run("checkout", "-qb", "br")
        _commit(run, {"f.txt": "BR\n"}, "br")
        run("checkout", "-q", base_branch)
        _commit(run, {"f.txt": "M\n"}, "m")
        run("checkout", "-q", "br")
        failed = subprocess.run(["git", "rebase", base_branch],
                                capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        with mock.patch.object(conflict_agent, "resolve_one",
                               return_value=None):
            with self.assertRaises(conflict_agent.AgentError) as ctx:
                conflict_agent.finish_loop("http://x", "K", "M",
                                           time.time() + 60)
        self.assertIn("arrêts", str(ctx.exception))


class FinishPushTest(unittest.TestCase):
    """Fin de rebase : un seul push avec bail, puis compte rendu."""

    def test_un_seul_push(self):
        # Non-régression 29/10 : le push était exécuté deux fois avec le même
        # bail — le second échouait toujours après un premier succès, et la PR
        # repartait en « conflit » sans commentaire ni label.
        appels = []

        def faux_run(argv, **kw):
            appels.append(argv)
            return mock.Mock(returncode=0, stdout="", stderr="")

        with mock.patch.object(conflict_agent.subprocess, "run",
                               side_effect=faux_run):
            rc = conflict_agent.finish("O/R", "1", "br", "sha", ["f.txt"])
        self.assertEqual(rc, 0)
        pushes = [a for a in appels if a[:2] == ["git", "push"]]
        self.assertEqual(len(pushes), 1)
        self.assertIn("--force-with-lease=br:sha", pushes[0])

    def test_push_refuse_sans_compte_rendu_succes(self):
        def faux_run(argv, **kw):
            if argv[:2] == ["git", "push"]:
                return mock.Mock(returncode=1, stdout="", stderr="bail")
            return mock.Mock(returncode=0, stdout="", stderr="")

        with mock.patch.object(conflict_agent.subprocess, "run",
                               side_effect=faux_run):
            rc = conflict_agent.finish("O/R", "1", "br", "sha", ["f.txt"])
        self.assertEqual(rc, 1)


class DryRunStampTest(unittest.TestCase):
    """Le dry-run n'écrit jamais d'empreinte (ni cooldown ni plafond)."""

    def test_dry_run_sans_stamp(self):
        # Non-régression 29/10 : une simulation touchait l'empreinte avant la
        # branche dry-run, et bloquait un vrai run pendant 20 min.
        run = _scratch_repo(self)
        _commit(run, {"f.txt": "base\n"}, "base")
        base_branch = run("branch", "--show-current").stdout.strip()
        run("checkout", "-qb", "br")
        _commit(run, {"f.txt": "BR\n"}, "br")
        run("checkout", "-q", base_branch)
        _commit(run, {"f.txt": "M\n"}, "m")
        run("checkout", "-q", "br")
        failed = subprocess.run(["git", "rebase", base_branch],
                                capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        home = tempfile.mkdtemp(prefix="fw-home-")
        self.addCleanup(shutil.rmtree, home, True)
        args = mock.Mock(repo="O/R", pr="1", branch="", sha="abc",
                         dry_run=True)
        with mock.patch.dict(os.environ, {"HOME": home}), \
                mock.patch.object(conflict_agent, "pr_labels", return_value=[]):
            rc = conflict_agent.cmd_resolve(args)
        self.assertEqual(rc, 0)
        cache = os.path.join(home, ".cache", "inferhub-agent")
        stamps = os.listdir(cache) if os.path.isdir(cache) else []
        self.assertEqual(stamps, [])


class UnionSansModeleTest(unittest.TestCase):
    """Le câblage : un journal se règle sans appeler le modèle."""

    def test_finish_loop_union_sans_saisine(self):
        run = _scratch_repo(self)
        _commit(run, {"CHANGELOG.md": "# J\n\n## Ancien\n"}, "base")
        base_branch = run("branch", "--show-current").stdout.strip()
        run("checkout", "-qb", "br")
        _commit(run, {"CHANGELOG.md": "# J\n\n## B\n\n## Ancien\n"}, "br")
        run("checkout", "-q", base_branch)
        _commit(run, {"CHANGELOG.md": "# J\n\n## M\n\n## Ancien\n"}, "m")
        run("checkout", "-q", "br")
        failed = subprocess.run(["git", "rebase", base_branch],
                                capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        with mock.patch.object(conflict_agent, "ih_complete",
                               side_effect=AssertionError("modèle appelé")):
            resolved = conflict_agent.finish_loop(
                "http://x", "K", "M", time.time() + 60)
        self.assertEqual(resolved, ["CHANGELOG.md"])
        self.assertFalse(conflict_agent.rebase_in_progress())



class TachesCompilationTest(unittest.TestCase):
    """Quels modules la porte compile pour un diff donné (v3, 31/10)."""

    def test_docs_ne_compilent_rien(self):
        self.assertEqual(
            compilation_gate.taches_compilation(["docs/A.md", "CHANGELOG.md"]),
            ([], []))

    def test_core(self):
        self.assertEqual(
            compilation_gate.taches_compilation(["core/src/X.kt"]),
            ([":core:compileDebugKotlin"], []))

    def test_app_et_core_dedoublonnes(self):
        gradle, python = compilation_gate.taches_compilation(
            ["app/a/X.kt", "core/b/Y.kt", "app/c/X2.kt"])
        self.assertEqual(gradle, [":app:compileDebugKotlin",
                                  ":core:compileDebugKotlin"])
        self.assertEqual(python, [])

    def test_python(self):
        self.assertEqual(
            compilation_gate.taches_compilation(["server/backend/x.py"]),
            ([], ["server/backend/x.py"]))

    def test_kt_hors_module_gradle_ignore(self):
        # Aucun `.kt` hors `core/`/`app/` ne se compile seul : le compiler
        # demanderait un module qui n'existe pas.
        self.assertEqual(
            compilation_gate.taches_compilation(["tools/x.kt",
                                               "build.gradle.kts"]),
            ([], []))


MARQUEUR = "<!-- agent-conflit sha=abc123 -->\n\nConflit réconcilié."


class PorteCompilationTest(unittest.TestCase):
    """Une résolution qui ne compile pas n'atteint jamais la branche."""

    def test_desactivee_par_variable(self):
        with mock.patch.dict(os.environ, {"CONFLICT_COMPILE_GATE": "0"}):
            ok, message = compilation_gate.verifier_compilation(["core/a.kt"])
        self.assertTrue(ok)
        self.assertIn("désactivée", message)

    def test_sans_gradlew_ne_bloque_rien(self):
        # Un clone sans outillage (test local, ancien déploiement) ne doit pas
        # empêcher le rebase : la CI reste la preuve.
        tmp = tempfile.mkdtemp()
        ancien = os.getcwd()
        os.chdir(tmp)
        try:
            ok, message = compilation_gate.verifier_compilation(["core/a.kt"])
        finally:
            os.chdir(ancien)
            shutil.rmtree(tmp, ignore_errors=True)
        self.assertTrue(ok)
        self.assertIn("gradlew", message)

    def test_compilation_rouge_refusee_avec_extrait(self):
        tmp = tempfile.mkdtemp()
        ancien = os.getcwd()
        os.chdir(tmp)
        try:
            with open("gradlew", "w", encoding="utf-8") as fichier:
                fichier.write("#!/bin/sh\nexit 0\n")
            os.chmod("gradlew", 0o755)

            def faux_run(argv, **kw):
                if argv[0] == "./gradlew":
                    return mock.Mock(
                        returncode=1, stdout="",
                        stderr="e: file:///x.kt:12:3 Unresolved reference "
                               "'hushed'")
                return mock.Mock(returncode=0, stdout="", stderr="")

            with mock.patch.object(conflict_agent.subprocess, "run",
                                   side_effect=faux_run):
                ok, message = compilation_gate.verifier_compilation(
                    ["core/src/X.kt"])
        finally:
            os.chdir(ancien)
            shutil.rmtree(tmp, ignore_errors=True)
        self.assertFalse(ok)
        self.assertIn("Unresolved reference", message)

    def test_finish_ne_pousse_pas_un_arbre_casse(self):
        # Régression 31/10 (#615) : la réconciliation avait poussé une tête
        # qui ne compilait pas, le balayeur l'a classée « échec réel » et plus
        # aucun agent n'écrit de correctif de code → PR morte en silence.
        appels = []

        def faux_run(argv, **kw):
            appels.append(argv)
            return mock.Mock(returncode=0, stdout="", stderr="")

        with mock.patch.object(conflict_agent, "porte_compilation",
                               return_value=(False, "e: x", "")), \
                mock.patch.object(conflict_agent.subprocess, "run",
                                  side_effect=faux_run):
            rc = conflict_agent.finish("O/R", "1", "br", "sha", ["f.kt"])
        self.assertEqual(rc, 1)
        self.assertEqual([a for a in appels if a[:2] == ["git", "push"]], [])

    def test_marqueur_de_tete_dans_le_compte_rendu(self):
        # Le balayeur cherche `<!-- agent-conflit sha=<tête> -->` mot pour mot
        # pour dire sur un échec réel que la résolution est en cause.
        corps = []
        with mock.patch.object(conflict_agent, "porte_compilation",
                               return_value=(True, "ok", MARQUEUR)), \
                mock.patch.object(conflict_agent, "comment",
                                  side_effect=lambda r, p, b: corps.append(b)), \
                mock.patch.object(conflict_agent, "add_label"), \
                mock.patch.object(conflict_agent.subprocess, "run",
                                  return_value=mock.Mock(
                                      returncode=0, stdout="", stderr="")):
            rc = conflict_agent.finish("O/R", "1", "br", "sha", ["f.kt"])
        self.assertEqual(rc, 0)
        self.assertIn("<!-- agent-conflit sha=abc123 -->", corps[0])

    def test_prepare_push_porte_le_marqueur_de_tete(self):
        with mock.patch.object(compilation_gate, "verifier_compilation",
                               return_value=(True, "ok")), \
                mock.patch.object(compilation_gate, "tete_git",
                                  return_value="deadbeef"):
            ok, message, corps = compilation_gate.prepare_push(
                ["core/b.kt", "core/a.kt"], "br")
        self.assertTrue(ok)
        self.assertTrue(corps.startswith("<!-- agent-conflit sha=deadbeef -->"))
        self.assertIn("2 fichier(s) (core/a.kt, core/b.kt)", corps)


class PlafondFichiersTest(unittest.TestCase):
    """Plafond de fichiers par passe : 100 (était 8 avant le 01/11)."""

    def test_neuf_fichiers_couverts(self):
        # Non-régression : un conflit à >8 fichiers était refusé avant.
        fichiers = ["f%02d.txt" % i for i in range(9)]
        conflict_agent.check_round_files(fichiers)  # ne lève pas

    def test_cent_fichiers_couverts(self):
        fichiers = ["f%03d.txt" % i for i in range(100)]
        conflict_agent.check_round_files(fichiers)  # ne lève pas

    def test_cent_un_fichiers_refuses(self):
        fichiers = ["f%03d.txt" % i for i in range(101)]
        with self.assertRaises(conflict_agent.AgentError) as ctx:
            conflict_agent.check_round_files(fichiers)
        self.assertIn("plafond 100", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
