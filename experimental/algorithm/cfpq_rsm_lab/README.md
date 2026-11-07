# CFPQ RSM experiment lab

Start here. **Production is the pre-DeepSeek baseline; candidates live only in the registry.**
Read [CURRENT_EXPERIMENT.md](CURRENT_EXPERIMENT.md) for the current campaign, [variants/REGISTRY.md](variants/REGISTRY.md) for versions, and [BACKUP.md](BACKUP.md) for the recoverable pre-rebuild state.

The current campaign selects `baseline,p1_review,p2_review,p3_review,p4_review`.
These are also the default `make compare` variants. The original p1–p4 and
stack remain available by explicit name; the stack has not been recomposed.
See [PATCH_REVIEW.md](PATCH_REVIEW.md) for the polish and its validation.

```text
cfpq_rsm_lab/
├── Makefile, lab.py       one human entry point; make help / python3 lab.py --help
├── run.py                existing isolated-library timing/profile runner and acceptance checks
├── labstate.py           shared registry, snapshot, diff and pinned-input verification
├── experiment_status.py  derive status from hashed start/completion receipts
├── CURRENT_CAMPAIGN.json explicit campaign, source hashes, required checks and scope
├── VALIDATION.json       generated machine evidence; never edit pass/fail claims by hand
├── CURRENT_EXPERIMENT.md active variants, recipes, validation evidence and unmeasured work
├── variants/             machine-readable registry, source origins and profiling support header
│   ├── active/           ten frozen baseline/original/reviewed/stack kernel sources
│   └── archive/          all distinct historical kernel sources; never implicit candidates
├── patches/              guidance for repository-relative, runner-compatible kernel diffs
│   ├── active/           each active candidate versus the same agreed baseline
│   └── archive/          historical diffs versus the declared comparison parent
├── inputs/               one pinned JSON manifest: configs, inputs, runtime and recipes
├── configs/              existing workload config selections; lab_batik.csv is the pinned eight-case set
├── tooling/              resolved-library record and the pinned cfg_bench executable
├── tests/                existing oracle, C tests, allocation/sanitizer targets and Python tooling tests
├── figures/              existing plotter, extended to read arbitrary accepted runner summaries
├── runs/<date>_<label>/   all NEW output: logs, generated code, binaries, raw CSVs and figures
├── archive/              immutable historical evidence bundles and the superseded tooling guide
└── build/                LEGACY results/DeepSeek scratch; retained as evidence, never searched for candidates
```

The old root `run_*overnight.sh` wrappers are disabled historical recipes. Their bodies and the old summarizers remain for provenance/tests; use the Makefile for active work. Old embedded source copies in historical bundles are evidence, not registry entry points. No old `variants/baseline.c` exists.

## Quick start

These commands work from the lab root on this host:

```bash
cd /home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab
make help
make status
make status-check
make list
make verify
make tooling-tests
make check VARIANT=p2_review
make correctness VARIANT=p2_review
make compare RECIPE=smoke VARIANTS=p1_review,p2_review,p3_review,p4_review
make profile VARIANT=p4_review
```

`check` runs the existing deterministic + 200 generated oracle cases and 10 existing C tests on the exact frozen source. `VARIANT=all` selects all ten active versions. `correctness` additionally invokes the existing runner on the tiny config and loads that exact candidate `.so` for the 15 reduced cases (60-second sweep limit). CNF-derived reference counts may be cached; this is weaker evidence than the small exact oracle. Failed/time-limited checks remain failures/incomplete.

`compare` defaults to a one-source, one-chunk, one-round smoke. **Smoke ratios are not performance results.** Inspect a full comparison without running workloads:

```bash
make compare RECIPE=full SIZES=100 DRY_RUN=1
```

Removing `DRY_RUN=1` deliberately starts the expensive eight-workload comparison (30 chunks, five rounds, one warm-up, seed 2026, inherited OpenMP threading). Omit `SIZES` to run 1, 10 and 100 separately. No full benchmark was run during this rebuild.

`profile` defaults to the tiny recipe and writes only diagnostic profiling outputs plus its exact uninstrumented reference. The `profile_workloads` recipe is available for a deliberate larger diagnosis; never use instrumented times as speedups. Phase/setup times and new operation timings/counters are supported. Candidate-union and prefix-overlap diagnostic probes are currently not collected by the active generator; availability flags in each record say so. Their zero-valued legacy fields are not measurements. Memory CSVs are snapshots of named GraphBLAS objects, not peak workspace/allocation attribution.

Plot a completed timing run using its actual output path (replace the example argument):

```bash
make plot RUN=/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_17-01-16-827750_validation-smoke
```

The plotter refuses failed/incomplete rows and labels one-round results as smoke. It derives names from the runner summary; it does not assume BB7 is the baseline. Min/max whiskers are across chunk medians, not confidence intervals.

## Trustworthy current status

`make status` generates `CURRENT_EXPERIMENT.md`, `VALIDATION.json`, and the active validation cells in `variants/REGISTRY.csv`/`.md` from saved execution receipts. `make status-check` verifies those files are current and all required checks pass; it never repairs stale files silently. `make list` refreshes evidence before displaying the registry.

Each executed entry-point command writes immutable `*.start.json` and `*.result.json` records under its fresh run directory. They record command/cwd, source and tool identities, the pinned manifest identity, exit code or timeout, and hashes of logs/result artifacts/libraries. A start without a valid completion is reported as incomplete (still running or interrupted). The latest matching attempt determines each check; a newer failure cannot be hidden by an older pass. Missing/modified evidence or changed source/tool/input identities becomes STALE. Failed receipts stay in the history after a successful retry. Pre-receipt logs remain historical evidence; they are not promoted into current PASS statuses.

