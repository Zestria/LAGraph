#!/usr/bin/env python3
"""Summarize the single combined 100-source selected-variant run.

Usage:
    python3 summarize_s100_selected.py OUTPUT_ROOT

Reads OUTPUT_ROOT/execution_status.tsv, OUTPUT_ROOT/summary.csv and
OUTPUT_ROOT/raw/<safe_display>/roundN.csv.  The opening baseline, the six
selected candidates and the closing baseline all share this one output
directory; there are no per-comparison subdirectories.

Writes OUTPUT_ROOT/morning_summary.csv and OUTPUT_ROOT/morning_summary.md with
one row per candidate / graph / grammar at source size 100 (42 rows).  The two
baseline series bracket the candidates: each candidate ratio is reported both
against the opening baseline and separately against the closing baseline, and
the per-workload closing/opening baseline ratio is shown as drift context.

MATCH requires all 30 chunks x 5 measured rounds for the workload, matching
counts and hashes on every round, a matching opening/closing baseline control,
an "ok" summary.csv row for every chunk, an "ok" execution_status.tsv row for
every scheduled pass, and complete positive timings.  A missing or failed pass,
partial data or a mismatching result is never a MATCH.  Each candidate is
evaluated independently, so a failed candidate does not hide a healthy one.

No winner is selected or combined here.  Process memory is the cfg_bench
process peak, not isolated kernel memory.
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

BASELINE_DISPLAY = "baseline"
BASELINE_COPY_DISPLAY = "baseline#2"
CANDIDATES = (
    "ds36_pair",
    "ds38_no_product_clear",
    "ds39_direct_calls",
    "ds32_empty",
    "d1_native",
    "config_block",
)
SOURCE_SIZE = 100
CHUNKS = 30
ROUNDS = 5

SUMMARY_FIELDS = [
    "candidate",
    "graph",
    "grammar",
    "source_size",
    "status",
    "matched_chunks",
    "expected_chunks",
    "matched_rounds",
    "expected_rounds",
    "candidate_execution_ok",
    "opening_baseline_execution_ok",
    "closing_baseline_execution_ok",
    "opening_baseline_median_ms",
    "candidate_median_ms",
    "time_ratio_open",
    "closing_baseline_median_ms",
    "time_ratio_close",
    "baseline_drift_ratio",
    "opening_baseline_peak_process_memory_mib",
    "candidate_peak_process_memory_mib",
    "closing_baseline_peak_process_memory_mib",
    "timing_complete",
    "memory_scope",
    "failure_reason",
    "log",
]

MEMORY_SCOPE = ("cfg_bench process peak (max max_memory_kb over retained measured rows / "
                "1024), not isolated kernel memory")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output_root", type=Path,
                        help="run_s100_selected.sh output root")
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


def load_execution(root: Path) -> tuple[bool, dict[tuple[str, int], dict]]:
    """Read execution_status.tsv; return (exists, {(display, round): row})."""
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
    """raw[display][(graph,cnf,source_size,chunk)][round] -> cfg_bench row."""
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
                      exec_status: dict) -> tuple[bool, list[str], list[int]]:
    """Check every scheduled pass for this display; return (ok, issues, bad_rounds)."""
    if not exec_exists:
        return False, ["execution_status.tsv missing"], list(range(rounds))
    issues: list[str] = []
    bad_rounds: list[int] = []
    for round_id in range(rounds):
        row = exec_status.get((display, round_id))
        if row is None:
            issues.append(f"round {round_id}: no execution_status.tsv row")
            bad_rounds.append(round_id)
            continue
        outcome = (row.get("outcome") or "").strip()
        exit_code = to_int(row.get("exit_code"))
        if outcome != "ok":
            issues.append(f"round {round_id}: outcome {outcome!r} (exit {exit_code})")
            bad_rounds.append(round_id)
        elif exit_code != 0:
            issues.append(f"round {round_id}: exit {exit_code}")
            bad_rounds.append(round_id)
    return not issues, issues, bad_rounds


def baseline_drift_rows(pairs: list[tuple[str, str]],
                        raw: dict[str, dict[tuple, dict[int, dict]]],
                        source_size: int, chunks: int, rounds: int) -> list[dict]:
    """Per-workload closing/opening baseline ratio and timings."""
    rows: list[dict] = []
    opening = raw.get(BASELINE_DISPLAY, {})
    closing = raw.get(BASELINE_COPY_DISPLAY, {})
    for graph, cnf in pairs:
        ratios: list[float] = []
        opening_medians: list[float] = []
        closing_medians: list[float] = []
        for chunk in range(chunks):
            key = (graph, cnf, str(source_size), str(chunk))
            open_rounds = opening.get(key, {})
            close_rounds = closing.get(key, {})
            open_times = [to_float(open_rounds[r].get("time_s")) for r in range(rounds)
                          if r in open_rounds]
            close_times = [to_float(close_rounds[r].get("time_s")) for r in range(rounds)
                           if r in close_rounds]
            open_times = [value for value in open_times if value is not None and value > 0]
            close_times = [value for value in close_times if value is not None and value > 0]
            if len(open_times) != rounds or len(close_times) != rounds:
                continue
            open_median = median(open_times)
            close_median = median(close_times)
            if open_median is None or close_median is None:
                continue
            opening_medians.append(open_median)
            closing_medians.append(close_median)
            if open_median > 0:
                ratios.append(close_median / open_median)
        rows.append({
            "graph": graph,
            "grammar": cnf,
            "source_size": source_size,
            "chunks_compared": len(ratios),
            "opening_median_ms": fmt(median(opening_medians) * 1000.0
                                     if opening_medians else None),
            "closing_median_ms": fmt(median(closing_medians) * 1000.0
                                     if closing_medians else None),
            "closing_over_opening": fmt(median(ratios), 6),
        })
    return rows


def evaluate_workload(candidate: str, graph: str, cnf: str, root: Path,
                      raw: dict, summary_exists: bool, summary: dict,
                      exec_exists: bool, exec_status: dict,
                      source_size: int, chunks: int, rounds: int,
                      candidate_attempted: bool, candidate_exec_ok: bool,
                      candidate_exec_issues: list[str],
                      opening_exec_ok: bool, opening_exec_issues: list[str],
                      closing_exec_ok: bool, closing_exec_issues: list[str],
                      drift_ratio: str) -> dict:
    candidate_raw = raw.get(candidate, {})
    opening_raw = raw.get(BASELINE_DISPLAY, {})
    closing_raw = raw.get(BASELINE_COPY_DISPLAY, {})
    issues: list[str] = []

    matched_chunks = 0
    matched_rounds = 0
    timing_chunks = 0
    any_data = False
    opening_chunk_medians: list[float] = []
    candidate_chunk_medians: list[float] = []
    closing_chunk_medians: list[float] = []
    ratios_open: list[float] = []
    ratios_close: list[float] = []
    opening_memories: list[float] = []
    candidate_memories: list[float] = []
    closing_memories: list[float] = []

    for chunk in range(chunks):
        key = (graph, cnf, str(source_size), str(chunk))
        candidate_rounds = candidate_raw.get(key, {})
        opening_rounds = opening_raw.get(key, {})
        closing_rounds = closing_raw.get(key, {})
        summary_row = summary.get((candidate, graph, cnf, str(source_size), str(chunk)))
        if candidate_rounds or opening_rounds or closing_rounds or summary_row is not None:
            any_data = True

        chunk_issues: list[str] = []
        rounds_ok = 0
        candidate_counts: set[int] = set()
        candidate_hashes: set[int] = set()
        baseline_consistent = True

        for round_id in range(rounds):
            if round_id not in candidate_rounds:
                chunk_issues.append(f"chunk {chunk}: candidate missing round {round_id}")
                continue
            if round_id not in opening_rounds:
                chunk_issues.append(f"chunk {chunk}: opening baseline missing round {round_id}")
                continue
            if round_id not in closing_rounds:
                chunk_issues.append(f"chunk {chunk}: closing baseline missing round {round_id}")
                baseline_consistent = False
                continue

            candidate_count = to_int(candidate_rounds[round_id].get("result"))
            candidate_hash = to_int(candidate_rounds[round_id].get("result_hash"))
            opening_count = to_int(opening_rounds[round_id].get("result"))
            opening_hash = to_int(opening_rounds[round_id].get("result_hash"))
            closing_count = to_int(closing_rounds[round_id].get("result"))
            closing_hash = to_int(closing_rounds[round_id].get("result_hash"))
            if None in (candidate_count, candidate_hash, opening_count, opening_hash,
                        closing_count, closing_hash):
                chunk_issues.append(f"chunk {chunk}: malformed count/hash in round {round_id}")
                continue
            if opening_count != closing_count or opening_hash != closing_hash:
                chunk_issues.append(
                    f"chunk {chunk}: opening/closing baseline disagree in round {round_id}")
                baseline_consistent = False
            candidate_counts.add(candidate_count)
            candidate_hashes.add(candidate_hash)
            if candidate_hash == 0:
                chunk_issues.append(f"chunk {chunk}: candidate zero hash in round {round_id}")
                continue
            if candidate_count != opening_count:
                chunk_issues.append(
                    f"chunk {chunk}: count mismatch in round {round_id} "
                    f"(candidate {candidate_count}, baseline {opening_count})")
                continue
            if candidate_hash != opening_hash:
                chunk_issues.append(
                    f"chunk {chunk}: hash mismatch in round {round_id} "
                    f"(candidate {candidate_hash}, baseline {opening_hash})")
                continue
            rounds_ok += 1

        if len(candidate_counts) > 1:
            chunk_issues.append(f"chunk {chunk}: unstable candidate counts")
        if len(candidate_hashes) > 1:
            chunk_issues.append(f"chunk {chunk}: unstable candidate hashes")

        summary_ok = False
        if not summary_exists:
            chunk_issues.append(f"chunk {chunk}: summary.csv missing")
        elif summary_row is None:
            chunk_issues.append(f"chunk {chunk}: no summary.csv row")
        else:
            rounds_seen = to_int(summary_row.get("rounds_seen"))
            count_ok = to_int(summary_row.get("count_ok"))
            hash_ok = to_int(summary_row.get("hash_ok"))
            summary_ok = (
                summary_row.get("status") == "ok"
                and rounds_seen == rounds
                and count_ok == 1
                and hash_ok == 1
            )
            if not summary_ok:
                chunk_issues.append(
                    f"chunk {chunk}: summary status {summary_row.get('status')!r} "
                    f"rounds {rounds_seen} count_ok={count_ok} hash_ok={hash_ok}")

        opening_times = [to_float(opening_rounds[r].get("time_s"))
                         for r in range(rounds) if r in opening_rounds]
        candidate_times = [to_float(candidate_rounds[r].get("time_s"))
                           for r in range(rounds) if r in candidate_rounds]
        closing_times = [to_float(closing_rounds[r].get("time_s"))
                         for r in range(rounds) if r in closing_rounds]
        opening_times = [value for value in opening_times if value is not None and value > 0]
        candidate_times = [value for value in candidate_times if value is not None and value > 0]
        closing_times = [value for value in closing_times if value is not None and value > 0]
        full_timing = (len(opening_times) == rounds and len(candidate_times) == rounds
                       and len(closing_times) == rounds)

        if len(opening_times) == rounds:
            opening_median = median(opening_times)
            if opening_median is not None:
                opening_chunk_medians.append(opening_median)
        if len(candidate_times) == rounds:
            candidate_median = median(candidate_times)
            if candidate_median is not None:
                candidate_chunk_medians.append(candidate_median)
        if len(closing_times) == rounds:
            closing_median = median(closing_times)
            if closing_median is not None:
                closing_chunk_medians.append(closing_median)

        if full_timing:
            timing_chunks += 1
            opening_median = median(opening_times)
            candidate_median = median(candidate_times)
            closing_median = median(closing_times)
            if (opening_median is not None and candidate_median is not None
                    and closing_median is not None and opening_median > 0):
                # ratios use matching chunks only; a mismatching chunk is not
                # silently averaged into a candidate result
                if rounds_ok == rounds and baseline_consistent and summary_ok:
                    ratios_open.append(candidate_median / opening_median)
                    if closing_median > 0:
                        ratios_close.append(candidate_median / closing_median)
        else:
            chunk_issues.append(f"chunk {chunk}: incomplete or non-positive timing")

        for round_row in opening_rounds.values():
            value = to_float(round_row.get("max_memory_kb"))
            if value is not None:
                opening_memories.append(value)
        for round_row in candidate_rounds.values():
            value = to_float(round_row.get("max_memory_kb"))
            if value is not None:
                candidate_memories.append(value)
        for round_row in closing_rounds.values():
            value = to_float(round_row.get("max_memory_kb"))
            if value is not None:
                closing_memories.append(value)

        chunk_correct = (
            rounds_ok == rounds
            and baseline_consistent
            and summary_ok
            and full_timing
        )
        matched_rounds += rounds_ok
        if chunk_correct:
            matched_chunks += 1
        else:
            issues.extend(chunk_issues)

    timing_complete = timing_chunks == chunks
    opening_ms = median(opening_chunk_medians)
    candidate_ms = median(candidate_chunk_medians)
    closing_ms = median(closing_chunk_medians)
    time_ratio_open = median(ratios_open)
    time_ratio_close = median(ratios_close)
    opening_memory = max(opening_memories) / 1024.0 if opening_memories else None
    candidate_memory = max(candidate_memories) / 1024.0 if candidate_memories else None
    closing_memory = max(closing_memories) / 1024.0 if closing_memories else None

    if not candidate_attempted:
        status = "NOT_RUN"
        reason = "candidate display was not attempted"
    elif (matched_chunks == chunks and timing_complete and summary_exists
          and candidate_exec_ok and opening_exec_ok and closing_exec_ok):
        status = "MATCH"
        reason = ""
    else:
        status = "FAIL"
        reasons: list[str] = []
        if not candidate_attempted:
            reasons.append("candidate display has no execution/CSV data")
        if not candidate_exec_ok:
            reasons.extend(candidate_exec_issues[:2])
        if not opening_exec_ok:
            reasons.extend(f"opening baseline: {item}" for item in opening_exec_issues[:2])
        if not closing_exec_ok:
            reasons.extend(f"closing baseline: {item}" for item in closing_exec_issues[:2])
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
                if recorded:
                    log_path = Path(recorded)
                else:
                    log_path = log_dir / f"round{round_id}.log"
                break

    return {
        "candidate": candidate,
        "graph": graph,
        "grammar": cnf,
        "source_size": source_size,
        "status": status,
        "matched_chunks": matched_chunks,
        "expected_chunks": chunks,
        "matched_rounds": matched_rounds,
        "expected_rounds": chunks * rounds,
        "candidate_execution_ok": int(candidate_exec_ok),
        "opening_baseline_execution_ok": int(opening_exec_ok),
        "closing_baseline_execution_ok": int(closing_exec_ok),
        "opening_baseline_median_ms": fmt(opening_ms * 1000.0 if opening_ms is not None else None),
        "candidate_median_ms": fmt(candidate_ms * 1000.0 if candidate_ms is not None else None),
        "time_ratio_open": fmt(time_ratio_open, 6),
        "closing_baseline_median_ms": fmt(closing_ms * 1000.0 if closing_ms is not None else None),
        "time_ratio_close": fmt(time_ratio_close, 6),
        "baseline_drift_ratio": drift_ratio,
        "opening_baseline_peak_process_memory_mib": fmt(opening_memory, 1),
        "candidate_peak_process_memory_mib": fmt(candidate_memory, 1),
        "closing_baseline_peak_process_memory_mib": fmt(closing_memory, 1),
        "timing_complete": "yes" if timing_complete else "no",
        "memory_scope": MEMORY_SCOPE,
        "failure_reason": reason,
        "log": str(log_path),
    }


def build_rows(root: Path, pairs: list[tuple[str, str]],
               candidates: tuple[str, ...] = CANDIDATES,
               source_size: int = SOURCE_SIZE, chunks: int = CHUNKS,
               rounds: int = ROUNDS) -> tuple[list[dict], list[dict]]:
    """Build candidate rows and the opening/closing baseline drift rows."""
    displays = (BASELINE_DISPLAY,) + tuple(candidates) + (BASELINE_COPY_DISPLAY,)
    exec_exists, exec_status = load_execution(root)
    summary_exists, summary = load_summary(root)
    raw = load_raw(root, displays, source_size, rounds)

    exec_state: dict[str, tuple[bool, list[str], list[int]]] = {}
    for display in displays:
        exec_state[display] = display_execution(display, rounds, exec_exists, exec_status)

    attempted = {
        candidate: display_attempted(candidate, raw, summary, exec_status)
        for candidate in candidates
    }
    drift_rows = baseline_drift_rows(pairs, raw, source_size, chunks, rounds)
    drift_by_pair = {(row["graph"], row["grammar"]): row["closing_over_opening"]
                     for row in drift_rows}

    rows: list[dict] = []
    for candidate in candidates:
        candidate_exec_ok, candidate_exec_issues, _ = exec_state[candidate]
        opening_exec_ok, opening_exec_issues, _ = exec_state[BASELINE_DISPLAY]
        closing_exec_ok, closing_exec_issues, _ = exec_state[BASELINE_COPY_DISPLAY]
        for graph, cnf in pairs:
            rows.append(evaluate_workload(
                candidate, graph, cnf, root, raw, summary_exists, summary,
                exec_exists, exec_status, source_size, chunks, rounds,
                attempted[candidate], candidate_exec_ok, candidate_exec_issues,
                opening_exec_ok, opening_exec_issues, closing_exec_ok,
                closing_exec_issues, drift_by_pair.get((graph, cnf), "")))
    return rows, drift_rows


def write_markdown(path: Path, root: Path, pairs: list[tuple[str, str]],
                   rows: list[dict], drift_rows: list[dict]) -> None:
    lines: list[str] = []
    lines.append("# CFPQ RSM selected 100-source summary")
    lines.append("")
    lines.append(f"- Run root: `{root}`")
    lines.append(f"- Generated: {datetime.datetime.now().astimezone().isoformat()}")
    lines.append(f"- Config: `{CONFIG}` ({len(pairs)} graph/grammar pairs)")
    lines.append(f"- Candidates: {', '.join(CANDIDATES)}")
    lines.append(f"- Source size: {SOURCE_SIZE}; {CHUNKS} chunks; {ROUNDS} measured rounds")
    lines.append(f"- Expected rows: {len(CANDIDATES)} candidates x {len(pairs)} workloads "
                 f"= {len(rows)}")
    lines.append("- The opening and closing baseline series are shared by all six "
                 "candidates and bracket them; there is no per-candidate baseline.")
    lines.append("- `MATCH` requires all 30 chunks x 5 measured rounds, matching counts "
                 "and hashes by round, a consistent opening/closing baseline, an `ok` "
                 "summary.csv row for every chunk, `ok` execution statuses for every "
                 "scheduled pass, and complete positive timings.")
    lines.append("- `ratio_open` is the median across matching chunks of candidate "
                 "round-median / opening-baseline round-median; `ratio_close` uses the "
                 "closing baseline. Smaller is better. `drift` is closing/opening for "
                 "the same workload.")
    lines.append(f"- Memory is **process memory**: {MEMORY_SCOPE}.")
    lines.append("- No winner is selected or combined; a failed candidate does not "
                 "change a healthy candidate's row.")
    lines.append("")

    lines.append("## Status overview")
    lines.append("")
    lines.append("| candidate | MATCH | FAIL | NOT_RUN | median ratio_open (MATCH) | "
                 "median ratio_close (MATCH) |")
    lines.append("| --- | --- | --- | --- | --- | --- |")
    for candidate in CANDIDATES:
        subset = [row for row in rows if row["candidate"] == candidate]
        matches = sum(1 for row in subset if row["status"] == "MATCH")
        fails = sum(1 for row in subset if row["status"] == "FAIL")
        not_run = sum(1 for row in subset if row["status"] == "NOT_RUN")
        open_ratios = [float(row["time_ratio_open"]) for row in subset
                       if row["status"] == "MATCH" and row["time_ratio_open"]]
        close_ratios = [float(row["time_ratio_close"]) for row in subset
                        if row["status"] == "MATCH" and row["time_ratio_close"]]
        lines.append(
            f"| {candidate} | {matches} | {fails} | {not_run} | "
            f"{fmt(median(open_ratios), 3)} | {fmt(median(close_ratios), 3)} |")
    lines.append("")

    lines.append("## Per-workload results")
    lines.append("")
    lines.append("| candidate | graph | grammar | status | chunks | rounds | "
                 "open ms | candidate ms | ratio_open | close ms | ratio_close | drift | "
                 "timing | candidate mem MiB | reason |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | "
                 "--- | --- | --- | --- |")
    for row in rows:
        reason = (row["failure_reason"] or "").replace("|", "\\|")
        lines.append(
            f"| {row['candidate']} | {Path(row['graph']).name} | "
            f"{Path(row['grammar']).name} | {row['status']} | "
            f"{row['matched_chunks']}/{row['expected_chunks']} | "
            f"{row['matched_rounds']}/{row['expected_rounds']} | "
            f"{row['opening_baseline_median_ms']} | {row['candidate_median_ms']} | "
            f"{row['time_ratio_open']} | {row['closing_baseline_median_ms']} | "
            f"{row['time_ratio_close']} | {row['baseline_drift_ratio']} | "
            f"{row['timing_complete']} | {row['candidate_peak_process_memory_mib']} | "
            f"{reason} |")
    lines.append("")

    lines.append("## Opening/closing baseline drift")
    lines.append("")
    lines.append("Per-workload closing/opening ratio, using chunks with complete positive "
                 "timings in both baseline series. This is drift context, not a candidate "
                 "result.")
    lines.append("")
    lines.append("| graph | grammar | chunks compared | opening median ms | "
                 "closing median ms | closing/opening |")
    lines.append("| --- | --- | --- | --- | --- | --- |")
    for row in drift_rows:
        lines.append(
            f"| {Path(row['graph']).name} | {Path(row['grammar']).name} | "
            f"{row['chunks_compared']} | {row['opening_median_ms']} | "
            f"{row['closing_median_ms']} | {row['closing_over_opening']} |")
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
    rows, drift_rows = build_rows(root, pairs)

    csv_path = root / "morning_summary.csv"
    with open(csv_path, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=SUMMARY_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    md_path = root / "morning_summary.md"
    write_markdown(md_path, root, pairs, rows, drift_rows)

    counts = {status: sum(1 for row in rows if row["status"] == status)
              for status in ("MATCH", "FAIL", "NOT_RUN")}
    print(f"morning_summary.csv: {csv_path} ({len(rows)} rows)")
    print(f"morning_summary.md:  {md_path}")
    print(f"MATCH={counts['MATCH']} FAIL={counts['FAIL']} NOT_RUN={counts['NOT_RUN']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
