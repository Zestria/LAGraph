#!/usr/bin/env bash
# ARCHIVED BY REGISTRY REBUILD: the original body below is provenance only.
printf '%s\n' 'Archived recipe: use make help / CURRENT_EXPERIMENT.md in cfpq_rsm_lab.' >&2
exit 2
#
# run_s100_selected.sh -- one combined 100-source comparison for the six
# selected RSM candidates, with the opening and closing baselines shared by all
# of them.
#
# The single runner invocation schedules 8 display series x 5 measured rounds
# = 40 cfg_bench passes in blocked order:
#     baseline, 36_pair, ds38_no_product_clear, ds39_direct_calls, ds32_empty,
#     d1_native, config_block, baseline#2
# Each display completes all five measured rounds before the next display
# starts.  Round 0 of each display carries the one unmeasured --hot warm-up.
# Settings are unchanged: 100 sources, 30 chunks, 5 measured rounds, seed 2026,
# unrestricted threading (--threads 0).  No profiling and no parallel
# benchmark processes are used.
#
# Output: the combined output root contains the runner's provenance.txt, raw/
# CSVs, execution_status.tsv, summary.csv, the shell-escaped commands, the
# runner and summarizer console logs, the separately retained exit codes and
# the morning summary (morning_summary.csv / morning_summary.md).
#
# Usage:
#     ./run_s100_selected.sh [--dry-run] [OUTPUT_ROOT]
#
#   OUTPUT_ROOT   output directory (default: a unique timestamped directory
#                 under <lab>/build/runs/).  A non-empty existing directory is
#                 rejected; previous results are never deleted.
#   --dry-run     print the exact runner and summarizer commands and exit; no
#                 build, no benchmark and no summary is run.
#   -h, --help    show this help.
#
# The summarizer runs even if the runner exits nonzero; its exit code is kept
# separately in exit_codes.tsv.  The script returns nonzero if either fails.
# This script never backgrounds work.

set -uo pipefail
# Deliberately no `set -e`: the morning summary must still run after a runner
# failure.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
LAB=$SCRIPT_DIR
BASELINE=$LAB/variants/baseline.c
CONFIG=$LAB/configs/lab.csv
RUNNER=$LAB/run.py
SUMMARIZER=$LAB/summarize_s100_selected.py

usage() {
    cat <<EOF
usage: $(basename -- "$0") [--dry-run] [OUTPUT_ROOT]

Run the one combined 100-source comparison: baseline, ds36_pair,
ds38_no_product_clear, ds39_direct_calls, ds32_empty, d1_native,
config_block, baseline#2.  There is no per-candidate runner loop.

  OUTPUT_ROOT   output directory (default: $LAB/build/runs/s100-selected-<timestamp>)
                A non-empty existing directory is rejected; nothing is deleted.
  --dry-run     print the exact runner and summarizer commands and exit without
                building or running workloads.
  -h, --help    show this help.
EOF
}

DRY_RUN=0
OUTPUT_ROOT=""
while (($# > 0)); do
    case $1 in
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        --*)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [[ -n $OUTPUT_ROOT ]]; then
                echo "at most one OUTPUT_ROOT may be given" >&2
                exit 2
            fi
            OUTPUT_ROOT=$1
            shift
            ;;
    esac
done

stamp=$(date +%Y-%m-%d_%H-%M-%S)
if [[ -z $OUTPUT_ROOT ]]; then
    OUTPUT_ROOT=$LAB/build/runs/s100-selected-$stamp
    suffix=1
    while [[ -e $OUTPUT_ROOT ]]; do
        OUTPUT_ROOT=$LAB/build/runs/s100-selected-$stamp-$suffix
        suffix=$((suffix + 1))
    done
else
    OUTPUT_ROOT=$(realpath -m -- "$OUTPUT_ROOT")
fi

runner_cmd=(
    python3 "$RUNNER"
    --baseline "$BASELINE"
    --config "$CONFIG"
    --variants "baseline,ds36_pair,ds38_no_product_clear,ds39_direct_calls,ds32_empty,d1_native,config_block,baseline"
    --source-size 100 --chunks 30 --rounds 5 --hot-rounds 1
    --seed 2026 --threads 0 --order blocked --keep-going
    --out "$OUTPUT_ROOT"
)
summarize_cmd=(python3 "$SUMMARIZER" "$OUTPUT_ROOT")

if ((DRY_RUN)); then
    echo "# dry run: one combined 100-source comparison; 8 display series x 5 measured rounds = 40 cfg_bench passes"
    echo "# output root would be: $OUTPUT_ROOT"
    printf '%q ' "${runner_cmd[@]}"
    printf '\n'
    printf '%q ' "${summarize_cmd[@]}"
    printf '\n'
    exit 0
fi

for path in "$BASELINE" "$CONFIG" "$RUNNER" "$SUMMARIZER"; do
    if [[ ! -f $path ]]; then
        echo "missing required file: $path" >&2
        exit 1
    fi
done
if [[ -e $OUTPUT_ROOT && ! -d $OUTPUT_ROOT ]]; then
    echo "output root exists and is not a directory: $OUTPUT_ROOT" >&2
    exit 1
fi
if [[ -d $OUTPUT_ROOT && -n $(ls -A -- "$OUTPUT_ROOT") ]]; then
    echo "refusing to reuse non-empty output root: $OUTPUT_ROOT" >&2
    echo "choose a new OUTPUT_ROOT or remove the old one yourself" >&2
    exit 1
fi
mkdir -p -- "$OUTPUT_ROOT" || exit 1

commands_file=$OUTPUT_ROOT/commands.sh
runner_log=$OUTPUT_ROOT/run.console.log
summarizer_log=$OUTPUT_ROOT/summarize.console.log
exit_file=$OUTPUT_ROOT/exit_codes.tsv

{
    echo '#!/usr/bin/env bash'
    echo '# exact shell-escaped commands from run_s100_selected.sh'
    printf '%q ' "${runner_cmd[@]}"
    printf '\n'
    printf '%q ' "${summarize_cmd[@]}"
    printf '\n'
} >"$commands_file"
{
    printf '%q ' "${runner_cmd[@]}"
    printf '\n'
} >"$OUTPUT_ROOT/command"

echo "=== run_s100_selected: one combined 100-source comparison"
echo "output root: $OUTPUT_ROOT"
if "${runner_cmd[@]}" 2>&1 | tee "$runner_log"; then
    runner_rc=0
else
    runner_rc=${PIPESTATUS[0]}
fi
printf 'runner\t%s\n' "$runner_rc" >"$exit_file"
echo "--- runner exit $runner_rc"

# Run the summary even if the runner failed; keep its exit code separately.
summarizer_rc=0
echo "=== morning summary ==="
if "${summarize_cmd[@]}" 2>&1 | tee "$summarizer_log"; then
    summarizer_rc=0
else
    summarizer_rc=${PIPESTATUS[0]}
fi
printf 'summarizer\t%s\n' "$summarizer_rc" >>"$exit_file"

if ((runner_rc != 0 || summarizer_rc != 0)); then
    echo "run_s100_selected: runner exit $runner_rc, summarizer exit $summarizer_rc" >&2
    exit 1
fi

echo "run_s100_selected: runner and summarizer completed"
exit 0
