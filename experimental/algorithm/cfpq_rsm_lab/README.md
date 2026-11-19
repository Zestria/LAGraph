# CFPQ RSM experiment lab

`baseline`, `p1_full`, and `p1_full_clean` are the active snapshots.
Prior versions, including `p1_review` and `p1_blocks`, are registered in the archive.
`p1_full` uses full coordinates and retains endpoint sharing, support filtering,
batched frontier gathering, and result routing. See [P1_FULL.md](P1_FULL.md)
for the formulas, the completed Avrora/Batik comparison, and validation.

## Java comparison of the active versions

```bash
cd /home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab
python3 run_simple.py --config configs/java_points_to.csv \
  --variants baseline p1_full --reference baseline \
  --source-sizes 1,10,100 --chunks 30 --rounds 5 --hot --dry-run
```

Remove `--dry-run` to execute. This selects Avrora and Batik and the two named
variants. `--build-only` compiles without benchmarking. Each variant gets one
`cfg_bench` process and each chunk gets one
warm-up and five consecutive measurements. Construction and cleanup are inside
the one-shot algorithm timer. There is no profiling or automatic binary rebuild.

The runner preserves all inherited OpenMP settings and records CPU affinity.
It saves a fresh run directory containing copied sources/config/script,
`inputs.json`, `settings.json`, commands/build/run logs, libraries, raw CSVs,
and `summary.csv`. Existing output directories are never overwritten.
`--reference` selects the count/hash and timing-ratio reference; it defaults to
baseline when selected, otherwise the first selected variant. Saved settings
and summaries identify that reference. Old runs remain readable.

Every expected case must have the requested repetitions and stable nonzero
count/hash evidence matching the reference. This is sample agreement, not an
exact oracle proof. Ratios below 1 mean faster than the selected reference.
The harness's `malloc_trim` does not eliminate allocator effects or run-order
drift. To repeat the completed comparison against `p1_review`, use
`--allow-archived --variants p1_review p1_full --reference p1_review`.
To time the refactored candidate, use
`--variants p1_full p1_full_clean --reference p1_full`. The legacy profile
generator requires source anchors that the refactored candidate lacks.

The original literal-block comparison is described in [P1_BLOCKS.md](P1_BLOCKS.md)
and its [saved analysis](runs/2026-11-17_p1_review_vs_blocks_analysis/analysis.md).
The prepared API prototype remains historical and is excluded from current
end-to-end measurements; see [PREPARED_P1.md](PREPARED_P1.md).

## Validation and registry

```bash
make verify
make tooling-tests
make correctness VARIANT=p1_full
make profile VARIANT=p1_full
make status
```

The current campaign identifies `baseline` and `p1_full`.
`CURRENT_EXPERIMENT.md` and `VALIDATION.json` are generated from saved execution
receipts. Do not edit PASS/FAIL claims by hand. Older campaign/catalog/document
snapshots are preserved in
[the cleanup backup](archive/campaigns/2026-11-17_13-33-11_before-full-coordinates/).
Legacy pinned full recipes cover their original workload set; they are separate
from the Java-only simple-run comparison above.

`make check` runs deterministic regressions, 200 generated exact-oracle cases,
and the 10 existing algorithm tests against one frozen translation unit.
`make correctness` additionally loads that exact shared library on the tiny
config and all 15 reduced datasets. Reduced counts may use a CNF reference
cache; the small oracle compares exact sets. Tiny profiling checks instrumentation
and result agreement; it does not establish performance. Allocation/sanitizer
targets remain in `tests/Makefile`; linked libraries are not sanitizer-instrumented.

The human legacy CLI is `lab.py` / `Makefile`. `make compare` defaults to the
two active variants on the tiny smoke recipe. Use `--allow-archived` with
`run_simple.py` or `run.py` to select archived snapshots explicitly. The optional
historical prepared test uses its archived snapshot.

## Source and input rules

- The authoritative catalog is [variants/REGISTRY.csv](variants/REGISTRY.csv).
  Registered frozen sources are named with their SHA-256 prefix, checked before
  use, and mode 0444. Never overwrite a snapshot or repoint a name at new bytes.
  Develop outside the frozen directories and publish a distinct revision.
- `parent` records ancestry; `diff_parent` records the actual comparison source.
  Kernel diffs use repository-relative labels. `make verify` applies every diff
  to its declared parent and compares the exact resulting bytes/hash.
- Original P1-P4 and their reviewed versions retain their ancestry and patches
  in the registry. Their relocation does not change any source bytes. The
  cumulative stack remains an explicitly archived historical source.
- `inputs/pinned.json` locks binary, runtime, graph, CNF, derived RSM, source-pool,
  and config identities. The simple runner derives and saves a selected-input
  manifest. Never silently rebuild a pinned binary, modify a lock, or overwrite
  old results to make a comparison pass.
- Publish source, patch, registry row, and origin metadata together. Add relevant
  regressions, run registry/tooling/oracle/existing/reduced/profile checks, and
  use allocation/sanitizer checks for new ownership paths. Keep failed attempts.
- Preserve the prior campaign before changing study scope. A new campaign must
  identify exact sources and collect fresh receipts. Smoke and profile timings
  do not establish full-workload speedups. Keep timing, process RSS, and named
  GraphBLAS allocation snapshots distinct.

## Threading policy

All real builds, tests, timings, and profiling runs inherit OpenMP settings
unchanged. Never set, unset, or override `OMP_NUM_THREADS`, `OMP_THREAD_LIMIT`,
other OpenMP environment settings, or a GraphBLAS thread count. Record inherited
settings and CPU affinity; do not infer actual per-operation threads from them.
The legacy `--threads 0` means inherit; nonzero overrides are rejected. Synthetic
tooling fixtures can use isolated environments to test this preservation rule.
