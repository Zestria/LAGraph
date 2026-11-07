#!/usr/bin/env python3
"""Summarize the combined current-vs-bb7 comparison.

Usage:
    python3 summarize_current_vs_bb7.py OUTPUT_ROOT

Reads the single shared OUTPUT_ROOT/execution_status.tsv, summary.csv and
raw/<safe_display>/roundN.csv for the historical baseline, the current
production RSM (`current`) and the built-in fully optimized CFL comparator
(`CFL_multsrc_adv`).

Writes 16 candidate/workload rows (2 candidates x 8 lab_batik workloads) to
morning_summary.csv and morning_summary.md.  There is exactly one baseline
series; no baseline#2 is expected.

`current` is checked strictly against the historical baseline: 30 chunks x 5
measured rounds, stable nonzero hashes/counts and equality on every matching
case/round.  `CFL_multsrc_adv` is timing-only per the user's instruction: its
result count and hash are not checked (the CFG_bench adapter has no result hash
callback).  Its execution and timing completeness are still required.

Ratios use paired chunk medians: for each chunk, the median of its five rounds,
then the median of candidate/baseline chunk ratios.  A separate current-RSM /
CFL_multsrc_adv ratio uses the same paired medians (below 1 means current RSM
is faster).  Ratios are reported only for complete, validated cases.

The process returns nonzero unless every one of the 16 rows is MATCH.
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
CONFIG = LAB_DIR / "configs" / "lab_batik.csv"

BASELINE_DISPLAY = "baseline"
CFL_DISPLAY = "CFL_multsrc_adv"
CANDIDATES = ("current", "CFL_multsrc_adv")
SOURCE_SIZE = 100
CHUNKS = 30
ROUNDS = 5

SUMMARY_FIELDS = [
    "candidate",
    "graph",
    "grammar",
    "source_size",
    "status",
    "result_check",
    "matched_chunks",
    "expected_chunks",
    "matched_rounds",
    "expected_rounds",
    "candidate_execution_ok",
    "baseline_execution_ok",
    "baseline_median_ms",
    "candidate_median_ms",
    "time_ratio",
    "current_over_cfl_ratio",
    "reported_result",
    "reported_hash",
    "baseline_peak_process_memory_mib",
    "candidate_peak_process_memory_mib",
    "timing_complete",
    "memory_scope",
    "failure_reason",
    "log",
]

MEMORY_SCOPE = ("cfg_bench process peak (max max_memory_kb over retained measured rows / "
                "1024), not isolated CFPQ kernel memory")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output_root", type=Path,
                        help="run_current_vs_bb7.sh output root")
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
    return result if result == result else None


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


def load_execution(root: Path) -> tuple[bool, dict[tuple[str, int], dict]]:
    path = root / "execution_status.tsv"
    if not path.is_file():
        return False, {}
    rows: dict[tuple[str, int], dict] = {}
    with open(path, newline="") as file:
        reader = csv.DictReader(file, delimiter="\t")
        for row in reader:
            display = (row.get("display") or "").strip()
            round_text = (row.get("round") or "").strip()
            if not display or not round_text:
                continue
            try:
                round_id = int(round_text)
            except ValueError:
                continue
            rows[(display, round_id)] = row
    return True, rows


def load_raw(root: Path, displays: tuple[str, ...], source_size: int,
             rounds: int) -> dict[str, dict[tuple, dict[int, dict]]]:
    raw: dict[str, dict[tuple, dict[int, dict]]] = {display: {} for display in displays}
    for display in displays:
        for round_id in range(rounds):
            path = root / "raw" / safe_name(display) / f"round{round_id}.csv"
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


def load_summary(root: Path) -> tuple[bool, dict[tuple, dict]]:
    path = root / "summary.csv"
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


def display_attempted(display: str, raw: dict, summary: dict,
                      exec_status: dict) -> bool:
    if raw.get(display):
        return True
    if any(key[0] == display for key in exec_status):
        return True
    return any(key[0] == display for key in summary)


def display_execution(display: str, rounds: int, exec_exists: bool,
                      exec_status: dict) -> tuple[bool, list[str]]:
    if not exec_exists:
        return False, ["execution_status.tsv missing"]
    issues: list[str] = []
    for round_id in range(rounds):
        row = exec_status.get((display, round_id))
        if row is None:
            issues.append(f"round {round_id}: no execution_status.tsv row")
            continue
        outcome = (row.get("outcome") or "").strip()
        exit_code = to_int(row.get("exit_code"))
        if outcome != "ok":
            issues.append(f"round {round_id}: outcome {outcome!r} (exit {exit_code})")
        elif exit_code != 0:
            issues.append(f"round {round_id}: exit {exit_code}")
    return not issues, issues


def positive_times(rounds_map: dict[int, dict], rounds: int) -> list[float]:
    values = [to_float(rounds_map[r].get("time_s")) for r in range(rounds)
              if r in rounds_map]
    return [value for value in values if value is not None and value > 0]


def current_chunk_valid(current_rounds: dict[int, dict], baseline_rounds: dict[int, dict],
                        rounds: int) -> bool:
    """Strict count/hash validation of one current chunk against the baseline."""
    counts: set[int] = set()
    hashes: set[int] = set()
    for round_id in range(rounds):
        if round_id not in current_rounds or round_id not in baseline_rounds:
            return False
        count = to_int(current_rounds[round_id].get("result"))
        hash_value = to_int(current_rounds[round_id].get("result_hash"))
        base_count = to_int(baseline_rounds[round_id].get("result"))
        base_hash = to_int(baseline_rounds[round_id].get("result_hash"))
        if None in (count, hash_value, base_count, base_hash):
            return False
        if hash_value == 0 or count != base_count or hash_value != base_hash:
            return False
        counts.add(count)
        hashes.add(hash_value)
    return len(counts) == 1 and len(hashes) == 1


def current_over_cfl(raw: dict, graph: str, cnf: str, source_size: int,
                     chunks: int, rounds: int) -> float | None:
    """Paired current/CFL chunk-median ratio on complete, validated chunks."""
    current_raw = raw.get("current", {})
    cfl_raw = raw.get(CFL_DISPLAY, {})
    baseline_raw = raw.get(BASELINE_DISPLAY, {})
    ratios: list[float] = []
    for chunk in range(chunks):
        key = (graph, cnf, str(source_size), str(chunk))
        current_rounds = current_raw.get(key, {})
        cfl_rounds = cfl_raw.get(key, {})
        baseline_rounds = baseline_raw.get(key, {})
        if not current_chunk_valid(current_rounds, baseline_rounds, rounds):
            continue
        current_times = positive_times(current_rounds, rounds)
        cfl_times = positive_times(cfl_rounds, rounds)
        if len(current_times) != rounds or len(cfl_times) != rounds:
            continue
        current_median = median(current_times)
        cfl_median = median(cfl_times)
        if current_median and cfl_median and cfl_median > 0:
            ratios.append(current_median / cfl_median)
    return median(ratios)


def evaluate_workload(candidate: str, graph: str, cnf: str, root: Path, raw: dict,
                      summary_exists: bool, summary: dict, exec_exists: bool,
                      exec_status: dict, source_size: int, chunks: int, rounds: int,
                      candidate_attempted: bool, candidate_exec_ok: bool,
                      candidate_exec_issues: list[str], baseline_exec_ok: bool,
                      baseline_exec_issues: list[str], check_result: bool) -> dict:
    candidate_raw = raw.get(candidate, {})
    baseline_raw = raw.get(BASELINE_DISPLAY, {})
    issues: list[str] = []
    matched_chunks = 0
    matched_rounds = 0
    timing_chunks = 0
    any_data = False
    baseline_chunk_medians: list[float] = []
    candidate_chunk_medians: list[float] = []
    ratios: list[float] = []
    baseline_memories: list[float] = []
    candidate_memories: list[float] = []
    reported_counts: set[int] = set()
    reported_hashes: set[int] = set()

    for chunk in range(chunks):
        key = (graph, cnf, str(source_size), str(chunk))
        candidate_rounds = candidate_raw.get(key, {})
        baseline_rounds = baseline_raw.get(key, {})
        summary_row = summary.get((candidate, graph, cnf, str(source_size), str(chunk)))
        if candidate_rounds or baseline_rounds or summary_row is not None:
            any_data = True

        chunk_issues: list[str] = []
        rounds_ok = 0
        if check_result:
            counts: set[int] = set()
            hashes: set[int] = set()
            for round_id in range(rounds):
                if round_id not in candidate_rounds:
                    chunk_issues.append(f"chunk {chunk}: candidate missing round {round_id}")
                    continue
                if round_id not in baseline_rounds:
                    chunk_issues.append(f"chunk {chunk}: baseline missing round {round_id}")
                    continue
                count = to_int(candidate_rounds[round_id].get("result"))
                hash_value = to_int(candidate_rounds[round_id].get("result_hash"))
                base_count = to_int(baseline_rounds[round_id].get("result"))
                base_hash = to_int(baseline_rounds[round_id].get("result_hash"))
                if None in (count, hash_value, base_count, base_hash):
                    chunk_issues.append(f"chunk {chunk}: malformed count/hash in round {round_id}")
                    continue
                counts.add(count)
                hashes.add(hash_value)
                reported_counts.add(count)
                reported_hashes.add(hash_value)
                if hash_value == 0:
                    chunk_issues.append(f"chunk {chunk}: zero hash in round {round_id}")
                    continue
                if count != base_count:
                    chunk_issues.append(
                        f"chunk {chunk}: count mismatch in round {round_id} "
                        f"(candidate {count}, baseline {base_count})")
                    continue
                if hash_value != base_hash:
                    chunk_issues.append(
                        f"chunk {chunk}: hash mismatch in round {round_id} "
                        f"(candidate {hash_value}, baseline {base_hash})")
                    continue
                rounds_ok += 1
            if len(counts) > 1:
                chunk_issues.append(f"chunk {chunk}: unstable candidate counts")
            if len(hashes) > 1:
                chunk_issues.append(f"chunk {chunk}: unstable candidate hashes")
        else:
            # timing-only comparator: completeness only, no count/hash check
            for round_id in range(rounds):
                if round_id not in candidate_rounds:
                    chunk_issues.append(f"chunk {chunk}: comparator missing round {round_id}")
                    continue
                count = to_int(candidate_rounds[round_id].get("result"))
                hash_value = to_int(candidate_rounds[round_id].get("result_hash"))
                if count is not None:
                    reported_counts.add(count)
                if hash_value is not None:
                    reported_hashes.add(hash_value)
                rounds_ok += 1

        summary_ok = False
        if not summary_exists:
            chunk_issues.append(f"chunk {chunk}: summary.csv missing")
        elif summary_row is None:
            chunk_issues.append(f"chunk {chunk}: no summary.csv row")
        else:
            rounds_seen = to_int(summary_row.get("rounds_seen"))
            summary_ok = summary_row.get("status") == "ok" and rounds_seen == rounds
            if check_result:
                summary_ok = (
                    summary_ok
                    and to_int(summary_row.get("count_ok")) == 1
                    and to_int(summary_row.get("hash_ok")) == 1
                )
            if not summary_ok:
                chunk_issues.append(
                    f"chunk {chunk}: summary status {summary_row.get('status')!r} "
                    f"rounds {rounds_seen}")

        baseline_times = positive_times(baseline_rounds, rounds)
        candidate_times = positive_times(candidate_rounds, rounds)
        full_timing = len(baseline_times) == rounds and len(candidate_times) == rounds
        if len(baseline_times) == rounds:
            value = median(baseline_times)
            if value is not None:
                baseline_chunk_medians.append(value)
        if len(candidate_times) == rounds:
            value = median(candidate_times)
            if value is not None:
                candidate_chunk_medians.append(value)

        chunk_correct = rounds_ok == rounds and summary_ok and full_timing
        if full_timing:
            timing_chunks += 1
            baseline_median = median(baseline_times)
            candidate_median = median(candidate_times)
            if (chunk_correct and baseline_median is not None and candidate_median is not None
                    and baseline_median > 0):
                ratios.append(candidate_median / baseline_median)
        else:
            chunk_issues.append(f"chunk {chunk}: incomplete or non-positive timing")

        for row in baseline_rounds.values():
            value = to_float(row.get("max_memory_kb"))
            if value is not None:
                baseline_memories.append(value)
        for row in candidate_rounds.values():
            value = to_float(row.get("max_memory_kb"))
            if value is not None:
                candidate_memories.append(value)

        matched_rounds += rounds_ok
        if chunk_correct:
            matched_chunks += 1
        else:
            issues.extend(chunk_issues)

    timing_complete = timing_chunks == chunks
    baseline_ms = median(baseline_chunk_medians)
    candidate_ms = median(candidate_chunk_medians)
    baseline_memory = max(baseline_memories) / 1024.0 if baseline_memories else None
    candidate_memory = max(candidate_memories) / 1024.0 if candidate_memories else None

    if not candidate_attempted:
        status = "NOT_RUN"
        reason = "candidate display was not attempted"
    elif (matched_chunks == chunks and timing_complete and summary_exists
          and candidate_exec_ok and baseline_exec_ok):
        status = "MATCH"
        reason = ""
    else:
        status = "FAIL"
        reasons: list[str] = []
        if not candidate_exec_ok:
            reasons.extend(candidate_exec_issues[:2])
        if not baseline_exec_ok:
            reasons.extend(f"baseline: {item}" for item in baseline_exec_issues[:2])
        if not summary_exists:
            reasons.append("summary.csv missing")
        if not any_data:
            reasons.append("no retained data for this workload")
        reasons.extend(issues[:3])
        if len(issues) > 3:
            reasons.append(f"+{len(issues) - 3} more case problem(s)")
        if not timing_complete:
            reasons.append(f"timing incomplete ({timing_chunks}/{chunks} chunks)")
        reason = "; ".join(reasons) if reasons else "did not meet MATCH requirements"

    log_dir = root / "logs" / safe_name(candidate)
    log_path = log_dir
    if exec_exists:
        for round_id in range(rounds):
            row = exec_status.get((candidate, round_id))
            if row is not None and (row.get("outcome") != "ok"
                                    or to_int(row.get("exit_code")) != 0):
                recorded = (row.get("log") or "").strip()
                log_path = Path(recorded) if recorded else log_dir / f"round{round_id}.log"
                break

    return {
        "candidate": candidate,
        "graph": graph,
        "grammar": cnf,
        "source_size": source_size,
        "status": status,
        "result_check": "strict count/hash" if check_result else "timing-only",
        "matched_chunks": matched_chunks,
        "expected_chunks": chunks,
        "matched_rounds": matched_rounds,
        "expected_rounds": chunks * rounds,
        "candidate_execution_ok": int(candidate_exec_ok),
        "baseline_execution_ok": int(baseline_exec_ok),
        "baseline_median_ms": fmt(baseline_ms * 1000.0 if baseline_ms is not None else None),
        "candidate_median_ms": fmt(candidate_ms * 1000.0 if candidate_ms is not None else None),
        "time_ratio": fmt(median(ratios) if status == "MATCH" else None, 6),
        "current_over_cfl_ratio": "",
        "reported_result": max(reported_counts) if reported_counts else "",
        "reported_hash": max(reported_hashes) if reported_hashes else "",
        "baseline_peak_process_memory_mib": fmt(baseline_memory, 1),
        "candidate_peak_process_memory_mib": fmt(candidate_memory, 1),
        "timing_complete": "yes" if timing_complete else "no",
        "memory_scope": MEMORY_SCOPE,
        "failure_reason": reason,
        "log": str(log_path),
    }


def build_rows(root: Path, pairs: list[tuple[str, str]],
               candidates: tuple[str, ...] = CANDIDATES,
               source_size: int = SOURCE_SIZE, chunks: int = CHUNKS,
               rounds: int = ROUNDS) -> list[dict]:
    displays = (BASELINE_DISPLAY,) + tuple(candidates)
    exec_exists, exec_status = load_execution(root)
    summary_exists, summary = load_summary(root)
    raw = load_raw(root, displays, source_size, rounds)

    baseline_exec_ok, baseline_exec_issues = display_execution(
        BASELINE_DISPLAY, rounds, exec_exists, exec_status)
    attempted = {candidate: display_attempted(candidate, raw, summary, exec_status)
                 for candidate in candidates}

    rows: list[dict] = []
    for candidate in candidates:
        check_result = candidate != CFL_DISPLAY
        candidate_exec_ok, candidate_exec_issues = display_execution(
            candidate, rounds, exec_exists, exec_status)
        for graph, cnf in pairs:
            rows.append(evaluate_workload(
                candidate, graph, cnf, root, raw, summary_exists, summary,
                exec_exists, exec_status, source_size, chunks, rounds,
                attempted[candidate], candidate_exec_ok, candidate_exec_issues,
                baseline_exec_ok, baseline_exec_issues, check_result))

    # current/CFL ratio per workload, only when both candidate rows are MATCH
    by_pair: dict[tuple[str, str], dict[str, dict]] = {}
    for row in rows:
        by_pair.setdefault((row["graph"], row["grammar"]), {})[row["candidate"]] = row
    for (graph, cnf), pair_rows in by_pair.items():
        current_row = pair_rows.get("current")
        cfl_row = pair_rows.get(CFL_DISPLAY)
        if (current_row is None or cfl_row is None
                or current_row["status"] != "MATCH" or cfl_row["status"] != "MATCH"):
            continue
        ratio = current_over_cfl(raw, graph, cnf, source_size, chunks, rounds)
        label = fmt(ratio, 6)
        current_row["current_over_cfl_ratio"] = label
        cfl_row["current_over_cfl_ratio"] = label
    return rows


def write_markdown(path: Path, root: Path, pairs: list[tuple[str, str]],
                   rows: list[dict]) -> None:
    lines: list[str] = []
    lines.append("# CFPQ RSM: current vs bb7dc96e, plus optimized CFL_multsrc_adv")
    lines.append("")
    lines.append(f"- Run root: `{root}`")
    lines.append(f"- Generated: {datetime.datetime.now().astimezone().isoformat()}")
    lines.append(f"- Config: `{CONFIG}` ({len(pairs)} graph/grammar pairs, including Batik)")
    lines.append(f"- Candidates: {', '.join(CANDIDATES)}")
    lines.append(f"- Source size: {SOURCE_SIZE}; {CHUNKS} chunks; {ROUNDS} measured rounds")
    lines.append(f"- Expected rows: {len(CANDIDATES)} candidates x {len(pairs)} workloads "
                 f"= {len(rows)}")
    lines.append("- The historical baseline (commit bb7dc96e) is a single series shared by "
                 "all workloads; there is no baseline#2 and no drift estimate.")
    lines.append("- `current` is checked strictly against the historical baseline. "
                 "`CFL_multsrc_adv` is **timing-only** per the user's instruction: its "
                 "result count and hash are not checked (the CFG_bench adapter has no "
                 "result-hash callback).")
    lines.append("- `time_ratio` is candidate/baseline (below 1 means the candidate is "
                 "faster). `current_over_cfl_ratio` is current-RSM/CFL (below 1 means "
                 "current RSM is faster). Both use paired chunk medians.")
    lines.append("- Ratios are reported only for complete validated cases; failed or "
                 "incomplete comparisons show no ratio.")
    lines.append(f"- Memory is **process memory**: {MEMORY_SCOPE}.")
    lines.append("- No winner is selected; all eight workloads, including regressions, are "
                 "reported.")
    lines.append("")

    lines.append("## Status overview")
    lines.append("")
    lines.append("| candidate | MATCH | FAIL | NOT_RUN | median time_ratio (MATCH) |")
    lines.append("| --- | --- | --- | --- | --- |")
    for candidate in CANDIDATES:
        subset = [row for row in rows if row["candidate"] == candidate]
        matches = sum(1 for row in subset if row["status"] == "MATCH")
        fails = sum(1 for row in subset if row["status"] == "FAIL")
        not_run = sum(1 for row in subset if row["status"] == "NOT_RUN")
        ratios = [float(row["time_ratio"]) for row in subset
                  if row["status"] == "MATCH" and row["time_ratio"]]
        lines.append(f"| {candidate} | {matches} | {fails} | {not_run} | "
                     f"{fmt(median(ratios), 3)} |")
    lines.append("")

    lines.append("## Per-workload results")
    lines.append("")
    lines.append("| candidate | graph | grammar | status | check | chunks | rounds | "
                 "baseline ms | candidate ms | time_ratio | baseline mem MiB | "
                 "candidate mem MiB | timing | reason |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | "
                 "--- | --- | --- |")
    for row in rows:
        reason = (row["failure_reason"] or "").replace("|", "\\|")
        lines.append(
            f"| {row['candidate']} | {Path(row['graph']).name} | "
            f"{Path(row['grammar']).name} | {row['status']} | {row['result_check']} | "
            f"{row['matched_chunks']}/{row['expected_chunks']} | "
            f"{row['matched_rounds']}/{row['expected_rounds']} | "
            f"{row['baseline_median_ms']} | {row['candidate_median_ms']} | "
            f"{row['time_ratio']} | {row['baseline_peak_process_memory_mib']} | "
            f"{row['candidate_peak_process_memory_mib']} | {row['timing_complete']} | "
            f"{reason} |")
    lines.append("")

    lines.append("## current RSM / CFL_multsrc_adv paired ratio")
    lines.append("")
    lines.append("| graph | grammar | status (current / CFL) | current_over_cfl |")
    lines.append("| --- | --- | --- | --- |")
    for (graph, cnf), pair_rows in _rows_by_pair(rows).items():
        current_row = pair_rows.get("current")
        cfl_row = pair_rows.get(CFL_DISPLAY)
        ratio = (current_row or {}).get("current_over_cfl_ratio", "")
        statuses = f"{(current_row or {}).get('status', 'missing')} / " \
                   f"{(cfl_row or {}).get('status', 'missing')}"
        lines.append(f"| {Path(graph).name} | {Path(cnf).name} | {statuses} | {ratio} |")
    lines.append("")

    lines.append("## Failures and missing data")
    lines.append("")
    bad_rows = [row for row in rows if row["status"] != "MATCH"]
    if not bad_rows:
        lines.append(f"None: all {len(rows)} rows are MATCH.")
    else:
        lines.append("| candidate | graph | grammar | status | reason | log |")
        lines.append("| --- | --- | --- | --- | --- | --- |")
        for row in bad_rows:
            reason = (row["failure_reason"] or "").replace("|", "\\|")
            log_link = relative_link(Path(row["log"]), root)
            lines.append(
                f"| {row['candidate']} | {Path(row['graph']).name} | "
                f"{Path(row['grammar']).name} | {row['status']} | {reason} | "
                f"[log]({log_link}) |")
    lines.append("")
    path.write_text("\n".join(lines) + "\n")


def _rows_by_pair(rows: list[dict]) -> dict[tuple[str, str], dict[str, dict]]:
    by_pair: dict[tuple[str, str], dict[str, dict]] = {}
    for row in rows:
        by_pair.setdefault((row["graph"], row["grammar"]), {})[row["candidate"]] = row
    return by_pair


def main() -> int:
    args = parse_args()
    root = args.output_root.resolve()
    if not root.is_dir():
        print(f"error: output root is not a directory: {root}", file=sys.stderr)
        return 2
    if not CONFIG.is_file():
        print(f"error: missing config: {CONFIG}", file=sys.stderr)
        return 2

    pairs = read_config_pairs(CONFIG)
    rows = build_rows(root, pairs)

    csv_path = root / "morning_summary.csv"
    with open(csv_path, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=SUMMARY_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    md_path = root / "morning_summary.md"
    write_markdown(md_path, root, pairs, rows)

    counts = {status: sum(1 for row in rows if row["status"] == status)
              for status in ("MATCH", "FAIL", "NOT_RUN")}
    print(f"morning_summary.csv: {csv_path} ({len(rows)} rows)")
    print(f"morning_summary.md:  {md_path}")
    print(f"MATCH={counts['MATCH']} FAIL={counts['FAIL']} NOT_RUN={counts['NOT_RUN']}")
    return 0 if counts["MATCH"] == len(rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
