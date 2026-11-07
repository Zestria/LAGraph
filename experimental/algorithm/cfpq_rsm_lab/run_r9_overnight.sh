#!/usr/bin/env bash
# ARCHIVED BY REGISTRY REBUILD: the original body below is provenance only.
printf '%s\n' 'Archived recipe: use make help / CURRENT_EXPERIMENT.md in cfpq_rsm_lab.' >&2
exit 2
#
# run_r9_overnight.sh -- sequential overnight comparisons for the R9 variants.
#
# Runs 30 foreground run.py comparisons in this order, with the inexpensive
# variants first within each source-count group:
#     source size 1:   ds36_pair ds37_fused ds38_no_product_clear ds39_direct_calls
#                      ds30_assign ds31_joint ds32_empty ds33_format
#                      ds34_lazy_p ds35_lazy_nt
#     source size 10:  the same ten variants
#     source size 100: the same ten variants
#
# Every comparison uses the frozen baseline (variants/baseline.c), cfg_bench's
# one unmeasured warm-up on round 0 (--hot-rounds 1), and five measured rounds
# with unrestricted library/runtime threading (--threads 0).  No profiling run
# is made here.
#
# Output: an output root containing per-comparison runner artifacts, each
# comparison's console log, the shell-escaped exact commands, status.tsv, and
# the morning summary (morning_summary.csv / morning_summary.md).
#
# Usage:
#     ./run_r9_overnight.sh [--dry-run] [OUTPUT_ROOT]
#
#   OUTPUT_ROOT   output directory (default: a unique timestamped directory
#                 under <lab>/build/runs/).  A non-empty existing directory is
#                 rejected; previous results are never deleted.
#   --dry-run     print the 30 commands and exit; no build, no benchmark, no
#                 summary is run.
#   -h, --help    show this help.
#
# A failed comparison is recorded as FAILED and the remaining comparisons still
# run; the morning summary is generated afterwards regardless.  The script
# exits nonzero if any comparison or the summary generation failed.

set -uo pipefail
# Deliberately no `set -e`: one failed comparison must not stop the rest.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
LAB=$SCRIPT_DIR
BASELINE=$LAB/variants/baseline.c
CONFIG=$LAB/configs/lab.csv
RUNNER=$LAB/run.py
SUMMARIZER=$LAB/summarize_r9_overnight.py

SOURCES=(1 10 100)
VARIANTS=(ds36_pair ds37_fused ds38_no_product_clear ds39_direct_calls ds30_assign ds31_joint ds32_empty ds33_format ds34_lazy_p ds35_lazy_nt)
total=$(( ${#SOURCES[@]} * ${#VARIANTS[@]} ))
CHUNKS=30
ROUNDS=5
HOT_ROUNDS=1
SEED=2026
THREADS=0

usage() {
    cat <<EOF
usage: $(basename -- "$0") [--dry-run] [OUTPUT_ROOT]

Run the 30 R9 CFPQ RSM comparisons (ds36_pair, ds37_fused,
ds38_no_product_clear, ds39_direct_calls, ds30_assign, ds31_joint,
ds32_empty, ds33_format, ds34_lazy_p, ds35_lazy_nt) sequentially, then write
the morning summary.

  OUTPUT_ROOT   output directory (default: $LAB/build/runs/r9-overnight-<timestamp>)
                A non-empty existing directory is rejected; nothing is deleted.
  --dry-run     print the 30 commands and exit without building or running.
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
    OUTPUT_ROOT=$LAB/build/runs/r9-overnight-$stamp
    suffix=1
    while [[ -e $OUTPUT_ROOT ]]; do
        OUTPUT_ROOT=$LAB/build/runs/r9-overnight-$stamp-$suffix
        suffix=$((suffix + 1))
    done
else
    OUTPUT_ROOT=$(realpath -m -- "$OUTPUT_ROOT")
fi

if ((DRY_RUN)); then
    echo "# dry run: $total comparisons; sources ${SOURCES[*]}; variants ${VARIANTS[*]}"
    echo "# output root would be: $OUTPUT_ROOT"
fi

if ((!DRY_RUN)); then
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

    STATUS_FILE=$OUTPUT_ROOT/status.tsv
    COMMANDS_FILE=$OUTPUT_ROOT/commands.sh
    printf 'variant\tsources\tstart\tfinish\texit_code\tstatus\toutput_dir\tlog\n' >"$STATUS_FILE"
    {
        echo '#!/usr/bin/env bash'
        echo '# exact shell-escaped commands from run_r9_overnight.sh (one per comparison)'
    } >"$COMMANDS_FILE"
fi

failures=0
index=0

for sources in "${SOURCES[@]}"; do
    for variant in "${VARIANTS[@]}"; do
        index=$((index + 1))
        out=$OUTPUT_ROOT/$variant/s$sources
        log=$OUTPUT_ROOT/$variant/s$sources.console.log
        cmd_file=$OUTPUT_ROOT/$variant/s$sources.command

        cmd=(
            python3 "$RUNNER"
            --baseline "$BASELINE"
            --config "$CONFIG"
            --variants "baseline,baseline,$variant"
            --source-size "$sources"
            --chunks "$CHUNKS"
            --rounds "$ROUNDS"
            --hot-rounds "$HOT_ROUNDS"
            --seed "$SEED"
            --threads "$THREADS"
            --out "$out"
        )

        if ((DRY_RUN)); then
            printf '%q ' "${cmd[@]}"
            printf '\n'
            continue
        fi

        mkdir -p -- "$OUTPUT_ROOT/$variant" || exit 1

        # save the shell-escaped exact command before running it
        { printf '%q ' "${cmd[@]}"; printf '\n'; } >"$cmd_file"
        { printf '%q ' "${cmd[@]}"; printf '\n'; } >>"$COMMANDS_FILE"

        echo "=== [$index/$total] variant=$variant sources=$sources"
        start=$(date -Iseconds)
        if "${cmd[@]}" 2>&1 | tee "$log"; then
            rc=0
        else
            rc=${PIPESTATUS[0]}
        fi
        finish=$(date -Iseconds)

        if ((rc == 0)); then
            status=OK
        else
            status=FAILED
            failures=$((failures + 1))
        fi
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$variant" "$sources" "$start" "$finish" "$rc" "$status" "$out" "$log" \
            >>"$STATUS_FILE"
        echo "--- [$index/$total] $status (exit $rc)"
    done
done

if ((DRY_RUN)); then
    exit 0
fi

# Always attempt the morning summary, even if comparisons failed or aborted.
summary_rc=0
echo "=== morning summary ==="
if python3 "$SUMMARIZER" "$OUTPUT_ROOT" 2>&1 | tee "$OUTPUT_ROOT/summarize.console.log"; then
    summary_rc=0
else
    summary_rc=${PIPESTATUS[0]}
fi
if ((summary_rc != 0)); then
    echo "morning summary generation failed (exit $summary_rc)" >&2
else
    echo "morning summary: $OUTPUT_ROOT/morning_summary.csv, $OUTPUT_ROOT/morning_summary.md"
fi

if ((failures > 0 || summary_rc != 0)); then
    echo "run_r9_overnight: $failures comparison(s) failed, summary exit $summary_rc" >&2
    exit 1
fi

echo "run_r9_overnight: all $total comparisons completed and the morning summary was generated"
exit 0
