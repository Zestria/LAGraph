"""Cheap source/provenance regressions; no benchmark workloads are run."""

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

LAB = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LAB))
sys.path.insert(0, str(LAB / "tests"))
import run
import labstate
import make_profile_variant as profile


class ProfileSourceTests(unittest.TestCase):
    def test_current_and_frozen_baseline_generate(self):
        for source in (labstate.source('baseline'), next(LAB / r["source"] for r in labstate.registry().values() if r["sha256"] == "f3642839be04eee2d6411d3db9d6973c50c19f386d7e61f4d9ff5a926af0447b")):
            with self.subTest(source=source):
                original = source.read_text()
                generated = profile.instrument(original)
                self.assertIn(original.splitlines()[0], generated)
                self.assertIn("cfpq_prof_snapshot", generated)
                self.assertIn("GrB_Vector_wait(*reachable", generated)
                self.assertIn("prof->phase2_3_s", generated)
                self.assertEqual(source.read_text(), original)

    def test_changed_algorithm_anchor_fails_closed(self):
        source = labstate.source('baseline').read_text()
        with self.assertRaisesRegex(SystemExit, "anchor occurs 0 times"):
            profile.instrument(source.replace("LG_CLEAR_MSG;", "/* changed entry */"))

    def test_tuple_build_is_in_call_construction_profile(self):
        source = labstate.source('baseline').read_text()
        generated = profile.instrument(source)
        build = generated.index("GRB_TRY(GrB_Matrix_build_BOOL(M_call")
        phase_end = generated.index("prof->phase2_3_s +=")
        self.assertLess(build, phase_end)
        self.assertIn("prof->callconstr_s +=", generated[build:phase_end])

    def test_profile_control_uses_the_instrumented_input(self):
        # An unrelated variants/baseline.c must not shadow --baseline in profile mode.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = labstate.source('baseline')
            args = SimpleNamespace(
                baseline=source, config=root / "cases.csv", cfg_bench_dir=root,
                rounds=1, threads=0, source_size=1, chunks=1, profile=True,
                include_cfl_multsrc_adv=False, out=root / "out", force=False,
            )
            with (patch.object(run, "parse_args", return_value=args),
                  patch.object(run, "check_config", return_value=[]),
                  patch.object(run, "read_config_pairs", return_value=[]),
                  patch.object(run, "build_cfg_bench", return_value=root / "cfg_bench"),
                  patch.object(run.subprocess, "run"),
                  patch.object(run, "variant_source", side_effect=AssertionError("shadowed input")),
                  patch.object(run, "build_variant", return_value=root / "variant.so") as build,
                  patch.object(run, "write_provenance"),
                  patch.object(run, "run_profile", return_value=0)):
                self.assertEqual(run.main(), 0)
                self.assertEqual(build.call_args_list[0].args[1], source.resolve())


class InputManifestTests(unittest.TestCase):
    def fixture(self, root):
        for name in ("graph.g", "grammar.cnf", "grammar.rsm", "sources.result"):
            (root / name).write_text(name + "\n")
        config = root / "cases.csv"
        config.write_text("graph.g,grammar.cnf,-1,sources.result\n")
        return config

    def test_derived_rsm_is_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.fixture(root)
            self.assertIn(root / "grammar.rsm", run.check_config(config, root))
            (root / "grammar.rsm").unlink()
            with self.assertRaisesRegex(SystemExit, "grammar.rsm"):
                run.check_config(config, root)

    def test_absolute_grammar_and_deduplication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.fixture(root)
            config.write_text(config.read_text() + f"graph.g,{root / 'grammar.cnf'},-1\n")
            paths = run.config_input_paths(config, root)
            self.assertEqual(paths.count(root / "grammar.rsm"), 1)

    def test_rsm_contents_are_hashed_in_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.fixture(root)
            binary = root / "cfg_bench"
            binary.write_text("fixture executable\n")
            args = SimpleNamespace(
                lagraph=LAB.parents[2], cfg_bench_dir=root, include_cfl_multsrc_adv=False,
                compiler="cc", cflags="-O2", threads=0, hot_rounds=1,
                order="blocked", keep_going=False, jit_cache=None,
            )
            rsm = root / "grammar.rsm"
            previous = None
            with patch.object(run.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "fixture")):
                for contents in ("first RSM\n", "changed RSM\n"):
                    rsm.write_text(contents)
                    run.write_provenance(args, root, binary, config, {}, {},
                                         run.check_config(config, root))
                    manifest = (root / "inputs.sha256").read_text()
                    digest = hashlib.sha256(contents.encode()).hexdigest()
                    self.assertIn(digest, manifest)
                    self.assertIn(str(rsm), manifest)
                    self.assertIn(digest, (root / "provenance.txt").read_text())
                    self.assertNotEqual(previous, manifest)
                    previous = manifest


if __name__ == "__main__":
    unittest.main()
