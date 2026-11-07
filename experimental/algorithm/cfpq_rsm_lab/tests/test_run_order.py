#!/usr/bin/env python3
"""Synthetic fixtures for the R11 blocked schedule, keep-going and combined summary.

These do not run real graphs, cfg_bench or the kernel.  Fixture parameters use
one or two cases/rounds; production requirements stay seven workloads, 30
chunks and five measured rounds.

Run: python3 -m unittest discover -s experimental/algorithm/cfpq_rsm_lab/tests -p "test_run_order.py"
"""

import contextlib
import csv
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

LAB_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LAB_DIR))
import run  # noqa: E402
import summarize_s100_selected as summarize  # noqa: E402
import summarize_current_vs_bb7 as summarize_current  # noqa: E402

SELECTED = (
    "ds36_pair",
    "ds38_no_product_clear",
    "ds39_direct_calls",
    "ds32_empty",
    "d1_native",
    "config_block",
)


def selected_displays():
    return ([("baseline", "baseline")]
            + [(name, name) for name in SELECTED]
            + [("baseline#2", "baseline")])


class ScheduleTests(unittest.TestCase):
    def test_blocked_eight_display_five_round_schedule(self):
        displays = selected_displays()
        jobs = run.build_timing_schedule(displays, 5, "blocked")
        self.assertEqual(len(jobs), 40)
        self.assertEqual(jobs[:5], [(r, "baseline", "baseline") for r in range(5)])
        self.assertEqual(jobs[-5:], [(r, "baseline#2", "baseline") for r in range(5)])
        # every display is contiguous and completes rounds 0..4 once
        for display, spec in displays:
            subset = [job for job in jobs if job[1] == display]
            self.assertEqual(subset, [(r, display, spec) for r in range(5)])
            self.assertEqual(sum(1 for round_id, _, _ in subset if round_id == 0), 1)
        # the six candidates occupy the middle 30 jobs in supplied order
        candidate_displays = [display for _, display, _ in jobs[5:35]]
        self.assertEqual(candidate_displays, [name for name in SELECTED for _ in range(5)])
        # warm-up applies only to each display's round 0
        warmups = [(display, round_id) for round_id, display, _ in jobs if round_id == 0]
        self.assertEqual(len(warmups), len(displays))
        self.assertEqual({display for display, _ in warmups}, {d for d, _ in displays})

    def test_rotated_schedule_unchanged(self):
        displays = selected_displays()
        expected = []
        for round_id in range(5):
            offset = round_id % len(displays)
            for display, spec in displays[offset:] + displays[:offset]:
                expected.append((round_id, display, spec))
        self.assertEqual(run.build_timing_schedule(displays, 5, "rotated"), expected)
        self.assertNotEqual(run.build_timing_schedule(displays, 5, "rotated"),
                            run.build_timing_schedule(displays, 5, "blocked"))

    def test_runner_defaults(self):
        with patch.object(sys, "argv", ["run.py"]):
            args = run.parse_args()
        self.assertEqual(args.order, "rotated")
        self.assertFalse(args.keep_going)


STUB_CFG_BENCH = r'''#!/usr/bin/env python3
import argparse
import csv
import os
import sys
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--results-file", required=True)
parser.add_argument("--source-sizes", default="1")
parser.add_argument("--chunks", default="1")
parser.add_argument("--experiment", action="store_true")
parser.add_argument("--hot", action="store_true")
parser.add_argument("-c", "--config")
parser.add_argument("-a")
parser.add_argument("--rsm-lib")
parser.add_argument("-r")
parser.add_argument("--seed")
args, _ = parser.parse_known_args()

path = Path(args.results_file)
display = path.parent.name
round_id = int(path.stem[len("round"):])
if display == os.environ.get("STUB_FAIL_DISPLAY") and round_id == 0:
    print("synthetic child failure", file=sys.stderr)
    sys.exit(3)

path.parent.mkdir(parents=True, exist_ok=True)
with open(path, "w", newline="") as file:
    writer = csv.writer(file)
    writer.writerow(["algo_name", "graph", "cnf", "source_size", "chunk",
                     "result", "result_hash", "time_s", "max_memory_kb"])
    for chunk in range(int(args.chunks)):
        writer.writerow(["CFL_CFPQ_RSM", "g.g", "c.cnf", str(args.source_sizes),
                         str(chunk), "5", "11", "1.0", "100.0"])
'''


