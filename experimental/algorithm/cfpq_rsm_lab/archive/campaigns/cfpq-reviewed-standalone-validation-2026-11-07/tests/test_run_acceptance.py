#!/usr/bin/env python3
"""Harness fixtures for the DS-01-R2 runner acceptance checks.

These are synthetic in-memory fixtures: they do not run cfg_bench or the kernel.
They exercise every failure mode the R2 review asked for, plus output-directory
reuse handling.

Run: python3 -m unittest discover -s experimental/algorithm/cfpq_rsm_lab/tests -p "test_*.py"
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run  # noqa: E402


def row(count, hash_value, time_s=1.0, memory_kb=100.0):
    return {
        "result": str(count),
        "result_hash": str(hash_value),
        "time_s": str(time_s),
        "max_memory_kb": str(memory_kb),
    }


KEY_A = ("graph_a.g", "grammar_a.cnf", "1", "0")
KEY_B = ("graph_b.g", "grammar_b.cnf", "1", "0")


def clean_results(rounds=2):
    results = {"base": {}, "var": {}}
    for key, count, hash_value in ((KEY_A, 5, 11), (KEY_B, 7, 13)):
        for round_id in range(rounds):
            run.add_result_row(results, "base", key, round_id, row(count, hash_value))
            run.add_result_row(results, "var", key, round_id, row(count, hash_value, 1.0 + 0.1 * round_id))
    return results


class AcceptanceTests(unittest.TestCase):
    def check(self, results, expected=(KEY_A, KEY_B), rounds=2, baseline="base"):
        return run.accept_rows(results, list(expected), rounds, baseline)

    def test_clean_passes(self):
        problems, stats = self.check(clean_results())
        self.assertEqual(problems, [])
        self.assertEqual(stats[("var", KEY_A)]["status"], "ok")

    def test_clean_single_round(self):
        problems, stats = self.check(clean_results(rounds=1), rounds=1)
        self.assertEqual(problems, [])

    def test_missing_case(self):
        results = clean_results()
        del results["var"][KEY_B]
        problems, _ = self.check(results)
        self.assertTrue(any("missing case" in problem for problem in problems))

    def test_extra_case(self):
        results = clean_results()
        run.add_result_row(results, "var", ("graph_z.g", "grammar_z.cnf", "1", "0"), 0, row(1, 1))
        problems, _ = self.check(results)
        self.assertTrue(any("unexpected case" in problem for problem in problems))

    def test_missing_round(self):
        results = clean_results()
        del results["var"][KEY_A][1]
        problems, stats = self.check(results)
        self.assertTrue(any("has rounds" in problem for problem in problems))
        self.assertEqual(stats[("var", KEY_A)]["status"], "MISSING_ROUNDS")

    def test_zero_hash(self):
        results = clean_results()
        results["var"][KEY_A][1] = row(5, 0)
        problems, stats = self.check(results)
        self.assertTrue(any("zero/unavailable hash" in problem for problem in problems))
        self.assertEqual(stats[("var", KEY_A)]["status"], "ZERO_HASH")

    def test_count_only_mismatch(self):
        results = clean_results()
        for round_id in (0, 1):
            results["var"][KEY_A][round_id] = row(6, 11)
        problems, stats = self.check(results)
        self.assertTrue(any("count 6 != baseline 5" in problem for problem in problems))
        self.assertEqual(stats[("var", KEY_A)]["status"], "WRONG")

    def test_hash_only_mismatch(self):
        results = clean_results()
        for round_id in (0, 1):
            results["var"][KEY_A][round_id] = row(5, 12)
        problems, stats = self.check(results)
        self.assertTrue(any("hash 12 != baseline 11" in problem for problem in problems))
        self.assertEqual(stats[("var", KEY_A)]["status"], "WRONG")

    def test_instability(self):
        results = clean_results()
        results["var"][KEY_A][1] = row(5, 99, 1.2)
        problems, stats = self.check(results)
        self.assertTrue(any("unstable hashes" in problem for problem in problems))
        self.assertEqual(stats[("var", KEY_A)]["status"], "UNSTABLE")

    def test_baseline_missing_case(self):
        results = clean_results()
        del results["base"][KEY_B]
        problems, _ = self.check(results)
        self.assertTrue(any("base: missing case" in problem for problem in problems))

    def test_baseline_missing_round(self):
        results = clean_results()
        del results["base"][KEY_A][1]
        problems, _ = self.check(results)
        self.assertTrue(any("base: case" in problem and "missing" in problem for problem in problems))

    def test_malformed_row(self):
        results = clean_results()
        results["var"][KEY_A][0] = {"time_s": "1.0", "max_memory_kb": "10"}
        problems, _ = self.check(results)
        self.assertTrue(any("malformed" in problem for problem in problems))

    def test_duplicate_row_rejected(self):
        results = clean_results()
        with self.assertRaises(SystemExit):
            run.add_result_row(results, "var", KEY_A, 0, row(5, 11))

    def test_extra_round_detected(self):
        results = clean_results()
        run.add_result_row(results, "var", KEY_A, 2, row(5, 11))
        problems, stats = self.check(results)
        self.assertTrue(any("has rounds" in problem for problem in problems))
        self.assertEqual(stats[("var", KEY_A)]["status"], "MISSING_ROUNDS")


class OutputDirectoryTests(unittest.TestCase):
    def test_fresh_directory_ok(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "run"
            run.prepare_output_dir(out, force=False)
            self.assertTrue(out.is_dir())

    def test_nonempty_rejected_without_force(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "run"
            out.mkdir()
            (out / "summary.csv").write_text("old\n")
            (out / "notes.txt").write_text("unrelated\n")
            with self.assertRaises(SystemExit):
                run.prepare_output_dir(out, force=False)
            self.assertTrue((out / "summary.csv").exists())

    def test_force_clears_only_run_owned(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "run"
            out.mkdir()
            (out / "summary.csv").write_text("old\n")
            (out / "profile").mkdir()
            (out / "profile" / "profile-x.txt").write_text("old\n")
            (out / "notes.txt").write_text("keep me\n")
            run.prepare_output_dir(out, force=True)
            self.assertFalse((out / "summary.csv").exists())
            self.assertFalse((out / "profile").exists())
            self.assertTrue((out / "notes.txt").exists())


if __name__ == "__main__":
    unittest.main()
