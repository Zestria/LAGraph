# CFPQ RSM experiment environment

Timing includes static construction of optimized traversals. Production is the
agreed baseline; use lab variants `baseline p1_review` with the default
`inputs/pinned.json` and its original one-shot binary. The prepared API and
CFG_bench adapter change were reverted on 2026-11-17. See
[PREPARED_P1.md](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/PREPARED_P1.md)
for the preserved lab-only implementation, patches and historical evidence.
The historical prepared binary excludes construction and is unsuitable for
end-to-end algorithm comparisons.

For new straightforward timing runs use
[run_simple.py](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/run_simple.py).
It builds selected registered kernels and passes `--hot -r 5` directly to
`cfg_bench`: each chunk gets one warm-up, then five consecutive measurements.
Its defaults are Java points-to, baseline/P1 reviewed, sources 1/10/100 and
30 chunks. `--dry-run` prints the build and benchmark commands without running
them. There is no profiling or outer repetition loop. See the beginning of
[README.md](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/README.md)
for the flags, simple correctness check and summary columns. The legacy tools
described below repeat whole passes and use a different output/status format.

The current reviewed candidates are `p1_review`–`p4_review`, with immutable
baseline-relative diffs and original p1–p4 ancestry in the registry. See
[PATCH_REVIEW.md](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/PATCH_REVIEW.md).
The original six snapshots below remain preserved; `make compare` now defaults
to baseline plus the four reviewed standalone revisions. No performance result
is implied by correctness or tiny-profile validation.

Updated 2026-11-07 after the registry rebuild. The live entry point is [the lab README](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/README.md). The previous 558-line guide is preserved as [historical documentation](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/archive/tooling_guide_before_registry_20261107.md); its old flat variant paths and overnight commands are superseded.

## Current source identities

Production `experimental/algorithm/LAGraph_CFPQ_RSM.c` is restored byte-for-byte from commit `ba6e4308`, SHA-256 `c7efb94f21bbbddc220ab8371d01cd4c852e3ab110d5942367932676f8916928`. It is the agreed pre-DeepSeek optimized source, not BB7 or the older DS reference.

[REGISTRY.csv](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/variants/REGISTRY.csv) is authoritative. It lists six active snapshots and the archived historical sources, including the old corrected baseline, BB7, R13/current, I/S rounds, DS/D rounds, intermediate DeepSeek cumulative sources and prior diagnostic builds. [REGISTRY.md](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/variants/REGISTRY.md) is the human index; [origins.json](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/variants/origins.json) retains original paths and known historical ancestry.

| Active name | Parent | Actual contents |
| --- | --- | --- |
| baseline | ba6e4308 | Agreed pre-DeepSeek production snapshot. |
| p1 | baseline | Compact grouped terminal transfers. |
| p2 | baseline | Trimming/compaction only; no grouped-terminal path. |
| p3 | baseline | Destination-local waiting/routing only; no trimming/grouping. |
| p4 | baseline | Fused continuations only; original full global waiting coordinates. |
| stack_p1234 | baseline comparison | Byte-identical preserved DeepSeek cumulative final source. |

p2–p4 were ported to the common baseline; they are not aliases for cumulative steps. See [DERIVATION.md](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/variants/DERIVATION.md). p4 and the stack retain DeepSeek's partial fusion design: intermediate configurations are bypassed, but the global state dimensions are not re-compacted and continuation products are not grouped. No runtime/memory gain has been established.

Sources have unique hash-suffixed names and mode 0444. Registry lookup precedes any legacy source discovery; an unregistered flat `variants/baseline.c` cannot shadow the control. Archived sources require explicit selection/permission in the low-level runner, never implicit discovery. Repository-relative kernel diffs under `patches/active/` and `patches/archive/` name their exact comparison parent. Unknown historical ancestry is recorded honestly, separately from `diff_parent`.

Before modification, branch `codex/backup-cfpq-lab-20261107-164733` was created at `508a4c02eba28c04784949a439c51bfb8ffa1959`. It includes current tracked/untracked sources, docs/evidence and the complete ignored DeepSeek experiment, excluding generated `build-exp-release` binaries. The original checkout branch/index were left unchanged. [BACKUP.md](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/BACKUP.md) records recovery details.

## Commands on this host

```bash
cd /home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab
make help
python3 lab.py --help
python3 run.py --help
make status
make status-check
make list
python3 lab.py list --all
make verify
make tooling-tests
make check VARIANT=all
make correctness VARIANT=all
make compare RECIPE=smoke VARIANTS=p1,p2,p3,p4,stack_p1234
make profile VARIANT=stack_p1234
make compare RECIPE=full SIZES=100 DRY_RUN=1
```

