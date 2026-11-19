# Prepared P1 (historical, reverted from production)

Reverted on 2026-11-17: static construction belongs inside the algorithm timer.
Production is restored to the agreed baseline; CFG_bench's RSM adapter is restored
to its exact pre-feature source. The public prepared header and installation
changes are removed. Unrelated changes in both checkouts are preserved.

For current measurements, use `python3 run_simple.py --variants baseline p1_full`
with its default manifest. Both kernels include their own construction. Reviewed P1 is
archived as a [source](variants/archive/p1_review__1c5ad06ef855.c) and
[baseline-relative patch](patches/archive/p1_review.patch).

The rejected split remains reproducible as a lab-only
[archived snapshot](variants/archive/p1_prepared__c26e8e4a08c8.c),
[patch against reviewed P1](patches/archive/p1_prepared.patch),
[support header](include/LAGraph_CFPQ_RSM_prepared.h) and
[unapplied adapter patch](tooling/cfg_bench_prepared.patch).
The [complete LAGraph API patch](tooling/lagraph_prepared_api.patch) also preserves
the former public header and installation changes; its parent is `p1_review`.
The historical prepared binary/manifest remain explicit opt-ins for inspecting
old results; they exclude construction and must not be used for end-to-end
comparisons. The manifest's header path now points inside the lab; its original
version and all pre-revert files are backed up under
[runs/2026-11-17_revert-prepared/](runs/2026-11-17_revert-prepared/).
Old measurements and plots are unchanged and retain their original timing scope.

The implementation and validation below describe that historical prototype.

P1's family discovery, graph-support extraction, compact incidence matrices,
dual lookup hash and residual ordinary transitions are constructed by
`LAGraph_CFPQ_RSM_Prepare`. `LAGraph_CFPQ_RSM_Execute` reuses that support with
fresh per-query products, frontier/visited matrices, summaries, waiting prefixes
and gather buffers. The original `LAGraph_CFPQ_RSM` entry point remains a
prepare/execute/free wrapper, preserving one-shot callers.

The API is declared in
[LAGraph_CFPQ_RSM_prepared.h](include/LAGraph_CFPQ_RSM_prepared.h).
The handle borrows the graph/RSM arrays and their contents; keep them alive and
unchanged until `FreePrepared`. It stores no sources or query results. Static
matrices and borrowed input matrices are materialized before Prepare returns.
FreePrepared is NULL-safe and runs before borrowed inputs are released. Failure
leaves a NULL output; failed execution does not invalidate the plan.

The CFG_bench change is confined to `src/adapters/adapter_CFL_CFPQ_RSM.c`.
[The adapter-only patch](tooling/cfg_bench_prepared.patch) discovers the three
optional entry points using `dlsym`. Old libraries with none of them use the
existing one-shot path; a partial API is rejected explicitly. No parser, flags,
generic benchmark loops or other adapters change. The adapter keeps the library
loaded through plan destruction and retains the final result-materialization
wait inside the measured query.

CFG_bench currently prepares/cleans up per chunk. Thus this feature reuses one
plan across that chunk's warm-up and five consecutive measurements, **not across
different chunks**. Construction time is printed separately in each adapter log
as `RSM timing: prepared query; static construction ... (excluded, once per chunk)`.
Query time still includes allocation/reset/free of query scratch and source
seeding. There is no global cache, workspace reuse, profiling or OpenMP override.

Historical prepared timing recipe (construction excluded), from this directory:

```bash
python3 run_simple.py --allow-archived --input-manifest inputs/pinned-prepared.json \
    --variants baseline p1_review p1_prepared --source-sizes 1,10,100 \
    --chunks 30 --rounds 5 --hot --dry-run
```

Do not use this recipe for end-to-end comparisons. `p1_review` includes
construction; `p1_prepared` excludes it. Baseline retains its own one-shot static
preprocessing. These are different timing scopes, **not prepared-vs-prepared
baseline speedups**. The selected manifest records these scopes and pins the
compatible binary, new API header and unchanged input/runtime files. The runner
never rebuilds cfg_bench and saves the selected scope in `settings.json`.

The default [inputs/pinned.json](inputs/pinned.json), binary and frozen variants are
unchanged. [inputs/pinned-prepared.json](inputs/pinned-prepared.json) identifies
the new compatible binary. Its source was built from a scratch copy of the
current CFG_bench checkout with only the adapter patch added; existing local
changes in either repo are preserved. The registry snapshot is `p1_prepared`,
with a diff against `p1_review`; no existing snapshot was overwritten.

Bounded verification evidence is under
[runs/2026-11-11_16-20-48_prepared-implementation/](runs/2026-11-11_16-20-48_prepared-implementation/):

- `oracle-existing.log`: legacy wrapper, 19,462 exact-oracle checks (200 generated
  cases) and the 10 existing C tests pass.
- `prepared-tests.log`: 4,377 checks pass for A/B/A reuse, different source
  counts, empty queries, invalid calls followed by valid reuse, forward/dual
  grouping, nullable recursion and NULL-safe destruction. Tracked allocation
  failure budgets 0–86 for preparation and 0–975 for execution all clean up.
  These budgets cover the harness scenario, not every possible input path.
- `prepared-asan-no-lsan.log`: the same suite passes ASan/UBSan. Linked
  LAGraph/GraphBLAS libraries are not sanitizer-instrumented. LeakSanitizer cannot
  operate under the executor's tracing (`prepared-asan.log` retains that failure);
  tracked allocator counts independently return to their pre-trial values.
- `prepared.csv` / `legacy.csv`: the scratch adapter's 20 measured results each
  match; four chunks cause four preparations, each followed by a warm-up and
  five measurements. `partial-api.log` confirms rejection of an incomplete API.
- `simple-run.log`: the simple runner builds frozen baseline and prepared P1,
  exercises source sizes 1/2, two tiny chunks and five consecutive measurements,
  and passes count/hash checks.
- `reduced-prepared.log` / `reduced-legacy.log`: all 15 reduced-data cases pass
  in both prepared and legacy fallback modes. `verification.json` records the
  final source/binary identities and confirms that the applied adapter is
  byte-identical to the tested scratch source.
- `tooling-tests.log`: all 71 Python tooling tests pass. All 87 registered
  source/diff applications were checked in scratch copies. The prepared API is
  explicitly unsupported by the legacy profile generator; no profiler was added.

Direct prepared tests can be repeated without a benchmark:

```bash
make -C tests check-prepared BIN_DIR=/tmp/cfpq-prepared-tests
```

The original implementation validation did not run full performance measurements.
A later Java-only run is preserved under
[runs/2026-11-11_17-02-42_java_compare/](runs/2026-11-11_17-02-42_java_compare/);
its P1 measurements exclude static construction. Old campaign receipts are not
relabeled as validation of a different API or timing scope.
