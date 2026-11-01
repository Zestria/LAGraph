#!/usr/bin/env python3
"""Build CFPQ RSM variants and compare them on the same inputs.

The baseline is always ../LAGraph_CFPQ_RSM.c. A variant is one of:

  * patches/<part>.patch applied in order, e.g. "A+B" applies A.patch and
    then B.patch to a temporary copy of the baseline;
  * variants/<name>.c, a full source file used as is. This is the escape
    hatch for combinations whose patches do not apply cleanly.

Each variant is compiled into a shared library that exports
LAGraph_CFPQ_RSM. cfg_bench loads it with --rsm-lib, so the installed
liblagraphx is not touched and no sudo/install step is needed.

The runner interleaves variants round by round, compares the exact result
hash with the baseline, and prints a compact summary. Raw cfg_bench CSV
files and logs are kept in the output directory.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import math
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

LAB_DIR = Path(__file__).resolve().parent
LAGRAPH_DIR = LAB_DIR.parents[2]
DEFAULT_BASELINE = LAB_DIR.parent / "LAGraph_CFPQ_RSM.c"
DEFAULT_CFG_BENCH_DIR = Path.home() / "repo" / "CFG_bench"
DEFAULT_CONFIG = LAB_DIR / "configs" / "lab.csv"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lagraph", type=Path, default=LAGRAPH_DIR, help="LAGraph checkout (default: %(default)s)")
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE, help="baseline CFPQ RSM source")
    parser.add_argument("--patches-dir", type=Path, default=LAB_DIR / "patches", help="directory with *.patch files")
    parser.add_argument("--variants-dir", type=Path, default=LAB_DIR / "variants", help="directory with full *.c variants")
    parser.add_argument("--cfg-bench-dir", type=Path, default=DEFAULT_CFG_BENCH_DIR, help="CFG_bench checkout")
    parser.add_argument("--cfg-bench", type=Path, help="cfg_bench binary (default: <cfg-bench-dir>/build/cfg_bench)")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="benchmark config")
    parser.add_argument("--variants", default="baseline,baseline", help="comma-separated variants, e.g. baseline,A,B,A+B")
    parser.add_argument("--baseline-variant", default="baseline", help="variant used as the reference")
    parser.add_argument("--rounds", type=int, default=5, help="interleaved measured runs per variant (default: 5)")
    parser.add_argument(
        "--source-size",
        type=int,
        default=100,
        help="experiment chunk size; 0 uses all start vertices in normal mode (default: 100)",
    )
    parser.add_argument("--chunks", type=int, default=1, help="experiment chunks per run (default: 1)")
    parser.add_argument("--seed", type=int, default=2026, help="experiment source shuffle seed (default: 2026)")
    parser.add_argument("--out", type=Path, help="output directory (default: build/runs/<timestamp>)")
    parser.add_argument("--skip-cfg-build", action="store_true", help="do not run make in the CFG_bench checkout")
    parser.add_argument("--skip-variant-build", action="store_true", help="reuse existing libs/<variant>.so")
    parser.add_argument("--lagraphx-dir", type=Path, help="directory with liblagraphx used to build cfg_bench")
    parser.add_argument("--lagraph-lib-dir", type=Path, default=Path("/usr/local/lib"), help="directory with liblagraph")
    parser.add_argument("--compiler", default="cc", help="C compiler (default: cc)")
    parser.add_argument(
        "--cflags",
        default="-O2 -fPIC -shared -std=c11",
        help="flags for variant shared libraries (default: %(default)s)",
    )
    return parser.parse_args()


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.+-]", "_", name)


def run(cmd: list[str], cwd: Path, log_path: Path, what: str) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with open(log_path, "w") as log:
        log.write("+ " + " ".join(str(part) for part in cmd) + "\n\n")
        log.flush()
        result = subprocess.run(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        print(f"{what} failed (exit {result.returncode}), see {log_path}", file=sys.stderr)
        print(log_path.read_text()[-4000:], file=sys.stderr)
        raise SystemExit(1)


def find_lagraphx_dir(lagraph: Path, override: Path | None) -> Path:
    if override is not None:
        return override
    for candidate in (
        lagraph / "build-exp-release" / "experimental",
        lagraph / "build-baseline-release" / "experimental",
        Path("/usr/local/lib"),
    ):
        if (candidate / "liblagraphx.so").exists() or (candidate / "liblagraphx.so.1").exists():
            return candidate
    raise SystemExit("cannot find liblagraphx; pass --lagraphx-dir")


def build_cfg_bench(args: argparse.Namespace) -> Path:
    cfg_dir = args.cfg_bench_dir.resolve()
    binary = args.cfg_bench.resolve() if args.cfg_bench is not None else cfg_dir / "build" / "cfg_bench"
    if args.skip_cfg_build:
        if not binary.is_file():
            raise SystemExit(f"cfg_bench not found: {binary}")
        return binary

    lagraph = args.lagraph.resolve()
    lagraphx_dir = find_lagraphx_dir(lagraph, args.lagraphx_dir)
    includes = (
        f"-I/usr/local/include/suitesparse -I{lagraph / 'include'} "
        f"-I{cfg_dir / 'src'} -I{cfg_dir / 'src' / 'adapters'}"
    )
    ldlibs = (
        f"-L{lagraphx_dir} -llagraphx -llagraph -lgraphblas -ldl "
        f"-Wl,-rpath,{lagraphx_dir}"
    )
    out_dir = args.out.resolve()
    run(
        ["make", "-C", str(cfg_dir), f"INCLUDES={includes}", f"LDLIBS={ldlibs}"],
        cwd=cfg_dir,
        log_path=out_dir / "logs" / "build_cfg_bench.log",
        what="cfg_bench build",
    )
    if not binary.is_file():
        raise SystemExit(f"cfg_bench was not built: {binary}")
    return binary


def variant_source(spec: str, args: argparse.Namespace, work_dir: Path) -> Path:
    full = args.variants_dir / f"{spec}.c"
    if full.is_file():
        return full.resolve()

    src_dir = work_dir / "src" / safe_name(spec)
    src_dir.mkdir(parents=True, exist_ok=True)
    src = src_dir / "LAGraph_CFPQ_RSM.c"
    shutil.copyfile(args.baseline, src)
    if spec == "baseline":
        return src.resolve()
    for part in spec.split("+"):
        patch = args.patches_dir / f"{part}.patch"
        if not patch.is_file():
            raise SystemExit(f"missing patch for {spec}: {patch}")
        result = subprocess.run(["patch", "-p1", "-s", "-i", str(patch)], cwd=src_dir, capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(
                f"patch {patch.name} does not apply to {spec}:\n{result.stdout}{result.stderr}"
                f"\nUse variants/{spec}.c for a manually combined full file."
            )
    return src.resolve()


def build_variant(spec: str, source: Path, args: argparse.Namespace, out_dir: Path) -> Path:
    so = out_dir / "libs" / f"{safe_name(spec)}.so"
    if args.skip_variant_build and so.is_file():
        return so
    so.parent.mkdir(parents=True, exist_ok=True)
    lagraph = args.lagraph.resolve()
    cmd = [
        args.compiler,
        *args.cflags.split(),
        f"-I{lagraph / 'include'}",
        f"-I{lagraph / 'src' / 'utility'}",
        "-I/usr/local/include/suitesparse",
        str(source),
        "-o",
        str(so),
        f"-L{args.lagraph_lib_dir}",
        "-llagraph",
        "-lgraphblas",
    ]
    run(cmd, cwd=args.lagraph.resolve(), log_path=out_dir / "logs" / "build" / f"{safe_name(spec)}.log",
        what=f"build {spec}")
    return so


def check_config(config: Path, cfg_dir: Path) -> None:
    if not config.is_file():
        raise SystemExit(f"config not found: {config}")
    missing = []
    for line_number, line in enumerate(config.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 2:
            raise SystemExit(f"{config}:{line_number}: expected at least graph,grammar")
        for field in fields[:2]:
            path = Path(field)
            if not path.is_absolute():
                path = cfg_dir / path
            if not path.exists():
                missing.append(str(path))
        if len(fields) >= 4 and fields[3] and fields[3] != "-1":
            path = Path(fields[3])
            if not path.is_absolute():
                path = cfg_dir / path
            if not path.exists():
                missing.append(str(path))
    if missing:
        raise SystemExit("missing input files:\n  " + "\n  ".join(sorted(set(missing))))


def read_rows(path: Path) -> list[dict]:
    with open(path, newline="") as file:
        return [row for row in csv.DictReader(file) if row.get("algo_name") != "algo_name"]


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def main() -> int:
    args = parse_args()
    args.baseline = args.baseline.resolve()
    args.config = args.config.resolve()
    args.cfg_bench_dir = args.cfg_bench_dir.resolve()
    if args.rounds <= 0:
        raise SystemExit("--rounds must be positive")
    if args.source_size < 0 or args.chunks <= 0:
        raise SystemExit("--source-size must be >= 0 and --chunks must be positive")
    if not args.baseline.is_file():
        raise SystemExit(f"baseline not found: {args.baseline}")

    if args.out is None:
        stamp = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%d_%H-%M-%S")
        args.out = LAB_DIR / "build" / "runs" / stamp
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)

    check_config(args.config, args.cfg_bench_dir)
    cfg_bench = build_cfg_bench(args)

    specs = [spec.strip() for spec in args.variants.split(",") if spec.strip()]
    if not specs:
        raise SystemExit("--variants is empty")
    counts: dict[str, int] = {}
    displays: list[tuple[str, str]] = []
    for spec in specs:
        counts[spec] = counts.get(spec, 0) + 1
        display = spec if counts[spec] == 1 else f"{spec}#{counts[spec]}"
        displays.append((display, spec))

    sources: dict[str, Path] = {}
    libraries: dict[str, Path] = {}
    for display, spec in displays:
        if spec not in sources:
            sources[spec] = variant_source(spec, args, args.out)
            libraries[spec] = build_variant(spec, sources[spec], args, args.out)
        print(f"variant {display}: {libraries[spec]}", flush=True)

    baseline_display = next((display for display, spec in displays if spec == args.baseline_variant), None)
    if baseline_display is None:
        raise SystemExit(f"baseline variant '{args.baseline_variant}' is not in --variants")

    results: dict[str, dict[tuple, list[dict]]] = {display: {} for display, _ in displays}
    for round_index in range(args.rounds):
        # rotate the order so a systematic first/last position effect is not
        # attributed to one variant
        offset = round_index % len(displays)
        order = displays[offset:] + displays[:offset]
        for display, spec in order:
            csv_path = args.out / "raw" / safe_name(display) / f"round{round_index}.csv"
            csv_path.parent.mkdir(parents=True, exist_ok=True)
            if csv_path.exists():
                csv_path.unlink()
            cmd = [
                str(cfg_bench),
                "-c",
                str(args.config),
                "-a",
                "CFL_CFPQ_RSM",
                "--rsm-lib",
                str(libraries[spec]),
                "-r",
                "1",
                "--hot",
                "--results-file",
                str(csv_path),
            ]
            if args.source_size > 0:
                cmd += [
                    "--experiment",
                    "--source-sizes",
                    str(args.source_size),
                    "--chunks",
                    str(args.chunks),
                    "--seed",
                    str(args.seed),
                ]
            else:
                cmd += ["--use-start-nodes"]
            run(
                cmd,
                cwd=args.cfg_bench_dir,
                log_path=args.out / "logs" / safe_name(display) / f"round{round_index}.log",
                what=f"cfg_bench {display} round {round_index}",
            )
            rows = read_rows(csv_path)
            if not rows:
                raise SystemExit(f"no result rows in {csv_path}")
            for row in rows:
                key = (row["graph"], row["cnf"], row["source_size"], row["chunk"])
                results[display].setdefault(key, []).append(row)

    base_results = results[baseline_display]
    if not base_results:
        raise SystemExit("baseline produced no results")

    def aggregate(display: str, key: tuple) -> dict:
        rows = results[display].get(key, [])
        hashes = {int(row.get("result_hash") or 0) for row in rows}
        return {
            "n": len(rows),
            "median_time": median([float(row["time_s"]) for row in rows]),
            "result": int(rows[-1]["result"]) if rows else 0,
            "hash": int(rows[-1].get("result_hash") or 0) if rows else 0,
            "memory_kb": median([float(row["max_memory_kb"]) for row in rows]),
            "unstable": len(hashes) > 1,
        }

    baseline_stats = {key: aggregate(baseline_display, key) for key in base_results}
    summary_rows: list[dict] = []
    per_variant: dict[str, dict] = {}

    for display, _ in displays:
        ratios = []
        worst = 0.0
        best = math.inf
        medians = []
        memory = 0.0
        wrong = 0
        missing = 0
        unstable = 0
        for key, base in baseline_stats.items():
            stats = aggregate(display, key)
            if stats["n"] == 0:
                missing += 1
                status = "missing"
                ratio = 0.0
            else:
                if stats["unstable"]:
                    unstable += 1
                if base["hash"] != 0 and stats["hash"] != 0:
                    correct = stats["hash"] == base["hash"]
                else:
                    correct = stats["result"] == base["result"]
                if not correct:
                    wrong += 1
                status = "ok" if correct and not stats["unstable"] else ("WRONG" if not correct else "UNSTABLE")
                ratio = stats["median_time"] / base["median_time"] if base["median_time"] > 0 else 0.0
                if ratio > 0:
                    ratios.append(ratio)
                    worst = max(worst, ratio)
                    best = min(best, ratio)
                medians.append(stats["median_time"])
                memory = max(memory, stats["memory_kb"])
            summary_rows.append(
                {
                    "variant": display,
                    "graph": key[0],
                    "cnf": key[1],
                    "source_size": key[2],
                    "chunk": key[3],
                    "n_runs": stats["n"],
                    "median_time_s": f"{stats['median_time']:.9f}",
                    "ratio_to_baseline": f"{ratio:.6f}",
                    "result": stats["result"],
                    "result_hash": stats["hash"],
                    "status": status,
                }
            )
        per_variant[display] = {
            "geo": math.exp(sum(math.log(ratio) for ratio in ratios) / len(ratios)) if ratios else 0.0,
            "best": best if best != math.inf else 0.0,
            "worst": worst,
            "median_ms": median(medians) * 1000.0,
            "memory_mb": memory / 1024.0,
            "wrong": wrong,
            "missing": missing,
            "unstable": unstable,
        }

    summary_path = args.out / "summary.csv"
    with open(summary_path, "w", newline="") as file:
        writer = csv.DictWriter(
            file,
            fieldnames=[
                "variant",
                "graph",
                "cnf",
                "source_size",
                "chunk",
                "n_runs",
                "median_time_s",
                "ratio_to_baseline",
                "result",
                "result_hash",
                "status",
            ],
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(summary_rows)

    print()
    print(f"{'variant':24} {'geo_ratio':>9} {'min':>7} {'max':>7} {'median_ms':>10} {'mem_MB':>8}  status")
    failed = False
    for display, _ in displays:
        stats = per_variant[display]
        if stats["wrong"] or stats["missing"]:
            failed = True
            status = f"WRONG({stats['wrong']})" if stats["wrong"] else f"missing({stats['missing']})"
        elif stats["unstable"]:
            status = f"unstable({stats['unstable']})"
        else:
            status = "ok"
        print(
            f"{display:24} {stats['geo']:9.3f} {stats['best']:7.3f} {stats['worst']:7.3f} "
            f"{stats['median_ms']:10.3f} {stats['memory_mb']:8.1f}  {status}"
        )
    print()
    print(f"summary: {summary_path}")
    print(f"raw:     {args.out / 'raw'}")
    if len([spec for spec in specs if spec == args.baseline_variant]) > 1:
        print("noise floor: compare the baseline rows above")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
