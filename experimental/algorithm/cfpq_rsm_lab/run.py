#!/usr/bin/env python3
"""Build CFPQ RSM variants and compare them on the same inputs (DS-01-R2).

The baseline is normally the frozen corrected reference
(experimental/algorithm/cfpq_rsm_lab/variants/correctness_r2.c). A variant is one
of:

  * patches/<part>.patch applied in order, e.g. "A+B" applies A.patch and
    then B.patch to a temporary copy of the baseline;
  * variants/<name>.c, a full source file used as is.

Each variant is compiled into a shared library that exports
LAGraph_CFPQ_RSM. cfg_bench loads it with --rsm-lib, so the installed
liblagraphx is not touched.

DS-01-R2 acceptance (shared by timing and profile modes; a run exits nonzero if
any check fails):
  * exactly the expected (config, source size, chunk) case set, once per round,
    and nothing else;
  * every expected round for every case;
  * every round has a nonzero result hash;
  * result cardinality and hash are stable across rounds for each variant;
  * result cardinality and hash match the baseline on every round, by round ID;
  * duplicate corrected baselines are compared and the noise floor is printed;
  * profile mode additionally requires exactly one expected row per case in
    each pass, well-formed diagnostic records with matching tags, two records
    (warm-up + measured) per (case, chunk), matching q/v shapes, and sane
    profiler reconciliation (sanity_ok=1).

Run-owned output files are cleared only with --force; a nonempty output
directory is rejected otherwise.  Every profile record carries a durable tag
(case, source size, seed) and, after annotation, chunk/warm-up/round columns.

--hot is cfg_bench's per-invocation warm-up: one unmeasured run of each case
before the measured -r 1 run. Warm-up is never a measured sample.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

LAB_DIR = Path(__file__).resolve().parent
LAGRAPH_DIR = LAB_DIR.parents[2]
DEFAULT_BASELINE = LAB_DIR / "variants" / "correctness_r2.c"
DEFAULT_CFG_BENCH_DIR = Path.home() / "repo" / "CFG_bench"
DEFAULT_CONFIG = LAB_DIR / "configs" / "lab.csv"

# entries owned by a run; reused only with --force
RUN_OWNED = ("summary.csv", "provenance.txt", "inputs.sha256", "raw", "logs", "libs",
             "profile", "src", "jit-cache", "profile.csv", "profile-calls.csv")

PROFILE_REQUIRED_KEYS = (
    "tag", "call", "q", "v", "iters", "setup_s", "activity_s", "phase1_s", "phase2_3_s",
    "phase4_s", "phase5_s", "final_s", "materialize_s", "cleanup_s", "boundary_s",
    "diag_io_s", "total_s", "kernel_s", "unclassified_s", "sanity_ok",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lagraph", type=Path, default=LAGRAPH_DIR, help="LAGraph checkout (default: %(default)s)")
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE, help="baseline CFPQ RSM source")
    parser.add_argument("--patches-dir", type=Path, default=LAB_DIR / "patches", help="directory with *.patch files")
    parser.add_argument("--variants-dir", type=Path, default=LAB_DIR / "variants", help="directory with full *.c variants")
    parser.add_argument("--cfg-bench-dir", type=Path, default=DEFAULT_CFG_BENCH_DIR, help="CFG_bench checkout")
    parser.add_argument("--cfg-bench", type=Path, help="cfg_bench binary (default: <cfg-bench-dir>/build/cfg_bench)")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="benchmark config")
    parser.add_argument("--variants", default="baseline,baseline", help="comma-separated variants, e.g. baseline,baseline,A")
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
    parser.add_argument("--force", action="store_true", help="clear run-owned files in a nonempty output directory")
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
    parser.add_argument("--threads", type=int, default=1, help="OMP_NUM_THREADS for cfg_bench (default: 1)")
    parser.add_argument(
        "--jit-cache",
        type=Path,
        help="GRAPHBLAS_CACHE_PATH for cfg_bench (default: <out>/jit-cache, a writable per-run cache)",
    )
    parser.add_argument("--provenance-only", action="store_true", help="write provenance and exit")
    parser.add_argument(
        "--profile",
        action="store_true",
        help="profiling pass: one single-case run per config/source-size, write profile.csv",
    )
    return parser.parse_args()


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.+-]", "_", name)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for chunk in iter(lambda: file.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_identity(path: Path) -> str:
    try:
        st = path.stat()
        return f"{sha256_file(path)}:{st.st_size}:{int(st.st_mtime)}"
    except OSError:
        return "unavailable"


def prepare_output_dir(out: Path, force: bool) -> None:
    """Reject a nonempty run directory, or clear only run-owned entries."""
    out.mkdir(parents=True, exist_ok=True)
    owned = [out / name for name in RUN_OWNED if (out / name).exists()]
    if owned and not force:
        names = ", ".join(str(p.name) for p in owned)
        raise SystemExit(
            f"output directory {out} already contains run-owned files ({names}); "
            "use --force to replace them or choose a new --out"
        )
    for path in owned:
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        else:
            path.unlink()


def run(cmd: list[str], cwd: Path, log_path: Path, what: str, env: dict | None = None,
        extra_env: dict | None = None) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    child_env = dict(env if env is not None else os.environ)
    if extra_env:
        child_env.update(extra_env)
    with open(log_path, "w") as log:
        log.write("+ " + " ".join(str(part) for part in cmd) + "\n")
        if extra_env:
            log.write("# env " + " ".join(f"{k}={v}" for k, v in extra_env.items()) + "\n\n")
        log.flush()
        result = subprocess.run(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, env=child_env)
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
    binary = args.cfg_bench.resolve() if args.cfg_bench is not None else cfg_dir / "build/cfg_bench"
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
    run(
        ["make", "-C", str(cfg_dir), f"INCLUDES={includes}", f"LDLIBS={ldlibs}"],
        cwd=cfg_dir,
        log_path=args.out / "logs" / "build_cfg_bench.log",
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


def build_dependencies(args: argparse.Namespace, source: Path) -> list[Path]:
    lagraph = args.lagraph.resolve()
    deps = [
        source,
        lagraph / "include" / "LAGraph.h",
        lagraph / "include" / "LAGraphX.h",
        lagraph / "src" / "utility" / "LG_internal.h",
        Path("/usr/local/include/suitesparse/GraphBLAS.h"),
    ]
    profile_header = LAB_DIR / "variants" / "cfpq_profile.h"
    deps.append(profile_header)
    # library contents also invalidate a reused shared object
    for name in ("liblagraph.so", "libgraphblas.so"):
        for base in (args.lagraph_lib_dir, Path("/usr/local/lib")):
            candidate = base / name
            if candidate.exists():
                deps.append(candidate)
                break
    return [p for p in deps if p.exists()]


def build_variant(spec: str, source: Path, args: argparse.Namespace, out_dir: Path,
                  extra_cflags: str = "") -> Path:
    so = out_dir / "libs" / f"{safe_name(spec)}.so"
    stamp = so.with_suffix(".stamp")
    deps = build_dependencies(args, source)
    compiler_identity = subprocess.run([args.compiler, "--version"], capture_output=True, text=True)
    compiler_line = compiler_identity.stdout.splitlines()[0] if compiler_identity.stdout else "unavailable"
    build_key = "\n".join(
        [f"compiler {args.compiler} {compiler_line}", f"cflags {args.cflags} {extra_cflags}".strip()]
        + [f"dep {p} {file_identity(p)}" for p in deps]
    )
    if args.skip_variant_build and so.is_file():
        if not stamp.is_file() or stamp.read_text() != build_key:
            raise SystemExit(
                f"--skip-variant-build: {so} does not match its recorded source/build key; rebuild it"
            )
        return so
    so.parent.mkdir(parents=True, exist_ok=True)
    lagraph = args.lagraph.resolve()
    cmd = [
        args.compiler,
        *args.cflags.split(),
        *extra_cflags.split(),
        f"-I{lagraph / 'include'}",
        f"-I{lagraph / 'src' / 'utility'}",
        f"-I{source.parent}",
        f"-I{LAB_DIR / 'variants'}",
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
    stamp.write_text(build_key)
    return so


def read_config_lines(config: Path) -> list[str]:
    return [line for line in config.read_text().splitlines() if line.strip() and not line.strip().startswith("#")]


def read_config_pairs(config: Path) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    for line in read_config_lines(config):
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 2:
            raise SystemExit(f"{config}: expected at least graph,grammar")
        pairs.append((fields[0], fields[1]))
    if not pairs:
        raise SystemExit(f"{config}: no cases")
    return pairs


def config_input_paths(config: Path, cfg_dir: Path) -> list[Path]:
    paths: list[Path] = []
    for line in read_config_lines(config):
        fields = [field.strip() for field in line.split(",")]
        for index in (0, 1, 3):
            if index < len(fields) and fields[index] and fields[index] != "-1":
                path = Path(fields[index])
                if not path.is_absolute():
                    path = cfg_dir / path
                paths.append(path)
    return sorted(set(paths))


def check_config(config: Path, cfg_dir: Path) -> list[Path]:
    if not config.is_file():
        raise SystemExit(f"config not found: {config}")
    missing = [str(path) for path in config_input_paths(config, cfg_dir) if not path.exists()]
    if missing:
        raise SystemExit("missing input files:\n  " + "\n  ".join(missing))
    return config_input_paths(config, cfg_dir)


def read_rows(path: Path) -> list[dict]:
    with open(path, newline="") as file:
        return [row for row in csv.DictReader(file) if row.get("algo_name") != "algo_name"]


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def iqr(values: list[float]) -> float:
    if len(values) < 4:
        return 0.0
    q = statistics.quantiles(values, n=4, method="inclusive")
    return q[2] - q[0]


def expected_keys(pairs: list[tuple[str, str]], args: argparse.Namespace) -> list[tuple] | None:
    if args.source_size <= 0:
        return None
    keys: list[tuple] = []
    for graph, cnf in pairs:
        for chunk in range(args.chunks):
            keys.append((graph, cnf, str(args.source_size), str(chunk)))
    return keys


def add_result_row(results: dict, display: str, key: tuple, round_id: int, row: dict) -> None:
    rounds = results.setdefault(display, {}).setdefault(key, {})
    if round_id in rounds:
        raise SystemExit(f"duplicate case {key} round {round_id} for variant {display}")
    rounds[round_id] = row


def accept_rows(results: dict, expected: list[tuple], rounds: int,
                baseline_display: str) -> tuple[list[str], dict]:
    """Shared acceptance for timing and profile modes.  Returns (problems, stats).

    results[display][key][round_id] -> cfg_bench row.  Every comparison is by
    explicit round ID, so missing rounds cannot misalign data or raise.
    """
    problems: list[str] = []
    stats: dict = {}
    expected_set = set(expected)
    if baseline_display not in results:
        return [f"missing baseline variant {baseline_display}"], stats
    base = results[baseline_display]
    for display in sorted(results):
        rows_by_key = results[display]
        for key in expected:
            if key not in rows_by_key:
                problems.append(f"{display}: missing case {key}")
        for key in sorted(rows_by_key):
            if key not in expected_set:
                problems.append(f"{display}: unexpected case {key}")
        for key in expected:
            if key not in rows_by_key:
                continue
            rounds_map = rows_by_key[key]
            seen = sorted(rounds_map)
            if seen != list(range(rounds)):
                problems.append(
                    f"{display}: case {key} has rounds {seen}, expected 0..{rounds - 1}"
                )
            counts = {}
            hashes = {}
            ref_counts = {}
            ref_hashes = {}
            malformed = False
            for round_id in seen:
                row = rounds_map[round_id]
                try:
                    count = int(row["result"])
                    hash_value = int(row["result_hash"] or 0)
                except (KeyError, TypeError, ValueError):
                    problems.append(f"{display}: case {key} round {round_id} is malformed")
                    malformed = True
                    continue
                counts[round_id] = count
                hashes[round_id] = hash_value
                if hash_value == 0:
                    problems.append(f"{display}: case {key} round {round_id} has a zero/unavailable hash")
            if len(set(counts.values())) > 1:
                problems.append(f"{display}: case {key} has unstable counts {sorted(set(counts.values()))}")
            if len(set(hashes.values())) > 1:
                problems.append(f"{display}: case {key} has unstable hashes {sorted(set(hashes.values()))}")
            ref_rounds = base.get(key, {})
            for round_id in seen:
                if round_id not in ref_rounds:
                    problems.append(f"{baseline_display}: case {key} round {round_id} missing")
                    malformed = True
                    continue
                try:
                    ref_count = int(ref_rounds[round_id]["result"])
                    ref_hash = int(ref_rounds[round_id]["result_hash"] or 0)
                except (KeyError, TypeError, ValueError):
                    problems.append(f"{baseline_display}: case {key} round {round_id} is malformed")
                    malformed = True
                    continue
                ref_counts[round_id] = ref_count
                ref_hashes[round_id] = ref_hash
                if counts.get(round_id) != ref_count:
                    problems.append(
                        f"{display}: case {key} round {round_id} count {counts.get(round_id)} "
                        f"!= baseline {ref_count}"
                    )
                if hashes.get(round_id) != ref_hash:
                    problems.append(
                        f"{display}: case {key} round {round_id} hash {hashes.get(round_id)} "
                        f"!= baseline {ref_hash}"
                    )
            base_times = [float(ref_rounds[r]["time_s"]) for r in ref_rounds if r in seen
                          and "time_s" in ref_rounds[r]]
            times = [float(rounds_map[r]["time_s"]) for r in seen
                     if "time_s" in rounds_map[r]]
            memories = [float(rounds_map[r]["max_memory_kb"]) for r in seen
                        if rounds_map[r].get("max_memory_kb") not in (None, "")]
            ref_memories = [float(ref_rounds[r]["max_memory_kb"]) for r in ref_rounds
                            if ref_rounds[r].get("max_memory_kb") not in (None, "")]
            count_ok = (not malformed) and len({counts[r] for r in seen}) <= 1 and all(
                counts.get(r) == ref_counts.get(r) for r in seen
            )
            hash_ok = (not malformed) and len({hashes[r] for r in seen}) <= 1 and all(
                hashes.get(r) == ref_hashes.get(r) for r in seen
            )
            status = "ok"
            if seen != list(range(rounds)):
                status = "MISSING_ROUNDS"
            elif malformed:
                status = "MALFORMED"
            elif any(hashes.get(r, 0) == 0 for r in seen):
                status = "ZERO_HASH"
            elif len(set(counts.values())) > 1 or len(set(hashes.values())) > 1:
                status = "UNSTABLE"
            elif not count_ok or not hash_ok:
                status = "WRONG"
            stats[(display, key)] = {
                "seen_rounds": seen,
                "counts": counts,
                "hashes": hashes,
                "times": times,
                "ref_times": base_times,
                "memories": memories,
                "ref_memories": ref_memories,
                "count_ok": count_ok,
                "hash_ok": hash_ok,
                "status": status,
            }
    return problems, stats


def cxx_ident(version_text: str) -> str:
    text = version_text.strip()
    return text.splitlines()[0] if text else "unavailable"


def write_provenance(args: argparse.Namespace, out: Path, cfg_bench: Path, config: Path,
                     libraries: dict[str, Path], sources: dict[str, Path],
                     input_paths: list[Path]) -> None:
    def sh(cmd: list[str], cwd: Path, default: str = "unavailable") -> str:
        try:
            result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=120)
            return result.stdout.strip() if result.returncode == 0 else default
        except Exception:
            return default

    lines: list[str] = []
    lines.append("# CFPQ RSM lab run provenance (DS-01-R2)")
    lines.append(f"timestamp: {datetime.datetime.now(datetime.UTC).isoformat()}")
    lines.append(f"output: {out}")
    lines.append(f"repo_commit: {sh(['git', 'rev-parse', 'HEAD'], args.lagraph.resolve())}")
    lines.append(f"repo_status: {sh(['git', 'status', '--porcelain'], args.lagraph.resolve()).replace(chr(10), ' | ')}")
    lines.append(f"config: {config} sha256={sha256_file(config)}")
    lines.append(f"cfg_bench: {cfg_bench} sha256={sha256_file(cfg_bench)}")
    lines.append(f"runner: {Path(__file__).resolve()} sha256={sha256_file(Path(__file__).resolve())}")
    generator = LAB_DIR / "tests" / "make_profile_variant.py"
    if generator.is_file():
        lines.append(f"generator: {generator} sha256={sha256_file(generator)}")
    header = LAB_DIR / "variants" / "cfpq_profile.h"
    if header.is_file():
        lines.append(f"profile_header: {header} sha256={sha256_file(header)}")
    lines.append(f"compiler: {cxx_ident(sh([args.compiler, '--version'], args.lagraph.resolve()))}")
    lines.append(f"cflags: {args.cflags}")
    lines.append(f"threads: OMP_NUM_THREADS={args.threads}")
    lines.append(f"machine: {sh(['uname', '-srm'], args.lagraph.resolve())}")
    lines.append(f"nproc: {os.cpu_count()}")
    cpu_model = ""
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    lines.append(f"cpu: {cpu_model or 'unavailable'}")
    jit_cache = args.jit_cache if args.jit_cache is not None else out / "jit-cache"
    lines.append(f"jit_cache: {jit_cache}")
    lines.append("jit_note: GRAPHBLAS_CACHE_PATH is set to the run-local cache; the JIT control is left "
                 "at the library default (ON); cfg_bench --hot performs one unmeasured warm-up call per "
                 "case before the measured -r 1 call")
    lines.append("")
    lines.append("# input manifest (sha256:size:mtime, resolved under the CFG_bench checkout)")
    input_lines = []
    for path in input_paths:
        identity = file_identity(path)
        lines.append(f"input: {path} {identity}")
        input_lines.append(f"{identity}  {path}")
    (out / "inputs.sha256").write_text("\n".join(input_lines) + "\n")
    lines.append("")
    lines.append("# variants")
    for spec, source in sorted(sources.items()):
        lines.append(f"source {spec}: {source} sha256={sha256_file(source)}")
        lines.append(f"library {spec}: {libraries[spec]} sha256={sha256_file(libraries[spec])}")
        ldd = sh(["ldd", str(libraries[spec])], args.lagraph.resolve())
        for entry in ldd.splitlines():
            if "lagraph" in entry or "graphblas" in entry:
                resolved = entry.split("=>")[-1].split("(")[0].strip()
                identity = file_identity(Path(resolved)) if Path(resolved).exists() else "unavailable"
                lines.append(f"  {entry.strip()} identity={identity}")
    lines.append("")
    lines.append("# exact commands")
    lines.append("python3 experimental/algorithm/cfpq_rsm_lab/run.py " + " ".join(sys.argv[1:]))
    (out / "provenance.txt").write_text("\n".join(lines) + "\n")


def profile_parse(path: Path) -> list[tuple[int, dict]]:
    calls: list[tuple[int, dict]] = []
    if not path.is_file():
        return calls
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or "=" not in line:
            continue
        record: dict[str, str] = {}
        for field in line.split(";"):
            if "=" in field:
                key, value = field.split("=", 1)
                record[key] = value
        calls.append((line_number, record))
    return calls


def annotate_profile_csv(path: Path, annotated: Path) -> None:
    """Add chunk/warm-up/round columns to a kernel-written diagnostic CSV."""
    if not path.is_file():
        return
    with open(path, newline="") as src, open(annotated, "w", newline="") as dst:
        reader = csv.reader(src)
        writer = csv.writer(dst, lineterminator="\n")
        try:
            header = next(reader)
        except StopIteration:
            return
        if header[:2] == ["tag", "call"]:
            call_index = 1
            new_header = header[:2] + ["chunk", "warmup", "round"] + header[2:]
        elif header[:3] == ["tag", "kind", "call"]:
            call_index = 2
            new_header = header[:3] + ["chunk", "warmup", "round"] + header[3:]
        else:
            raise SystemExit(f"unexpected diagnostic CSV header in {path}: {header[:4]}")
        writer.writerow(new_header)
        for row in reader:
            if len(row) <= call_index:
                raise SystemExit(f"malformed diagnostic row in {path}: {row[:4]}")
            try:
                call = int(row[call_index])
            except ValueError:
                raise SystemExit(f"malformed call id in {path}: {row[:4]}")
            # cfg_bench --hot runs the warm-up first: call 0 is warm-up, call 1
            # is measured, then call 2 is the next chunk's warm-up, and so on
            warmup = 1 - (call % 2)
            writer.writerow(row[:call_index + 1] + [str(call // 2), str(warmup), "0"] + row[call_index + 1:])
    path.unlink()


def run_profile(args: argparse.Namespace, cfg_bench: Path, out: Path, pairs: list[tuple[str, str]],
                libraries: dict[str, Path]) -> int:
    if args.source_size <= 0:
        raise SystemExit("--profile requires --source-size > 0")
    size = args.source_size
    env = dict(os.environ)
    env["OMP_NUM_THREADS"] = str(args.threads)
    env["GRAPHBLAS_CACHE_PATH"] = str(args.jit_cache if args.jit_cache is not None else out / "jit-cache")
    (out / "profile").mkdir(parents=True, exist_ok=True)
    (out / "profile" / "configs").mkdir(parents=True, exist_ok=True)

    config_lines = read_config_lines(args.config)
    line_by_pair = {}
    for line in config_lines:
        fields = [field.strip() for field in line.split(",")]
        line_by_pair[(fields[0], fields[1])] = line

    problems: list[str] = []
    results: dict[str, dict[tuple, dict[int, dict]]] = {"reference": {}, "profile": {}}
    profile_rows: list[dict] = []
    all_calls: list[dict] = []
    expected: set[tuple] = set()

    for graph, cnf in pairs:
        slug = safe_name(f"{Path(graph).stem}__{Path(cnf).stem}__s{size}")
        config_path = out / "profile" / "configs" / f"{slug}.csv"
        config_path.write_text(line_by_pair[(graph, cnf)] + "\n")
        for chunk in range(args.chunks):
            expected.add((graph, cnf, str(size), str(chunk)))
        tag = f"{slug}|s{size}|seed{args.seed}"
        for variant in ("reference", "profile"):
            results_csv = out / "profile" / f"results-{variant}-{slug}.csv"
            profile_path = out / "profile" / f"profile-{slug}.txt"
            if variant == "profile":
                profile_path.unlink(missing_ok=True)
            cmd = [
                str(cfg_bench),
                "-c", str(config_path),
                "-a", "CFL_CFPQ_RSM",
                "--rsm-lib", str(libraries[variant]),
                "-r", "1",
                "--hot",
                "--results-file", str(results_csv),
                "--experiment",
                "--source-sizes", str(size),
                "--chunks", str(args.chunks),
                "--seed", str(args.seed),
            ]
            extra = ({"CFPQ_RSM_PROFILE_TAG": tag, "CFPQ_RSM_PROFILE_FILE": str(profile_path)}
                     if variant == "profile" else None)
            run(cmd, cwd=args.cfg_bench_dir,
                log_path=out / "logs" / "profile" / f"{variant}-{slug}.log",
                what=f"cfg_bench profile {variant} {slug}", env=env, extra_env=extra)
            rows = read_rows(results_csv)
            if len(rows) != args.chunks:
                problems.append(f"{slug}: {variant} produced {len(rows)} result rows, expected {args.chunks}")
            for row in rows:
                key = (graph, cnf, row["source_size"], row["chunk"])
                add_result_row(results, variant, key, 0, row)
        # diagnostic records: exactly warm-up + measured per chunk
        calls = profile_parse(out / "profile" / f"profile-{slug}.txt")
        if len(calls) != 2 * args.chunks:
            problems.append(
                f"{slug}: expected {2 * args.chunks} diagnostic records (warm-up + measured per chunk), "
                f"found {len(calls)}"
            )
            continue
        for chunk in range(args.chunks):
            warm_line, warm = calls[2 * chunk]
            meas_line, meas = calls[2 * chunk + 1]
            missing_keys = [key for key in PROFILE_REQUIRED_KEYS if key not in meas]
            if missing_keys:
                problems.append(f"{slug} chunk {chunk}: measured record misses {missing_keys}")
                continue
            if meas["tag"] != tag or warm.get("tag") != tag:
                problems.append(f"{slug} chunk {chunk}: diagnostic tag {meas['tag']!r} != {tag!r}")
            if meas.get("q") != warm.get("q") or meas.get("v") != warm.get("v"):
                problems.append(f"{slug} chunk {chunk}: warm-up/measured shapes disagree")
            if meas.get("sanity_ok") != "1":
                problems.append(
                    f"{slug} chunk {chunk}: profiler sanity failed "
                    f"(violations={meas.get('sanity_violations')}, unclassified="
                    f"{meas.get('unclassified_s')}, tolerance={meas.get('tolerance_s')})"
                )
            for key in ("total_s", "kernel_s", "phase1_s", "phase5_s"):
                try:
                    value = float(meas[key])
                except (KeyError, ValueError):
                    problems.append(f"{slug} chunk {chunk}: malformed {key}")
                    continue
                if value < 0:
                    problems.append(f"{slug} chunk {chunk}: negative {key}")
            record = dict(meas)
            record.update({
                "graph": graph,
                "cnf": cnf,
                "source_size": str(size),
                "chunk": str(chunk),
                "round": "0",
                "warmup": "0",
                "warmup_total_s": warm.get("total_s", ""),
                "warmup_sanity_ok": warm.get("sanity_ok", ""),
            })
            profile_rows.append(record)
            all_calls.append({
                "tag": tag, "call": meas.get("call", ""), "graph": graph, "cnf": cnf,
                "source_size": str(size), "chunk": str(chunk), "round": "0", "warmup": "0",
                "total_s": meas.get("total_s", ""), "sanity_ok": meas.get("sanity_ok", ""),
            })
            all_calls.append({
                "tag": tag, "call": warm.get("call", ""), "graph": graph, "cnf": cnf,
                "source_size": str(size), "chunk": str(chunk), "round": "0", "warmup": "1",
                "total_s": warm.get("total_s", ""), "sanity_ok": warm.get("sanity_ok", ""),
            })
        # annotate the kernel-written diagnostic CSVs with durable identity
        for suffix in (".detail.csv", ".matrix.csv", ".vector.csv"):
            annotate_profile_csv(out / "profile" / f"profile-{slug}.txt{suffix}",
                                 out / "profile" / f"profile-{slug}{suffix}")

    expected_list = sorted(expected)
    profile_problems, stats = accept_rows(results, expected_list, 1, "reference")
    problems.extend(profile_problems)

    if profile_rows:
        fieldnames: list[str] = []
        for row in profile_rows:
            for key in row:
                if key not in fieldnames:
                    fieldnames.append(key)
        with open(out / "profile.csv", "w", newline="") as file:
            writer = csv.DictWriter(file, fieldnames=fieldnames, lineterminator="\n")
            writer.writeheader()
            writer.writerows(profile_rows)
    if all_calls:
        fields = ["tag", "call", "graph", "cnf", "source_size", "chunk", "round", "warmup",
                  "total_s", "sanity_ok"]
        with open(out / "profile-calls.csv", "w", newline="") as file:
            writer = csv.DictWriter(file, fieldnames=fields, lineterminator="\n")
            writer.writeheader()
            writer.writerows(all_calls)
    for (display, key), stat in sorted(stats.items()):
        if stat["status"] != "ok":
            problems.append(f"profile result acceptance: {display} {key}: {stat['status']}")
    print(f"profile: {out / 'profile.csv'} ({len(profile_rows)} measured calls)")

    # one summary table entry per case so the printed status is useful
    failed = False
    for (display, key), stat in sorted(stats.items()):
        if stat["status"] != "ok":
            failed = True
    if problems:
        for problem in problems:
            print(f"PROFILE PROBLEM: {problem}", file=sys.stderr)
        return 1
    return 1 if failed else 0


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
    prepare_output_dir(args.out, args.force)

    input_paths = check_config(args.config, args.cfg_bench_dir)
    pairs = read_config_pairs(args.config)
    cfg_bench = build_cfg_bench(args)

    sources: dict[str, Path] = {}
    libraries: dict[str, Path] = {}
    if args.profile:
        profile_source = args.out / "profile" / "src" / "correctness_profile.c"
        profile_source.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [sys.executable, str(LAB_DIR / "tests" / "make_profile_variant.py"), str(args.baseline),
             str(profile_source)],
            check=True,
        )
        sources["baseline"] = variant_source("baseline", args, args.out)
        libraries["baseline"] = build_variant("baseline", sources["baseline"], args, args.out)
        sources["profile"] = profile_source
        libraries["profile"] = build_variant("profile", profile_source, args, args.out,
                                             extra_cflags="-DCFPQ_RSM_PROFILE")
        write_provenance(args, args.out, cfg_bench, args.config, libraries, sources, input_paths)
        # the reference for profile acceptance is the uninstrumented baseline build
        profile_libraries = {"reference": libraries["baseline"], "profile": libraries["profile"]}
        return run_profile(args, cfg_bench, args.out, pairs, profile_libraries)

    specs = [spec.strip() for spec in args.variants.split(",") if spec.strip()]
    if not specs:
        raise SystemExit("--variants is empty")
    counts: dict[str, int] = {}
    displays: list[tuple[str, str]] = []
    for spec in specs:
        counts[spec] = counts.get(spec, 0) + 1
        display = spec if counts[spec] == 1 else f"{spec}#{counts[spec]}"
        displays.append((display, spec))

    for display, spec in displays:
        if spec not in sources:
            sources[spec] = variant_source(spec, args, args.out)
            libraries[spec] = build_variant(spec, sources[spec], args, args.out)
        print(f"variant {display}: {libraries[spec]}", flush=True)

    baseline_display = next((display for display, spec in displays if spec == args.baseline_variant), None)
    if baseline_display is None:
        raise SystemExit(f"baseline variant '{args.baseline_variant}' is not in --variants")

    write_provenance(args, args.out, cfg_bench, args.config, libraries, sources, input_paths)
    if args.provenance_only:
        print(f"provenance: {args.out / 'provenance.txt'}")
        return 0

    keys = expected_keys(pairs, args)
    env = dict(os.environ)
    env["OMP_NUM_THREADS"] = str(args.threads)
    env["GRAPHBLAS_CACHE_PATH"] = str(args.jit_cache if args.jit_cache is not None else args.out / "jit-cache")
    (args.out / "jit-cache").mkdir(parents=True, exist_ok=True)

    results: dict[str, dict[tuple, dict[int, dict]]] = {display: {} for display, _ in displays}
    for round_index in range(args.rounds):
        offset = round_index % len(displays)
        order = displays[offset:] + displays[:offset]
        for display, spec in order:
            csv_path = args.out / "raw" / safe_name(display) / f"round{round_index}.csv"
            csv_path.parent.mkdir(parents=True, exist_ok=True)
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
            run(cmd, cwd=args.cfg_bench_dir,
                log_path=args.out / "logs" / safe_name(display) / f"round{round_index}.log",
                what=f"cfg_bench {display} round {round_index}", env=env)
            rows = read_rows(csv_path)
            if not rows:
                raise SystemExit(f"no result rows in {csv_path}")
            seen: set[tuple] = set()
            for row in rows:
                key = (row["graph"], row["cnf"], row["source_size"], row["chunk"])
                if key in seen:
                    raise SystemExit(f"duplicate case {key} in {csv_path}")
                seen.add(key)
                add_result_row(results, display, key, round_index, row)

    if keys is None:
        base_keys = set(results[baseline_display])
        expected_pairs = set(pairs)
        got_pairs = {(key[0], key[1]) for key in base_keys}
        if got_pairs != expected_pairs:
            raise SystemExit(
                f"case set mismatch: missing {sorted(expected_pairs - got_pairs)}, "
                f"unexpected {sorted(got_pairs - expected_pairs)}"
            )
        keys = sorted(base_keys)
    problems, stats = accept_rows(results, list(keys), args.rounds, baseline_display)

    base_copies = [display for display, spec in displays if spec == args.baseline_variant]
    noise = 0.0
    if len(base_copies) > 1:
        ratios = []
        for key in keys:
            base_times = [float(stats[(base_copies[0], key)]["times"][i])
                          for i in range(len(stats[(base_copies[0], key)]["times"]))
                          if i < len(stats[(base_copies[0], key)]["times"])]
            for other in base_copies[1:]:
                other_times = stats[(other, key)]["times"]
                a = stats[(base_copies[0], key)]["times"]
                if a and other_times and median(a) > 0:
                    ratios.append(median(other_times) / median(a))
        noise = max(ratios) - min(ratios) if len(ratios) > 1 else 0.0

    summary_rows: list[dict] = []
    per_variant: dict[str, dict] = {}
    for display, _ in displays:
        ratios = []
        worst = 0.0
        best = math.inf
        medians = []
        memories = 0.0
        statuses: list[str] = []
        for key in keys:
            stat = stats.get((display, key))
            if stat is None:
                statuses.append(f"MISSING:{key}")
                continue
            if stat["status"] != "ok":
                statuses.append(f"{stat['status']}:{key}")
            times = stat["times"]
            ref_times = stat["ref_times"]
            ratio = median(times) / median(ref_times) if times and ref_times and median(ref_times) > 0 else 0.0
            if ratio > 0:
                ratios.append(ratio)
                worst = max(worst, ratio)
                best = min(best, ratio)
            medians.append(median(times))
            memory = max(stat["memories"], default=0.0)
            memories = max(memories, memory)
            summary_rows.append({
                "variant": display,
                "graph": key[0],
                "cnf": key[1],
                "source_size": key[2],
                "chunk": key[3],
                "rounds_seen": len(stat["seen_rounds"]),
                "n_runs": len(stat["seen_rounds"]),
                "ref_median_s": f"{median(ref_times):.9f}",
                "median_s": f"{median(times):.9f}",
                "iqr_s": f"{iqr(times):.9f}",
                "min_s": f"{min(times):.9f}" if times else "0",
                "max_s": f"{max(times):.9f}" if times else "0",
                "ratio": f"{ratio:.6f}",
                "result": max(stat["counts"].values()) if stat["counts"] else 0,
                "result_hash": max(stat["hashes"].values()) if stat["hashes"] else 0,
                "count_ok": int(stat["count_ok"]),
                "hash_ok": int(stat["hash_ok"]),
                "status": stat["status"],
                "median_memory_kb": f"{median(stat['memories']):.1f}" if stat["memories"] else "0",
                "min_memory_kb": f"{min(stat['memories']):.1f}" if stat["memories"] else "0",
                "max_memory_kb": f"{max(stat['memories']):.1f}" if stat["memories"] else "0",
                "memory_scope": "cfg_bench process peak (malloc_trim reset per run)",
            })
        per_variant[display] = {
            "geo": math.exp(sum(math.log(ratio) for ratio in ratios) / len(ratios)) if ratios else 0.0,
            "best": best if best != math.inf else 0.0,
            "worst": worst,
            "median_ms": median(medians) * 1000.0,
            "memory_mb": memories / 1024.0,
            "statuses": statuses,
        }

    summary_path = args.out / "summary.csv"
    fieldnames = [
        "variant", "graph", "cnf", "source_size", "chunk", "rounds_seen", "n_runs", "ref_median_s",
        "median_s", "iqr_s", "min_s", "max_s", "ratio", "result", "result_hash", "count_ok", "hash_ok",
        "status", "median_memory_kb", "min_memory_kb", "max_memory_kb", "memory_scope",
    ]
    with open(summary_path, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(summary_rows)

    print()
    print(f"{'variant':24} {'geo_ratio':>9} {'min':>7} {'max':>7} {'median_ms':>10} {'mem_MB':>8}  status")
    failed = False
    for display, _ in displays:
        stats_v = per_variant[display]
        if stats_v["statuses"]:
            failed = True
            status = " ".join(stats_v["statuses"][:3])
            if len(stats_v["statuses"]) > 3:
                status += f" (+{len(stats_v['statuses']) - 3})"
        else:
            status = "ok"
        print(f"{display:24} {stats_v['geo']:9.3f} {stats_v['best']:7.3f} {stats_v['worst']:7.3f} "
              f"{stats_v['median_ms']:10.3f} {stats_v['memory_mb']:8.1f}  {status}")
    if len(base_copies) > 1:
        print(f"\nduplicate-baseline variation (max/min ratio spread over cases): {noise:.3f}")
    print()
    print(f"summary:  {summary_path}")
    print(f"raw:      {args.out / 'raw'}")
    print(f"logs:     {args.out / 'logs'}")
    print(f"provenance: {args.out / 'provenance.txt'}")
    if problems:
        print(f"\nACCEPTANCE FAILURES ({len(problems)}):", file=sys.stderr)
        for problem in problems[:200]:
            print(f"  {problem}", file=sys.stderr)
        if len(problems) > 200:
            print(f"  ... and {len(problems) - 200} more", file=sys.stderr)
    return 1 if (failed or problems) else 0


if __name__ == "__main__":
    raise SystemExit(main())