The final command is a dry run. A deliberate full experiment removes `DRY_RUN=1`; omitting `SIZES` runs source sizes 1, 10 and 100 separately. That can be expensive and was not executed during this rebuild. New output always lives under `runs/<Moscow-date>_<label>/`; timing/profile/generated code never goes into the frozen variant directories.

`check` compiles the selected snapshot directly through the existing test Makefile and runs the oracle and existing C suites. `correctness` additionally uses the existing runner on `CFG_bench/configs/for_test.csv`, then loads that exact generated `.so` on `CFG_bench/tests/data/configs/all.csv`. Each reduced sweep has a 60-second limit; timeout means incomplete. `VARIANT=all` means all active snapshots. The external CFG_bench instructions still require the tiny config first, checking large file sizes, and the final reduced-data sweep.

Existing sanitizer/allocation targets remain under `tests/Makefile`; `R2=/absolute/snapshot.c` and `BIN_DIR=/absolute/run/tests` select the source/output. The harness itself disables JIT. ASan's deliberate giant-allocation regression may need `allocator_may_return_null=1`, as documented in historical DeepSeek feedback; no new exhaustive sanitizer/allocation sweep was claimed for the standalone ports in this rebuild.

## One input and executable lock

[inputs/pinned.json](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/inputs/pinned.json) contains 99 file identities: graphs, CNFs, every derived `.rsm`, supplied source pools, configs, the compatible `cfg_bench` executable, and resolved runtime/header dependencies. The binary is `tooling/bin/cfg_bench__18b417d1ea39`, copied from DeepSeek's checkout-linked build and hash-checked. [tooling/ldd.txt](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/tooling/ldd.txt) records its library resolution.

| Recipe | Config | Sources | Chunks | Measured rounds | Warm-up | Other fixed settings |
| --- | --- | --- | ---: | ---: | ---: | --- |
| smoke | CFG/configs/for_test.csv | 1 | 1 | 1 | 1 | Seed 2026, inherited OpenMP threading, blocked order. |
| full | LAB/configs/lab_batik.csv | 1, 10, 100 in separate runs | 30 | 5 | 1 | Same seed/threading policy/order. Eight workloads. |
| profile_smoke | CFG/configs/for_test.csv | 1 | 1 | 1 diagnostic call | 1 | Reference/profile are separate libraries. |
| profile_workloads | LAB/configs/lab_batik.csv | 1, 10, 100 separately | 1 | 1 diagnostic call | 1 | Explicit larger diagnosis; not a speedup study. |

`make verify` checks all pinned files. Every actual comparison checks its selected input/config files, runtime and executable before/after the run and before each scheduled child pass. Missing/changed hashes, unpinned settings, a different executable, or absence of `--skip-cfg-build` stop the run. The entry point always supplies the pinned binary and skip flag, checkout library paths, a writable run-local JIT cache, and the inherited OpenMP environment unchanged. All variants get the same config, pools, seed and chunk selection. The manifest is copied into each runner output as `pinned-inputs.json`; provenance and `inputs.sha256` are retained as before. Input hashing happens outside the measured kernel call.

Source pools wrap cyclically if necessary and are reachability-biased, not uniform independent vertex samples. Relative config fields resolve from CFG_bench's root. Do not replace installed data, regenerate source pools, download new inputs, auto-rebuild the executable, change an existing lock, or mix recipes silently. A new experiment design requires an explicitly versioned lock.

The pinned binary and run outputs are generated/Git-ignored. Preserve them with the experiment. The binary can also be recovered from the backup Git object's original path `experimental/algorithm/cfpq_rsm_lab/build/deepseek-rXWCtw/tooling/cfg-checkout/cfg_bench`. Its restored bytes must match the lock before execution.

## Profiling and interpretation

`tests/make_profile_variant.py` supports the agreed baseline and every active variant. Active instrumentation uses balanced C function boundaries and whitespace-tolerant phase/statement patterns. Missing/ambiguous anchors report their names and counts. Legacy historical layout support remains explicit; unsupported archived layouts fail with a named anchor instead of `StopIteration`. The registry test invokes the generator for every entry and requires support for every active entry.

The existing `cfpq_profile.h` record/CSV formats are retained and extended with grouped transfer/setup, compaction, local routing/setup and fusion/setup nested times/counters, compact state count, group axes and waiting/fused block counts. Matrix snapshots enumerate grouped incidence/scratch matrices, local routing/frontier/waiting/product matrices, global scatter and fusion scratch. Phase completion waits include accumulator, calls/waiting matrices and summaries as appropriate. Nested times overlap containing setup/phase times and must not be added again.