The current campaign explicitly selects its source hashes in `CURRENT_CAMPAIGN.json`. Performance coverage is tracked independently for each planned source size; one successful size is PARTIAL, never complete coverage. Smoke and profile results cannot establish a measured full-performance result. To start a different campaign, preserve the old definition, use a new campaign ID and exact source identities, and record fresh evidence. Updating validation prose or editing a registry status cell cannot make a check pass.

## Source and input rules

- The logical names `baseline`, `p1`, `p2`, `p3`, `p4`, `stack_p1234` resolve through `variants/REGISTRY.csv`. File names contain a source-hash suffix; snapshots are mode 0444 and hash-checked before use. The registry is authoritative, so a stray flat `baseline.c` cannot shadow the control. Archives require an explicit low-level `--allow-archived`; the human entry point selects only active variants.
- **p1–p4 are standalone against the same baseline.** They are not the old cumulative DeepSeek steps. `stack_p1234` is the byte-identical original cumulative final source. Parent and comparison-parent are separate fields when historical ancestry is unknown; see `variants/origins.json`.
- Each registry diff has `a/experimental/algorithm/LAGraph_CFPQ_RSM.c` and `b/...` labels. `make verify` copies its declared diff parent into a scratch repository layout, applies `patch -p1`, and compares the result with the registered SHA-256. `variants/DIFF_VERIFICATION.json` records the initial proofs. For viewing: `git diff --no-index PARENT_SNAPSHOT CANDIDATE_SNAPSHOT` (exit 1 means differences).
- `inputs/pinned.json` records graph, CNF, derived RSM, source-pool and config hashes, the exact executable path/hash, runtime/header identities, and every recipe setting. Selected inputs/runtime are checked before and after runs and before each runner pass. `make verify` hashes the entire manifest. Hashing is outside the kernel timer.
- Every real comparison requires the pinned binary and `--skip-cfg-build`. The entry point supplies them, consistent checkout library paths, a writable run-local JIT cache and blocked order. Builds/runs are sequential. The binary is copied from DeepSeek's compatible checkout-linked build; its original remains recoverable in the backup. `tooling/ldd.txt` records resolved dependencies. A missing/changed binary/library/input is a hard error, not an automatic rebuild/download.
- Fresh output directories are mandatory. Neither sources nor old results are overwritten by a run. `runs/` and the pinned binary directory are Git-ignored generated artifacts; preserve/copy them with results. The manifest documents the local binary to restore, and BACKUP.md identifies its Git backup.

## Adding a variant

1. Start with an explicitly chosen registered parent. Develop outside the frozen directories. Give each revision a new descriptive logical name; do not reuse `p2` for a second source.
2. Publish a new `variants/active/NAME__SHA12.c` with exclusive creation, SHA-256 in the registry and mode 0444. Never edit an existing snapshot or point an existing name at new bytes. On a fresh checkout Git may restore non-executable files as writable; verify hashes first, then restore mode 0444 before running.
3. Generate a kernel-only diff with repository-relative labels, for example the following command with actual parent/candidate paths. `diff` returns 1 for a nonempty valid diff:

```bash
diff -u --label a/experimental/algorithm/LAGraph_CFPQ_RSM.c --label b/experimental/algorithm/LAGraph_CFPQ_RSM.c variants/active/baseline__c7efb94f21bb.c variants/active/p2__791520ee778c.c
```

4. Add a registry row: name, parent, diff_parent, status, one-line description, source, source SHA-256, diff, correctness, benchmark and profiling status. Keep actual ancestry honest; unknown ancestry is `unknown`, with a separate explicit diff parent. Add origin/derivation information and regression cases to the existing driver.
5. Run `make verify`, `make tooling-tests`, `make check VARIANT=NAME`, `make correctness VARIANT=NAME`, and `make profile VARIANT=NAME`. The registry generator test must exercise the new source. Use existing `tests/Makefile` sanitizer/allocation targets when new memory paths warrant them. Record logs and failed seeds.
6. If the new variant is part of the current study, preserve the prior campaign definition and create a new explicit campaign ID/source-hash selection in `CURRENT_CAMPAIGN.json`; then collect fresh receipts. Do not change the identity of an existing campaign or rely on an unrelated passing run.
7. Performance remains `NOT MEASURED` until an adequate pinned uninstrumented experiment exists. A new dataset, executable or schedule requires an explicitly versioned manifest; do not silently change the existing lock. Keep timing, profiling, process RSS and structural estimates distinct.

See the repository-level [CFPQ_RSM_EXPERIMENT_TOOLING.md](/home/hollow/repo/LAGraph/CFPQ_RSM_EXPERIMENT_TOOLING.md) for the verification record and measurement limits.

## OpenMP policy

All builds, correctness checks, timing and profiling runs inherit OpenMP settings unchanged. The lab never sets or unsets `OMP_NUM_THREADS`, `OMP_THREAD_LIMIT`, or other OpenMP variables, and never forces a GraphBLAS thread count. GraphBLAS/OpenMP choose parallelism from the inherited environment and runtime defaults. The legacy `--threads 0` option means inherit, not serial execution; nonzero values are rejected.

Provenance and execution receipts record inherited `OMP_*`, `GOMP_*`, `KMP_*` variables and CPU affinity. Status marks receipts from a different inherited environment or affinity STALE for the current invocation. These records describe runtime settings, not actual per-operation thread counts. Leave variables unset to use runtime defaults; the tooling does not clear variables set in your shell.

The previous single-thread campaign and exact input lock are preserved under `archive/campaigns/cfpq-reviewed-standalone-validation-2026-11-07/`. The current `inputs/pinned.json` is a new version with identical graph/query/source/binary/runtime hashes and inherited-threading recipes. Old validation is historical evidence, not a pass for this new campaign.
