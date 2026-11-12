#!/usr/bin/env python3
"""Unit tests for the mutation operators and the mutation-score engine.

A tiny C project is fabricated in a tmpdir so the tests do not depend on the
Relais core, the build system, or the compiler flags of the repository.
"""
import os
import tempfile
import unittest

import mutation_ops
from mutation_ops import Mutation, generate, select, apply_mutation
import mutation_score
from mutation_score import Project, prepare_nominal, read_sources, _evaluate


def write(db, name, text):
    path = os.path.join(db, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    return path


MINI_SRC = (
    "int is_zero(int x) { return x == 0; }\n"
    "int clamp_lo(int x) { return x < 0 ? 0 : x; }\n"
    "int dead(void) { int a = 1 + 1; return a - 2; }\n"
    "int banner(void) { return 7; }\n"
)

MINI_TEST = (
    '#include <stdio.h>\n'
    "int is_zero(int x);\n"
    "int clamp_lo(int x);\n"
    "int banner(void);\n"
    "int main(void) {\n"
    "    if (is_zero(0) != 1) return 1;\n"
    "    if (is_zero(1) != 0) return 2;\n"
    "    if (clamp_lo(-5) != 0) return 3;\n"
    "    if (clamp_lo(5) != 5) return 4;\n"
    '    printf("OK 4 checks banner=%d\\n", banner());\n'
    "    return 0;\n"
    "}\n"
)


def build_mini(root):
    write(root, "src/mini.c", MINI_SRC)
    write(root, "tests/test_mini.c", MINI_TEST)
    return Project(
        name="mini",
        sources=["src/mini.c"],
        build=["gcc", "-std=c11", "-O2", "src/mini.c", "tests/test_mini.c",
               "-o", "test_mini"],
        tests=[["./test_mini"]],
        scenarios=[],
        root=root,
    )


class OperatorTests(unittest.TestCase):
    def test_operators_are_applied(self):
        muts = generate({
            "m.c": [
                "int f(int x){ return x < 0 ? -1 : x; }",
                "if (a && b || c == 7) return k;",
            ],
        })
        seen = {(m.operator, m.before, m.after) for m in muts}
        for triple in [
            ("cmp", "<", "<="),        # comparison flip
            ("cmp", "==", "!="),       # equality flip
            ("logic", "&&", "||"),     # logical swap
            ("logic", "||", "&&"),
            ("const", "1", "2"),       # constant +1
            ("const", "1", "0"),       # constant -1
            ("const", "7", "8"),
            ("bound", "0", "1"),       # off-by-one on a bound
            ("sign", "-", "+"),        # unary minus flipped
        ]:
            self.assertIn(triple, seen, "missing operator %r" % (triple,))

    def test_select_is_capped_and_deterministic(self):
        muts = generate({
            "a.c": ["int a(int x){ return x < 0 ? -1 : x; }"],
            "b.c": ["int b(int x){ return x == 0; }"],
            "c.c": ["int c(int x){ return -x; }"],
        })
        five = select(muts, 5)
        self.assertEqual(len(five), 5)
        self.assertEqual([m._asdict() for m in five],
                         [m._asdict() for m in select(muts, 5)])
        # a capped run must still touch more than one file
        self.assertGreater(len({m.file for m in five}), 1)

    def test_apply_mutation(self):
        root = tempfile.mkdtemp()
        path = write(root, "m.c", "int f(int x){ return x == 0; }\n")
        line = "int f(int x){ return x == 0; }\n"
        col = line.index("==")
        apply_mutation(path, Mutation("m.c", 1, col, "cmp", "==", "!="))
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("return x != 0;", text)
        self.assertNotIn("return x == 0;", text)

    def test_apply_anchor_mismatch_raises(self):
        root = tempfile.mkdtemp()
        path = write(root, "m.c", "int f(int x){ return x == 0; }\n")
        with self.assertRaises(ValueError):
            apply_mutation(path, Mutation("m.c", 1, 99, "cmp", "==", "!="))


class EngineTests(unittest.TestCase):
    def _mini(self):
        root = tempfile.mkdtemp()
        return build_mini(root), root

    def test_killed_survived_equivalent_invalid(self):
        project, _root = self._mini()
        template, nominal_hash, _detail = prepare_nominal(project, 1)
        try:
            muts = generate(read_sources(project.sources, project.root))
            line1 = "int is_zero(int x) { return x == 0; }"
            killed = Mutation("src/mini.c", 1, line1.index("=="), "cmp",
                              "==", "!=")
            # any literal inside dead(): observable behaviour is unchanged
            line3 = "int dead(void) { int a = 1 + 1; return a - 2; }"
            equiv = Mutation("src/mini.c", 3, line3.index("1"), "const",
                             "1", "2")
            # banner() is printed but never asserted: output differs, test passes
            line4 = "int banner(void) { return 7; }"
            survived = Mutation("src/mini.c", 4, line4.index("7"), "const",
                                "7", "8")
            invalid = Mutation("src/mini.c", 3, line3.index("1"), "const",
                               "1", "1 2 3")
            self.assertEqual(_evaluate((project, template, killed,
                                        nominal_hash, 1))["status"], "killed")
            self.assertEqual(_evaluate((project, template, equiv,
                                        nominal_hash, 1))["status"],
                             "equivalent")
            self.assertEqual(_evaluate((project, template, survived,
                                        nominal_hash, 1))["status"],
                             "survived")
            self.assertEqual(_evaluate((project, template, invalid,
                                        nominal_hash, 1))["status"], "invalid")
        finally:
            import shutil
            shutil.rmtree(template, ignore_errors=True)

    def test_time_budget_stops_early(self):
        project, root = self._mini()
        muts = generate(read_sources(project.sources, project.root))
        many = (muts * 40)[:60]
        rows, partial = mutation_score.run_score(project, many, jobs=1,
                                                 time_budget=1)
        self.assertTrue(partial)
        self.assertLess(len(rows), len(many))


if __name__ == "__main__":
    unittest.main()