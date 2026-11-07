#!/usr/bin/env python3
"""Summarize an interrupted or completed run_r10_overnight.sh output root.

Usage:
    python3 summarize_r10_overnight.py RUN_ROOT

Reads RUN_ROOT/status.tsv, each comparison's summary.csv, and the retained raw
cfg_bench rows.  Writes RUN_ROOT/morning_summary.csv and RUN_ROOT/morning_summary.md
with one row per variant / graph / grammar / source size (273 rows for the
current seven-pair lab.csv, thirteen variants, and three source sizes),
including failed and missing data.

MATCH requires, for the workload, all 30 chunks x 5 measured rounds with
matching counts and hashes, an "ok" row in that comparison's summary.csv, and a
zero run exit code.  Partial data, a missing comparison, or a missing
summary.csv is never a pass.  Rows with no retained data are NOT_RUN; rows with
partial or mismatching data are FAIL.  Partial timings are kept but marked
incomplete.

No winner is selected or combined here.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import re
import statistics
import sys
from pathlib import Path

LAB_DIR = Path(__file__).resolve().parent
CONFIG = LAB_DIR / "configs" / "lab.csv"

VARIANTS = (
    "ds36_pair",
    "ds37_fused",
    "ds38_no_product_clear",
    "ds39_direct_calls",
    "ds30_assign",
    "ds31_joint",
    "ds32_empty",
    "ds33_format",
    "ds34_lazy_p",
    "ds35_lazy_nt",
    "d1_native",
    "config_block",
    "d3_masked_product",
)
SOURCES = (1, 10, 100)
CHUNKS = 30
ROUNDS = 5
BASELINE_DISPLAY = "baseline"
BASELINE_COPY_DISPLAY = "baseline#2"

SUMMARY_FIELDS = [
    "variant",
    "graph",
    "grammar",
    "source_size",
    "status",
    "matched_chunks",
    "expected_chunks",
    "matched_rounds",
    "expected_rounds",
    "run_exit_code",
    "baseline_median_ms",
    "variant_median_ms",
    "time_ratio",
    "baseline_peak_process_memory_mib",
    "variant_peak_process_memory_mib",
    "timing_complete",
    "memory_scope",
    "failure_reason",
    "log",
    "output_dir",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run_root", type=Path, help="run_r10_overnight.sh output root")
    return parser.parse_args()


def safe_name(name: str) -> str:
    """Must match run.py's raw/ directory naming."""
    return re.sub(r"[^A-Za-z0-9_.+-]", "_", name)


def read_config_pairs(path: Path) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 2:
            raise SystemExit(f"{path}: expected at least graph,grammar")
        pairs.append((fields[0], fields[1]))
    if not pairs:
        raise SystemExit(f"{path}: no cases")
    return pairs