class KeepGoingTests(unittest.TestCase):
    def test_keep_going_continues_after_one_failed_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stub = root / "stub_cfg_bench.py"
            stub.write_text(STUB_CFG_BENCH)
            stub.chmod(0o755)
            config = root / "lab.csv"
            config.write_text("g.g,c.cnf,-1\n")
            out = root / "out"
            displays = selected_displays()
            libraries = {spec: root / f"{spec}.so" for _, spec in displays}
            args = SimpleNamespace(
                out=out, config=config, cfg_bench_dir=root, chunks=1, rounds=5,
                seed=2026, source_size=1, threads=0, hot_rounds=1,
                keep_going=True, order="blocked", jit_cache=None,
                baseline_variant="baseline",
            )
            os.environ["STUB_FAIL_DISPLAY"] = "ds39_direct_calls"
            try:
                with contextlib.redirect_stdout(io.StringIO()), \
                        contextlib.redirect_stderr(io.StringIO()):
                    rc = run.run_timing(args, stub, [("g.g", "c.cnf")],
                                        libraries, displays)
            finally:
                os.environ.pop("STUB_FAIL_DISPLAY", None)

            self.assertNotEqual(rc, 0)
            status_path = out / "execution_status.tsv"
            self.assertTrue(status_path.is_file())
            with open(status_path, newline="") as file:
                statuses = list(csv.DictReader(file, delimiter="\t"))
            self.assertEqual(len(statuses), 40)
            expected_order = [display for display, _ in displays for _ in range(5)]
            self.assertEqual([row["display"] for row in statuses], expected_order)
            failed = [row for row in statuses if row["outcome"] != "ok"]
            self.assertEqual(len(failed), 1)
            self.assertEqual(failed[0]["display"], "ds39_direct_calls")
            self.assertEqual(failed[0]["round"], "0")
            self.assertEqual(failed[0]["exit_code"], "3")
            self.assertEqual(failed[0]["outcome"], "child_failed")
            # later candidates and the closing baseline still ran
            later = [row for row in statuses if row["display"] in
                     ("ds32_empty", "d1_native", "config_block", "baseline#2")]
            self.assertEqual(len(later), 20)
            self.assertTrue(all(row["outcome"] == "ok" for row in later))
            self.assertEqual(statuses[-1]["display"], "baseline#2")
            self.assertEqual(statuses[-1]["round"], "4")
            self.assertEqual(statuses[-1]["exit_code"], "0")
            self.assertTrue((out / "summary.csv").is_file())

    def test_missing_closing_baseline_does_not_raise_keyerror(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stub = root / "stub_cfg_bench.py"
            stub.write_text(STUB_CFG_BENCH)
            stub.chmod(0o755)
            config = root / "lab.csv"
            config.write_text("g.g,c.cnf,-1\n")
            out = root / "out"
            displays = [("baseline", "baseline"), ("cand1", "cand1"),
                        ("baseline#2", "baseline")]
            libraries = {spec: root / f"{spec}.so" for _, spec in displays}
            args = SimpleNamespace(
                out=out, config=config, cfg_bench_dir=root, chunks=1, rounds=2,
                seed=2026, source_size=1, threads=0, hot_rounds=1,
                keep_going=True, order="blocked", jit_cache=None,
                baseline_variant="baseline",
            )
            os.environ["STUB_FAIL_DISPLAY"] = "baseline_2"
            try:
                with contextlib.redirect_stdout(io.StringIO()), \
                        contextlib.redirect_stderr(io.StringIO()):
                    rc = run.run_timing(args, stub, [("g.g", "c.cnf")],
                                        libraries, displays)
            finally:
                os.environ.pop("STUB_FAIL_DISPLAY", None)
            self.assertNotEqual(rc, 0)
            self.assertTrue((out / "summary.csv").is_file())
            with open(out / "execution_status.tsv", newline="") as file:
                statuses = list(csv.DictReader(file, delimiter="\t"))
            self.assertEqual(len(statuses), 6)
            self.assertEqual(statuses[-2]["display"], "baseline#2")
            self.assertEqual(statuses[-2]["outcome"], "child_failed")


def write_raw(root: Path, display: str, chunks: int, rounds: int, *,
              count: int = 5, hash_value: int = 11, time_s: float = 1.0,
              memory_kb: float = 100.0, skip_rounds: tuple[int, ...] = ()) -> None:
    directory = root / "raw" / summarize.safe_name(display)
    directory.mkdir(parents=True, exist_ok=True)
    for round_id in range(rounds):
        if round_id in skip_rounds:
            continue
        path = directory / f"round{round_id}.csv"
        with open(path, "w", newline="") as file:
            writer = csv.writer(file)
            writer.writerow(["algo_name", "graph", "cnf", "source_size", "chunk",
                             "result", "result_hash", "time_s", "max_memory_kb"])
            for chunk in range(chunks):
                writer.writerow(["CFL_CFPQ_RSM", "g.g", "c.cnf", "100", str(chunk),
                                 str(count), str(hash_value), str(time_s),
                                 str(memory_kb)])


def write_summary(root: Path, variants, chunks: int, rounds: int, *,
                  status: str = "ok", count_ok: int = 1, hash_ok: int = 1) -> None:
    path = root / "summary.csv"
    exists = path.is_file()
    with open(path, "a", newline="") as file:
        writer = csv.DictWriter(
            file, fieldnames=["variant", "graph", "cnf", "source_size", "chunk",
                              "rounds_seen", "n_runs", "status", "count_ok", "hash_ok",
                              "result", "result_hash", "median_memory_kb"],
            lineterminator="\n")
        if not exists:
            writer.writeheader()
        for variant in variants:
            for chunk in range(chunks):
                writer.writerow({
                    "variant": variant,
                    "graph": "g.g",
                    "cnf": "c.cnf",
                    "source_size": "100",
                    "chunk": str(chunk),
                    "rounds_seen": str(rounds),
                    "n_runs": str(rounds),
                    "status": status,
                    "count_ok": str(count_ok),
                    "hash_ok": str(hash_ok),
                    "result": "5",
                    "result_hash": "11",
                    "median_memory_kb": "100.0",
                })


def write_execution(root: Path, displays, rounds: int,
                    failures: dict[str, tuple[str, int]] | None = None) -> None:
    failures = failures or {}
    path = root / "execution_status.tsv"
    with open(path, "w", newline="") as file:
        writer = csv.writer(file, delimiter="\t", lineterminator="\n")
        writer.writerow(["display", "spec", "round", "start", "finish", "exit_code",
                         "outcome", "log"])
        for display, spec in displays:
            for round_id in range(rounds):
                outcome, exit_code = failures.get(display, ("ok", 0))
                writer.writerow([display, spec, round_id, "start", "finish",
                                 exit_code, outcome,
                                 str(root / "logs" / summarize.safe_name(display)
                                     / f"round{round_id}.log")])


class CombinedSummaryTests(unittest.TestCase):
    def test_complete_failed_wrong_and_missing_candidates(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidates = ("cand_good", "cand_bad", "cand_wrong", "cand_missing")
            chunks, rounds = 2, 2
            displays = (("baseline", "baseline"),) + tuple(
                (name, name) for name in candidates) + (("baseline#2", "baseline"),)

            # opening and closing baselines agree on counts/hashes; times drift
            write_raw(root, "baseline", chunks, rounds, time_s=1.0)
            write_raw(root, "baseline#2", chunks, rounds, time_s=1.1)
            write_raw(root, "cand_good", chunks, rounds, time_s=0.9)
            write_raw(root, "cand_bad", chunks, rounds, time_s=0.9,
                      skip_rounds=(0,))
            write_raw(root, "cand_wrong", chunks, rounds, hash_value=12, time_s=0.9)

            # cand_missing was never reached: it has no raw, summary or
            # execution_status rows
            attempted_variants = ["baseline", "cand_good", "cand_bad",
                                  "cand_wrong", "baseline#2"]
            write_summary(root, attempted_variants, chunks, rounds)
            # the wrong-hash candidate also has a wrong summary row
            write_summary(root, ["cand_wrong"], chunks, rounds,
                          status="WRONG", hash_ok=0)

            attempted_displays = tuple(display for display in displays
                                       if display[0] != "cand_missing")
            write_execution(root, attempted_displays, rounds,
                            failures={"cand_bad": ("child_failed", 3)})

            rows, drift = summarize.build_rows(
                root, [("g.g", "c.cnf")], candidates=candidates,
                source_size=100, chunks=chunks, rounds=rounds)
            by_candidate = {row["candidate"]: row for row in rows}

            self.assertEqual(by_candidate["cand_good"]["status"], "MATCH")
            self.assertEqual(by_candidate["cand_bad"]["status"], "FAIL")
            self.assertEqual(by_candidate["cand_wrong"]["status"], "FAIL")
            self.assertEqual(by_candidate["cand_missing"]["status"], "NOT_RUN")
            # the failed sibling did not damage the healthy candidate
            self.assertEqual(by_candidate["cand_good"]["matched_chunks"], chunks)
            self.assertEqual(by_candidate["cand_good"]["timing_complete"], "yes")
            self.assertAlmostEqual(
                float(by_candidate["cand_good"]["time_ratio_open"]), 0.9, places=6)
            self.assertAlmostEqual(
                float(by_candidate["cand_good"]["time_ratio_close"]), 0.9 / 1.1,
                places=6)
            self.assertAlmostEqual(
                float(by_candidate["cand_good"]["baseline_drift_ratio"]), 1.1,
                places=6)
            self.assertEqual(len(drift), 1)
            self.assertAlmostEqual(float(drift[0]["closing_over_opening"]), 1.1,
                                   places=6)



STUB_CFG_BENCH_CFL = r"""#!/usr/bin/env python3
import argparse
import csv
import os
import sys
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--results-file", required=True)
parser.add_argument("--source-sizes", default="1")
parser.add_argument("--chunks", default="1")
parser.add_argument("--experiment", action="store_true")
parser.add_argument("--hot", action="store_true")
parser.add_argument("-c", "--config")
parser.add_argument("-a", dest="algo")
parser.add_argument("--rsm-lib")
parser.add_argument("-r")
parser.add_argument("--seed")
parser.add_argument("-e", action="store_true")
parser.add_argument("-f", action="store_true")
parser.add_argument("-l", action="store_true")
parser.add_argument("-b", action="store_true")
args, _ = parser.parse_known_args()

record = os.environ.get("STUB_ARGV_FILE")
if record:
    with open(record, "a") as file:
        file.write(" ".join(sys.argv[1:]) + "\n")

path = Path(args.results_file)
display = path.parent.name
round_id = int(path.stem[len("round"):])
if display == os.environ.get("STUB_FAIL_DISPLAY") and round_id == 0:
    sys.exit(4)

algo = args.algo or ""
if os.environ.get("STUB_WRONG_ALGO") == "1" and algo == "CFL_multsrc_adv":
    algo = "CFL_CFPQ_RSM"
hash_value = 0 if algo == "CFL_multsrc_adv" else 11
path.parent.mkdir(parents=True, exist_ok=True)
with open(path, "w", newline="") as file:
    writer = csv.writer(file)
    writer.writerow(["algo_name", "graph", "cnf", "source_size", "chunk",
                     "result", "result_hash", "time_s", "max_memory_kb"])
    for chunk in range(int(args.chunks)):
        writer.writerow([algo, "g.g", "c.cnf", str(args.source_sizes), str(chunk),
                         "5", str(hash_value), "1.0", "100.0"])
"""


def cfl_args(root: Path, out: Path, *, rounds: int, chunks: int = 1,
             keep_going: bool = True) -> SimpleNamespace:
    return SimpleNamespace(
        out=out, config=root / "lab.csv", cfg_bench_dir=root, chunks=chunks,
        rounds=rounds, seed=2026, source_size=1, threads=0, hot_rounds=1,
        keep_going=keep_going, order="blocked", jit_cache=None,
        baseline_variant="baseline", include_cfl_multsrc_adv=True,
    )


def cfl_displays():
    return [("baseline", "baseline"), ("current", "current"),
            ("CFL_multsrc_adv", "CFL_multsrc_adv")]


class CflRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.stub = self.root / "stub_cfg_bench.py"
        self.stub.write_text(STUB_CFG_BENCH_CFL)
        self.stub.chmod(0o755)
        (self.root / "lab.csv").write_text("g.g,c.cnf,-1\n")
        self.argv_file = self.root / "argv.txt"
        os.environ["STUB_ARGV_FILE"] = str(self.argv_file)

    def tearDown(self):
        for name in ("STUB_ARGV_FILE", "STUB_FAIL_DISPLAY", "STUB_WRONG_ALGO"):
            os.environ.pop(name, None)
        self.temporary.cleanup()

    def run_checks(self, args, displays, libraries):
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            return run.run_timing(args, self.stub, [("g.g", "c.cnf")],
                                  libraries, displays)

    def test_cfl_third_display_flags_order_and_timing_only_hash(self):
        out = self.root / "out"
        displays = cfl_displays()
        libraries = {"baseline": self.root / "b.so", "current": self.root / "c.so"}
        args = cfl_args(self.root, out, rounds=5)
        rc = self.run_checks(args, displays, libraries)
        self.assertEqual(rc, 0)

        with open(out / "execution_status.tsv", newline="") as file:
            statuses = list(csv.DictReader(file, delimiter="\t"))
        self.assertEqual(len(statuses), 15)
        self.assertEqual([row["display"] for row in statuses],
                         [display for display, _ in displays for _ in range(5)])
        self.assertEqual({row["outcome"] for row in statuses}, {"ok"})

        invocations = self.argv_file.read_text().splitlines()
        self.assertEqual(len(invocations), 15)
        cfl_lines = [line for line in invocations if "-a CFL_multsrc_adv" in line]
        rsm_lines = [line for line in invocations if "-a CFL_CFPQ_RSM" in line]
        self.assertEqual(len(cfl_lines), 5)
        self.assertEqual(len(rsm_lines), 10)
        for line in cfl_lines:
            tokens = line.split()
            for flag in ("-e", "-f", "-l", "-b"):
                self.assertIn(flag, tokens)
            self.assertNotIn("--rsm-lib", tokens)
        for line in rsm_lines:
            self.assertIn("--rsm-lib", line.split())
        self.assertEqual(sum(1 for line in cfl_lines if "--hot" in line.split()), 1)
        self.assertEqual(sum(1 for line in rsm_lines if "--hot" in line.split()), 2)

        with open(out / "summary.csv", newline="") as file:
            rows = list(csv.DictReader(file))
        cfl_rows = [row for row in rows if row["variant"] == "CFL_multsrc_adv"]
        self.assertEqual(len(cfl_rows), 1)
        self.assertEqual(cfl_rows[0]["status"], "ok")
        self.assertEqual(cfl_rows[0]["count_ok"], "")
        self.assertEqual(cfl_rows[0]["hash_ok"], "")
        self.assertEqual(cfl_rows[0]["result_hash"], "0")

    def test_wrong_algo_label_is_rejected(self):
        os.environ["STUB_WRONG_ALGO"] = "1"
        out = self.root / "out"
        libraries = {"baseline": self.root / "b.so", "current": self.root / "c.so"}
        args = cfl_args(self.root, out, rounds=1)
        rc = self.run_checks(args, cfl_displays(), libraries)
        self.assertNotEqual(rc, 0)
        with open(out / "execution_status.tsv", newline="") as file:
            statuses = list(csv.DictReader(file, delimiter="\t"))
        cfl = [row for row in statuses if row["display"] == "CFL_multsrc_adv"]
        self.assertEqual(len(cfl), 1)
        self.assertEqual(cfl[0]["outcome"], "malformed_csv")

    def test_keep_going_after_cfl_failure(self):
        os.environ["STUB_FAIL_DISPLAY"] = "CFL_multsrc_adv"
        out = self.root / "out"
        libraries = {"baseline": self.root / "b.so", "current": self.root / "c.so"}
        args = cfl_args(self.root, out, rounds=2)
        rc = self.run_checks(args, cfl_displays(), libraries)
        self.assertNotEqual(rc, 0)
        with open(out / "execution_status.tsv", newline="") as file:
            statuses = list(csv.DictReader(file, delimiter="\t"))
        self.assertEqual(len(statuses), 6)
        failed = [row for row in statuses if row["outcome"] != "ok"]
        self.assertEqual(len(failed), 1)
        self.assertEqual(failed[0]["display"], "CFL_multsrc_adv")
        self.assertEqual(failed[0]["round"], "0")
        self.assertEqual(failed[0]["exit_code"], "4")
        self.assertEqual(statuses[-1]["display"], "CFL_multsrc_adv")
        self.assertEqual(statuses[-1]["round"], "1")
        self.assertEqual(statuses[-1]["outcome"], "ok")


def write_multi_raw(root: Path, display: str, pairs, chunks: int, rounds: int, *,
                    hash_value: int = 11, skip_rounds: tuple[int, ...] = ()) -> None:
    directory = root / "raw" / summarize.safe_name(display)
    directory.mkdir(parents=True, exist_ok=True)
    algo = "CFL_multsrc_adv" if display == "CFL_multsrc_adv" else "CFL_CFPQ_RSM"
    for round_id in range(rounds):
        if round_id in skip_rounds:
            continue
        with open(directory / f"round{round_id}.csv", "w", newline="") as file:
            writer = csv.writer(file)
            writer.writerow(["algo_name", "graph", "cnf", "source_size", "chunk",
                             "result", "result_hash", "time_s", "max_memory_kb"])
            for graph, cnf in pairs:
                for chunk in range(chunks):
                    writer.writerow([algo, graph, cnf, "100", str(chunk), "5",
                                     str(hash_value), "1.0", "100.0"])


def write_multi_summary(root: Path, displays, pairs, chunks: int, rounds: int) -> None:
    fields = ["variant", "graph", "cnf", "source_size", "chunk", "rounds_seen",
              "n_runs", "status", "count_ok", "hash_ok", "result", "result_hash",
              "median_memory_kb"]
    with open(root / "summary.csv", "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for display in displays:
            cfl = display == "CFL_multsrc_adv"
            for graph, cnf in pairs:
                for chunk in range(chunks):
                    writer.writerow({
                        "variant": display, "graph": graph, "cnf": cnf,
                        "source_size": "100", "chunk": str(chunk),
                        "rounds_seen": str(rounds), "n_runs": str(rounds),
                        "status": "ok", "count_ok": "" if cfl else "1",
                        "hash_ok": "" if cfl else "1", "result": "5",
                        "result_hash": "0" if cfl else "11",
                        "median_memory_kb": "100.0",
                    })


def write_multi_execution(root: Path, displays, rounds: int) -> None:
    with open(root / "execution_status.tsv", "w", newline="") as file:
        writer = csv.writer(file, delimiter="\t", lineterminator="\n")
        writer.writerow(["display", "spec", "round", "start", "finish", "exit_code",
                         "outcome", "log"])
        for display, spec in displays:
            for round_id in range(rounds):
                writer.writerow([display, spec, round_id, "start", "finish", 0, "ok",
                                 str(root / "logs" / summarize.safe_name(display)
                                     / f"round{round_id}.log")])


class CurrentVsBb7SummaryTests(unittest.TestCase):
    def test_sixteen_match_without_closing_baseline(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pairs = summarize_current.read_config_pairs(summarize_current.CONFIG)
            self.assertEqual(len(pairs), 8)
            self.assertTrue(any("batik" in graph for graph, _ in pairs))
            displays = [("baseline", "baseline"), ("current", "current"),
                        ("CFL_multsrc_adv", "CFL_multsrc_adv")]
            chunks, rounds = 2, 2
            for display, _ in displays:
                write_multi_raw(root, display, pairs, chunks, rounds)
            write_multi_summary(root, [d for d, _ in displays], pairs, chunks, rounds)
            write_multi_execution(root, displays, rounds)

            rows = summarize_current.build_rows(root, pairs, chunks=chunks, rounds=rounds)
            self.assertEqual(len(rows), 16)
            self.assertEqual({row["status"] for row in rows}, {"MATCH"})
            self.assertFalse((root / "raw" / "baseline_2").exists())
            cfl_rows = [row for row in rows if row["candidate"] == "CFL_multsrc_adv"]
            self.assertTrue(cfl_rows)
            self.assertEqual({row["result_check"] for row in cfl_rows}, {"timing-only"})
            self.assertTrue(all(row["current_over_cfl_ratio"] for row in rows))
            # the full-run summarizer must not treat a one-chunk smoke as MATCH
            default_rows = summarize_current.build_rows(root, pairs)
            self.assertNotIn("MATCH", {row["status"] for row in default_rows})

            # wrong current hash -> current FAIL, CFL remains timing-only MATCH
            path = root / "raw" / "current" / "round0.csv"
            text = path.read_text().replace(",11,1.0,", ",12,1.0,", 1)
            path.write_text(text)
            rows = summarize_current.build_rows(root, pairs, chunks=chunks, rounds=rounds)
            current_statuses = {row["status"] for row in rows
                                if row["candidate"] == "current"}
            self.assertIn("FAIL", current_statuses)
            self.assertIn("MATCH", current_statuses)
            self.assertEqual({row["status"] for row in rows
                              if row["candidate"] == "CFL_multsrc_adv"}, {"MATCH"})

            # restore current, then remove CFL data -> CFL FAIL, current healthy
            write_multi_raw(root, "current", pairs, chunks, rounds)
            for round_id in range(rounds):
                (root / "raw" / "CFL_multsrc_adv" / f"round{round_id}.csv").unlink()
            rows = summarize_current.build_rows(root, pairs, chunks=chunks, rounds=rounds)
            self.assertEqual({row["status"] for row in rows
                              if row["candidate"] == "CFL_multsrc_adv"}, {"FAIL"})
            self.assertEqual({row["status"] for row in rows
                              if row["candidate"] == "current"}, {"MATCH"})


if __name__ == "__main__":
    unittest.main()