The active generator currently collects phase/operation diagnostics, not candidate-union or prefix-overlap probes. Availability fields in the record explicitly mark those legacy counters uncollected. Do not interpret their zeroes as measured absence. `callconstr_s` in active profiles measures the bulk call build, not every tuple-preparation operation. Matrix allocation snapshots do not measure all C-array/allocator overhead or peak live temporary memory. Profile mode always performs a warm-up and one diagnostic call per chunk; changing `--rounds` is not a way to obtain five profile rounds.

Timing builds compile the original frozen source without profiler hooks; profiling uses a separate generated translation unit and separate reference/profile libraries. Timing mode rejects profiling compiler flags. Explicit waits and diagnostic I/O change scheduling, so profile times are attribution only. The runner checks tags, expected reference/profile count/hash equality, warm-up/measured identities and `sanity_ok=1`.

Raw `time_s` covers completed kernel execution, including kernel-local setup/cleanup and the adapter's final materialization wait. Parsing, adapter preparation/hash/cleanup are outside that timer. Process `VmHWM` is whole-process peak RSS after reset, not isolated algorithm workspace. Hash/count equality is a practical guard, not a proof; use the small exact oracle. Reduced CNF-derived count references can be cached by path/mtime and do not replace exact set checks.

## Results and plots

A fresh run retains raw CSVs, `summary.csv`, `execution_status.tsv`, logs, measured libraries/build stamps, `provenance.txt`, `inputs.sha256`, and the pinned manifest. Existing `run.py` acceptance rejects missing/duplicate cases/rounds, unstable hashes or reference mismatch. Failures are retained. `--force` is destructive replacement rather than resume; the human entry point always chooses a fresh directory.

```bash
cd /home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab
make plot RUN=/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_17-01-16-827750_validation-smoke
```

The existing plotter now accepts arbitrary accepted runner summaries and derives variant names from them. The verified example is deliberately labeled smoke, not performance evidence. Chunk ratios use candidate round median / control round median; speedup is the reciprocal. Whiskers are min/max chunk medians, not confidence intervals. The historical BB7 summarizer/plot mode still has its exact old three-series layout assumptions; do not use it to aggregate these active variants.

## Evidence-derived status

The explicit current campaign is [CURRENT_CAMPAIGN.json](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/CURRENT_CAMPAIGN.json). `make status` reads hashed start/completion receipts in `runs/` and regenerates CURRENT_EXPERIMENT.md, VALIDATION.json, and active registry validation cells. `make status-check` exits nonzero for missing/inconsistent reports or unmet required checks. Entry-point commands refresh status automatically; `make list` also refreshes before printing.

Receipts identify exact sources/tools, command/cwd, input manifest, exit code/timeouts, logs, result CSVs, build stamps and loaded libraries. Result completeness/count/hash checks reuse the existing runner acceptance. Reduced checks require all named config cases and the recorded candidate library. The latest matching attempt wins, including failure or timeout; evidence drift is STALE, missing evidence is NOT_RUN, and a start without a valid completion is incomplete rather than assumed successful. Earlier failures stay visible after retries. Old pre-receipt evidence is not imported as a fresh pass.

Full performance is separate from smoke/profiling and tracked per source size. Completion at size 100 does not mark sizes 1/10 measured. No long benchmark is required to refresh status. Current records and their exact evidence paths are machine-generated; do not manually change validation claims in the registry.

## Verification record

See [CURRENT_EXPERIMENT.md](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/CURRENT_EXPERIMENT.md) and [VALIDATION.json](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/VALIDATION.json) for exact source hashes, current test counts, exit receipts and artifact hashes. Validation includes all original 34 tooling tests, additional registry/lock tests, patch application/hash proofs for every entry, direct and generated-source oracle checks, existing C tests, loaded-candidate tiny comparisons, all 15 reduced cases for every active variant, and tiny profiles with matching results and sane records. Help, dry-run full recipes, Python/Bash syntax, paths and the plot command were checked. The full eight-workload performance experiment was not run; all active speedups/memory benefits remain unmeasured.

## Inherited OpenMP threading

The current recipe version never sets or unsets OpenMP variables and never forces a GraphBLAS thread count. OpenMP/GraphBLAS control parallelism from runtime defaults and inherited settings. `--threads 0` is retained only as a compatibility spelling of inheritance; every nonzero value is rejected. Both entry point and runner preserve all inherited OpenMP settings in checks, timing and profiling. Provenance and receipts record `OMP_*`, `GOMP_*`, `KMP_*` variables and CPU affinity; a different environment/affinity makes prior receipts STALE for the current invocation. Actual active threads per operation are not measured.

The old single-thread campaign, reports, tools and exact manifest are preserved under `experimental/algorithm/cfpq_rsm_lab/archive/campaigns/cfpq-reviewed-standalone-validation-2026-11-07/`. The new current lock retains every input/runtime/binary file hash and changes the threading policy. Prior validation is not silently relabeled as multithreaded evidence.