def read_rows(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    with open(path, newline="") as file:
        return [row for row in csv.DictReader(file) if row.get("algo_name") != "algo_name"]


def to_float(value) -> float | None:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if result == result else None  # reject NaN


def to_int(value) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def median(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def fmt(value: float | None, digits: int = 3) -> str:
    return "" if value is None else f"{value:.{digits}f}"


def relative_link(path: Path, root: Path) -> str:
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path.resolve())


def load_status(root: Path) -> dict[tuple[str, str], dict]:
    status_path = root / "status.tsv"
    if not status_path.is_file():
        raise SystemExit(f"missing {status_path}; cannot summarize an unattempted run")
    statuses: dict[tuple[str, str], dict] = {}
    with open(status_path, newline="") as file:
        reader = csv.DictReader(file, delimiter="\t")
        required = {"variant", "sources", "start", "finish", "exit_code", "status",
                    "output_dir", "log"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise SystemExit(
                f"{status_path}: expected tab-separated columns {sorted(required)}, "
                f"found {reader.fieldnames}"
            )
        for row in reader:
            variant = (row.get("variant") or "").strip()
            sources = (row.get("sources") or "").strip()
            if not variant or not sources:
                continue
            statuses[(variant, sources)] = row
    return statuses


def comparison_paths(root: Path, variant: str, sources: int, status_row: dict | None) -> tuple[Path, Path]:
    fallback_out = root / variant / f"s{sources}"
    fallback_log = root / variant / f"s{sources}.console.log"
    out = fallback_out
    log = fallback_log
    if status_row is not None:
        raw_out = (status_row.get("output_dir") or "").strip()
        raw_log = (status_row.get("log") or "").strip()
        if raw_out:
            candidate = Path(raw_out)
            out = candidate if candidate.is_absolute() else (root / candidate)
        if raw_log:
            candidate = Path(raw_log)
            log = candidate if candidate.is_absolute() else (root / candidate)
    # tolerate a moved run root: fall back to the standard layout if the
    # recorded paths no longer exist
    if not out.is_dir():
        out = fallback_out
    if not log.is_file():
        log = fallback_log
    return out.resolve(), log.resolve()


def load_raw(out: Path, displays: list[str]) -> dict[str, dict[tuple, dict[int, dict]]]:
    """raw[display][(graph,cnf,source_size,chunk)][round] -> cfg_bench row."""
    raw: dict[str, dict[tuple, dict[int, dict]]] = {display: {} for display in displays}
    for display in displays:
        for round_id in range(ROUNDS):
            path = out / "raw" / safe_name(display) / f"round{round_id}.csv"
            for row in read_rows(path):
                key = (
                    row.get("graph"),
                    row.get("cnf"),
                    str(row.get("source_size", "")),
                    str(row.get("chunk", "")),
                )
                if key[0] is None or key[1] is None:
                    continue
                raw[display].setdefault(key, {})[round_id] = row
    return raw


def load_summary(out: Path) -> tuple[bool, dict[tuple, dict]]:
    path = out / "summary.csv"
    if not path.is_file():
        return False, {}
    rows: dict[tuple, dict] = {}
    for row in read_rows(path):
        key = (
            row.get("variant"),
            row.get("graph"),
            row.get("cnf"),
            str(row.get("source_size", "")),
            str(row.get("chunk", "")),
        )
        rows[key] = row
    return True, rows


def round_result(row: dict) -> tuple[int | None, int | None, float | None]:
    return (
        to_int(row.get("result")),
        to_int(row.get("result_hash")),
        to_float(row.get("time_s")),
    )


def evaluate_workload(
    variant: str,
    sources: int,
    graph: str,
    cnf: str,
    out: Path,
    log: Path,
    status_row: dict | None,
    raw: dict[str, dict[tuple, dict[int, dict]]],
    summary_exists: bool,
    summary: dict[tuple, dict],
) -> dict:
    exit_code = to_int(status_row.get("exit_code")) if status_row is not None else None
    issues: list[str] = []

    matched_chunks = 0
    matched_rounds = 0
    full_timing_chunks = 0
    any_data = False
    baseline_chunk_medians: list[float] = []
    variant_chunk_medians: list[float] = []
    chunk_ratios: list[float] = []
    baseline_memories: list[float] = []
    variant_memories: list[float] = []

    base_raw = raw.get(BASELINE_DISPLAY, {})
    variant_raw = raw.get(variant, {})

    for chunk in range(CHUNKS):
        key = (graph, cnf, str(sources), str(chunk))
        base_rounds = base_raw.get(key, {})
        variant_rounds = variant_raw.get(key, {})
        summary_row = summary.get((variant, graph, cnf, str(sources), str(chunk)))

        if base_rounds or variant_rounds or summary_row is not None:
            any_data = True

        # --- timing ---
        base_times = [to_float(row.get("time_s")) for row in base_rounds.values()]
        variant_times = [to_float(row.get("time_s")) for row in variant_rounds.values()]
        base_times = [value for value in base_times if value is not None]
        variant_times = [value for value in variant_times if value is not None]
        base_median = median(base_times)
        variant_median = median(variant_times)
        if base_median is not None:
            baseline_chunk_medians.append(base_median)
        if variant_median is not None:
            variant_chunk_medians.append(variant_median)
        if base_median is not None and variant_median is not None and base_median > 0:
            chunk_ratios.append(variant_median / base_median)
        if len(base_times) == ROUNDS and len(variant_times) == ROUNDS:
            full_timing_chunks += 1

        # --- process memory ---
        for row in base_rounds.values():
            value = to_float(row.get("max_memory_kb"))
            if value is not None:
                baseline_memories.append(value)
        for row in variant_rounds.values():
            value = to_float(row.get("max_memory_kb"))
            if value is not None:
                variant_memories.append(value)

        # --- round-by-round counts and hashes ---
        rounds_seen = sorted(variant_rounds)
        base_seen = sorted(base_rounds)
        chunk_issues: list[str] = []

        rounds_ok = 0
        variant_counts: set[int] = set()
        variant_hashes: set[int] = set()
        for round_id in range(ROUNDS):
            if round_id not in variant_rounds:
                chunk_issues.append(f"chunk {chunk}: variant missing round {round_id}")
                continue
            if round_id not in base_rounds:
                chunk_issues.append(f"chunk {chunk}: baseline missing round {round_id}")
                continue
            variant_count, variant_hash, variant_time = round_result(variant_rounds[round_id])
            base_count, base_hash, base_time = round_result(base_rounds[round_id])
            if None in (variant_count, variant_hash, base_count, base_hash):
                chunk_issues.append(f"chunk {chunk}: malformed count/hash in round {round_id}")
                continue
            if variant_time is None or base_time is None:
                chunk_issues.append(f"chunk {chunk}: malformed time in round {round_id}")
                continue
            variant_counts.add(variant_count)
            variant_hashes.add(variant_hash)
            if variant_hash == 0:
                chunk_issues.append(f"chunk {chunk}: variant zero hash in round {round_id}")
                continue
            if variant_count != base_count:
                chunk_issues.append(
                    f"chunk {chunk}: count mismatch in round {round_id} "
                    f"(variant {variant_count}, baseline {base_count})")
                continue
            if variant_hash != base_hash:
                chunk_issues.append(
                    f"chunk {chunk}: hash mismatch in round {round_id} "
                    f"(variant {variant_hash}, baseline {base_hash})")
                continue
            rounds_ok += 1

        if len(variant_counts) > 1:
            chunk_issues.append(f"chunk {chunk}: unstable variant counts")
        if len(variant_hashes) > 1:
            chunk_issues.append(f"chunk {chunk}: unstable variant hashes")

        # --- summary.csv cross-check ---
        summary_ok = False
        if not summary_exists:
            chunk_issues.append(f"chunk {chunk}: summary.csv missing")
        elif summary_row is None:
            chunk_issues.append(f"chunk {chunk}: no summary.csv row")
        else:
            rounds_seen_summary = to_int(summary_row.get("rounds_seen"))
            count_ok = to_int(summary_row.get("count_ok"))
            hash_ok = to_int(summary_row.get("hash_ok"))
            summary_ok = (
                summary_row.get("status") == "ok"
                and rounds_seen_summary == ROUNDS
                and count_ok == 1
                and hash_ok == 1
            )
            if not summary_ok:
                chunk_issues.append(
                    f"chunk {chunk}: summary status {summary_row.get('status')!r} "
                    f"rounds {rounds_seen_summary} count_ok={count_ok} hash_ok={hash_ok}")

        rounds_match = (
            rounds_ok == ROUNDS
            and rounds_seen == list(range(ROUNDS))
            and base_seen == list(range(ROUNDS))
        )
        matched_rounds += rounds_ok
        if rounds_match and summary_ok:
            matched_chunks += 1
        else:
            issues.extend(chunk_issues)

    baseline_ms = None
    variant_ms = None
    time_ratio = None
    if baseline_chunk_medians:
        baseline_ms = median(baseline_chunk_medians) * 1000.0
    if variant_chunk_medians:
        variant_ms = median(variant_chunk_medians) * 1000.0
    if chunk_ratios:
        time_ratio = median(chunk_ratios)
    baseline_memory = max(baseline_memories) / 1024.0 if baseline_memories else None
    variant_memory = max(variant_memories) / 1024.0 if variant_memories else None
    timing_complete = full_timing_chunks == CHUNKS

    # --- status ---
    if not out.is_dir():
        status = "NOT_RUN"
        reason = f"comparison output directory missing: {out}"
    elif not any_data:
        status = "NOT_RUN"
        if status_row is None:
            reason = "comparison was not attempted (no status.tsv row)"
        else:
            reason = "no retained data for this workload"
    elif matched_chunks == CHUNKS and exit_code == 0 and summary_exists:
        status = "MATCH"
        reason = ""
    else:
        status = "FAIL"
        reasons: list[str] = []
        if status_row is None:
            reasons.append("comparison did not finish (no status.tsv row)")
        elif exit_code != 0:
            reasons.append(f"runner exit {exit_code}")
        if not summary_exists:
            reasons.append("summary.csv missing")
        reasons.extend(issues[:3])
        if len(issues) > 3:
            reasons.append(f"+{len(issues) - 3} more case problem(s)")
        if not timing_complete:
            reasons.append(f"timing incomplete ({full_timing_chunks}/{CHUNKS} chunks)")
        reason = "; ".join(reasons) if reasons else "did not meet MATCH requirements"

    return {
        "variant": variant,
        "graph": graph,
        "grammar": cnf,
        "source_size": sources,
        "status": status,
        "matched_chunks": matched_chunks,
        "expected_chunks": CHUNKS,
        "matched_rounds": matched_rounds,
        "expected_rounds": CHUNKS * ROUNDS,
        "run_exit_code": "" if exit_code is None else exit_code,
        "baseline_median_ms": fmt(baseline_ms),
        "variant_median_ms": fmt(variant_ms),
        "time_ratio": fmt(time_ratio, 6),
        "baseline_peak_process_memory_mib": fmt(baseline_memory, 1),
        "variant_peak_process_memory_mib": fmt(variant_memory, 1),
        "timing_complete": "yes" if timing_complete else "no",
        "memory_scope": "cfg_bench process peak (max max_memory_kb over retained measured rows / 1024)",
        "failure_reason": reason,
        "log": str(log),
        "output_dir": str(out),
    }


def duplicate_baseline_noise(
    root: Path,
    comparisons: list[tuple[str, int, dict | None, Path]],
) -> list[dict]:
    """Per-chunk T(baseline#2)/T(baseline) for each attempted comparison."""
    by_size: dict[int, list[float]] = {size: [] for size in SOURCES}
    all_ratios: list[float] = []
    for _variant, sources, _status_row, out in comparisons:
        raw = load_raw(out, [BASELINE_DISPLAY, BASELINE_COPY_DISPLAY])
        base = raw[BASELINE_DISPLAY]
        copy = raw[BASELINE_COPY_DISPLAY]
        for key, base_rounds in base.items():
            copy_rounds = copy.get(key, {})
            if not copy_rounds:
                continue
            base_times = [to_float(row.get("time_s")) for row in base_rounds.values()]
            copy_times = [to_float(row.get("time_s")) for row in copy_rounds.values()]
            base_values = [value for value in base_times if value is not None]
            copy_values = [value for value in copy_times if value is not None]
            if not base_values or not copy_values:
                continue
            base_median = median(base_values)
            copy_median = median(copy_values)
            if base_median is None or copy_median is None or base_median <= 0:
                continue
            ratio = copy_median / base_median
            all_ratios.append(ratio)
            by_size[sources].append(ratio)

    rows: list[dict] = []
    for sources in SOURCES:
        values = by_size[sources]
        rows.append({
            "sources": sources,
            "chunks": len(values),
            "min": fmt(min(values), 4) if values else "",
            "median": fmt(median(values), 4) if values else "",
            "max": fmt(max(values), 4) if values else "",
        })
    rows.append({
        "sources": "all",
        "chunks": len(all_ratios),
        "min": fmt(min(all_ratios), 4) if all_ratios else "",
        "median": fmt(median(all_ratios), 4) if all_ratios else "",
        "max": fmt(max(all_ratios), 4) if all_ratios else "",
    })
    return rows


def write_markdown(
    path: Path,
    root: Path,
    pairs: list[tuple[str, str]],
    rows: list[dict],
    noise_rows: list[dict],
) -> None:
    lines: list[str] = []
    lines.append("# CFPQ RSM overnight summary")
    lines.append("")
    lines.append(f"- Run root: `{root}`")
    lines.append(f"- Generated: {datetime.datetime.now().astimezone().isoformat()}")
    lines.append(f"- Config: `{CONFIG}` ({len(pairs)} graph/grammar pairs)")
    lines.append(
        f"- Expected rows: {len(VARIANTS)} variants x {len(SOURCES)} source sizes x "
        f"{len(pairs)} workloads = {len(rows)}")
    lines.append(
        "- `MATCH` requires all 30 chunks x 5 measured rounds for the workload, matching "
        "counts and hashes, an `ok` row in that comparison's `summary.csv`, and a zero "
        "run exit code.")
    lines.append(
        "- Times are milliseconds: median of the per-chunk median times. `ratio` is the "
        "median across chunks of `T_variant / T_first_baseline` (smaller is better).")
    lines.append(
        "- Memory is **process memory**: the maximum `max_memory_kb` over retained measured "
        "rows for the cfg_bench process, converted to MiB.")
    lines.append(
        "- No winner is selected or combined; failed/incomplete rows are excluded from the "
        "descriptive aggregates. Partial timings are kept but marked incomplete.")
    lines.append("")

    # status overview
    lines.append("## Status overview")
    lines.append("")
    lines.append("| variant | sources | MATCH | FAIL | NOT_RUN | median ratio (MATCH rows) |")
    lines.append("| --- | --- | --- | --- | --- | --- |")
    for variant in VARIANTS:
        for sources in SOURCES:
            subset = [row for row in rows
                      if row["variant"] == variant and row["source_size"] == sources]
            matches = sum(1 for row in subset if row["status"] == "MATCH")
            fails = sum(1 for row in subset if row["status"] == "FAIL")
            not_run = sum(1 for row in subset if row["status"] == "NOT_RUN")
            ratios = [float(row["time_ratio"]) for row in subset
                      if row["status"] == "MATCH" and row["time_ratio"]]
            lines.append(
                f"| {variant} | {sources} | {matches} | {fails} | {not_run} | "
                f"{fmt(median(ratios), 3) if ratios else ''} |")
    lines.append("")

    # per-workload rows
    lines.append("## Per-workload results")
    lines.append("")
    lines.append(
        "| variant | graph | grammar | sources | status | chunks | rounds | exit | "
        "baseline ms | variant ms | ratio | baseline mem MiB | variant mem MiB | timing | reason |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for row in rows:
        baseline_ms = row["baseline_median_ms"]
        variant_ms = row["variant_median_ms"]
        if row["timing_complete"] == "no":
            if baseline_ms:
                baseline_ms += " (incomplete)"
            if variant_ms:
                variant_ms += " (incomplete)"
        reason = (row["failure_reason"] or "").replace("|", "\\|")
        lines.append(
            f"| {row['variant']} | {Path(row['graph']).name} | {Path(row['grammar']).name} | "
            f"{row['source_size']} | {row['status']} | "
            f"{row['matched_chunks']}/{row['expected_chunks']} | "
            f"{row['matched_rounds']}/{row['expected_rounds']} | "
            f"{row['run_exit_code']} | {baseline_ms} | {variant_ms} | "
            f"{row['time_ratio']} | {row['baseline_peak_process_memory_mib']} | "
            f"{row['variant_peak_process_memory_mib']} | {row['timing_complete']} | {reason} |")
    lines.append("")

    # duplicate-baseline noise
    lines.append("## Duplicate-baseline noise context")
    lines.append("")
    lines.append(
        "Per-chunk ratio of the second baseline copy to the first "
        "(`T_baseline#2 / T_baseline`), using per-chunk medians. A ratio of 1.0 means "
        "identical timing; the spread is measurement noise, not a variant result.")
    lines.append("")
    lines.append("| sources | chunks compared | min | median | max |")
    lines.append("| --- | --- | --- | --- | --- |")
    for row in noise_rows:
        lines.append(
            f"| {row['sources']} | {row['chunks']} | {row['min']} | "
            f"{row['median']} | {row['max']} |")
    lines.append("")

    # failures and missing data
    lines.append("## Failures and missing data")
    lines.append("")
    bad_rows = [row for row in rows if row["status"] != "MATCH"]
    if not bad_rows:
        lines.append(f"None: all {len(rows)} rows are MATCH.")
    else:
        lines.append("| variant | sources | graph | grammar | status | reason | log | output |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for row in bad_rows:
            reason = (row["failure_reason"] or "").replace("|", "\\|")
            log_link = relative_link(Path(row["log"]), root)
            out_link = relative_link(Path(row["output_dir"]), root)
            lines.append(
                f"| {row['variant']} | {row['source_size']} | {Path(row['graph']).name} | "
                f"{Path(row['grammar']).name} | {row['status']} | {reason} | "
                f"[log]({log_link}) | [output]({out_link}) |")
    lines.append("")

    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    root = args.run_root.resolve()
    if not root.is_dir():
        print(f"error: run root is not a directory: {root}", file=sys.stderr)
        return 2
    if not CONFIG.is_file():
        print(f"error: missing config: {CONFIG}", file=sys.stderr)
        return 2

    pairs = read_config_pairs(CONFIG)
    statuses = load_status(root)

    comparisons: list[tuple[str, int, dict | None, Path]] = []
    rows: list[dict] = []
    for variant in VARIANTS:
        for sources in SOURCES:
            status_row = statuses.get((variant, str(sources)))
            out, log = comparison_paths(root, variant, sources, status_row)
            comparisons.append((variant, sources, status_row, out))
            summary_exists, summary = load_summary(out)
            raw = load_raw(out, [BASELINE_DISPLAY, BASELINE_COPY_DISPLAY, variant]) if out.is_dir() else {
                BASELINE_DISPLAY: {}, BASELINE_COPY_DISPLAY: {}, variant: {}}
            for graph, cnf in pairs:
                rows.append(evaluate_workload(
                    variant, sources, graph, cnf, out, log, status_row, raw,
                    summary_exists, summary))

    noise_rows = duplicate_baseline_noise(root, comparisons)

    csv_path = root / "morning_summary.csv"
    with open(csv_path, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=SUMMARY_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    md_path = root / "morning_summary.md"
    write_markdown(md_path, root, pairs, rows, noise_rows)

    counts = {status: sum(1 for row in rows if row["status"] == status)
              for status in ("MATCH", "FAIL", "NOT_RUN")}
    print(f"morning_summary.csv: {csv_path} ({len(rows)} rows)")
    print(f"morning_summary.md:  {md_path}")
    print(f"MATCH={counts['MATCH']} FAIL={counts['FAIL']} NOT_RUN={counts['NOT_RUN']}")
    missing_comparisons = [
        f"{variant}/s{sources}" for variant in VARIANTS for sources in SOURCES
        if statuses.get((variant, str(sources))) is None
    ]
    if missing_comparisons:
        print(f"no status.tsv row (not finished or not attempted): {', '.join(missing_comparisons)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
