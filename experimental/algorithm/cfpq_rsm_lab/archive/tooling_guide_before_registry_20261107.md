# CFPQ RSM experiment tooling: handoff to GPT 6 Astra

Verified against the local working trees on **2026-11-07**, after the four reproducibility fixes described below. This is a guide to the existing environment, not a record of a new performance experiment. Commands marked as measurements can take a long time; they were inspected and checked through CLI help or dry runs when preparing this document, rather than executed as full benchmarks.

## 1. Task and working rules

Optimize `/home/hollow/repo/LAGraph/experimental/algorithm/LAGraph_CFPQ_RSM.c` while preserving its API and reachability semantics. Use the existing correctness harness, input preparation, timing runner, profiler, and analysis tools. **Do not write a replacement benchmark, timer, correctness oracle, synthetic performance generator, or result-aggregation pipeline.** Small source snapshots and config selections are inputs to the existing tools, not new harnesses.

Keep a frozen pre-change reference and make one explainable change per measured candidate. Validate correctness before interpreting timing. Preserve raw results, failures, commands, source identities, and dependency identities. Keep benchmark processes sequential. Do not silently pool historical results from different controls or harness revisions.

The commands below use Bash and this host's absolute checkout paths. Run the initialization block first in a shell and keep its variables for subsequent blocks:

```bash
LAG=/home/hollow/repo/LAGraph
LAB="$LAG/experimental/algorithm/cfpq_rsm_lab"
CFG=/home/hollow/repo/CFG_bench
PRODUCTION="$LAG/experimental/algorithm/LAGraph_CFPQ_RSM.c"
TOOLBIN="$LAB/build/tooling/cfg"
HISTORICAL_RUN="$LAB/build/runs/current-vs-bb7-2026-11-04_00-17-29"
cd "$LAG"
```

`TOOLBIN` and new run directories below are generated outputs, not required preexisting inputs. Other named inputs were checked to exist. This is an existing-host recipe; it does not install GraphBLAS/LAGraph from scratch.

The applicable external instructions are in `/home/hollow/repo/CFG_bench/AGENTS.md`: use `configs/for_test.csv` for initial tests, check large graph/grammar file sizes before reading whole files, and finish affected-algorithm validation on `tests/data/configs/all.csv`. Honor any newer user instructions that narrow the permitted work or benchmark budget.

## 2. Tool and source map

All paths in the first table are relative to the absolute `LAB` directory defined above.

| Path under `LAB` | Purpose |
| --- | --- |
| `run.py` | Builds isolated RSM shared libraries, invokes CFG_bench, checks results, writes timing/profile data and provenance. Primary tool for optimization experiments. |
| `tests/Makefile` | Builds a selected RSM source directly with existing regression/oracle tests. Includes allocation-failure and sanitizer targets. |
| `tests/test_ds00.c`, `tests/oracle.c`, `tests/oracle.h` | Deterministic and generated small correctness cases, independent explicit fixed-point oracle. Not performance datasets. |
| `tests/test_run_acceptance.py`, `tests/test_run_order.py`, `tests/test_reproducibility.py` | Cheap Python checks for runner acceptance, scheduling, profiling generation, and input manifests. |
| `tests/make_profile_variant.py`, `variants/cfpq_profile.h` | Optional instrumentation generator and diagnostic support. Timing builds use uninstrumented sources. |
| `configs/lab.csv`, `configs/lab_batik.csv` | Seven original workloads; eight workloads after adding Batik. |
| `variants/*.c` | Existing complete candidate/reference snapshots. A filename is not evidence of an accepted optimization. |
| `patches/README.md` | Patch-composition instructions. Current active `patches/` has no optimization patches; historical patches are archived. |
| `variants/compare_bb7_current/` | Frozen historical baseline, frozen optimized source, manifest, and source diff for the completed comparison. |
| `run_current_vs_bb7.sh`, `summarize_current_vs_bb7.py` | Exact historical three-series experiment and its strict workload-level summary. |
| `figures/current_vs_bb7/plot_comparison.py` | Existing SVG/PNG bar-chart generator for the exact eight-workload, three-series layout. |
| `archive/2026-11-03/` | Immutable earlier sources, patches, inventories, summaries, provenance, and evidence archive. |

Additional existing tools:

| Absolute path | Purpose |
| --- | --- |
| `/home/hollow/repo/CFG_bench/src/test.c` | CLI, measured-run boundaries, deterministic source chunks, test mode. |
| `/home/hollow/repo/CFG_bench/src/adapters/adapter_CFL_CFPQ_RSM.c` | RSM preparation, dynamic variant dispatch, completed-result timing boundary, result hash. |
| `/home/hollow/repo/CFG_bench/src/memory.c` | Linux process peak RSS collection and peak reset. |
| `/home/hollow/repo/CFG_bench/tools/download_graph.py` | Fetches/converts FLPQ_Data graphs and copies CNF/RSM grammars. |
| `/home/hollow/repo/CFG_bench/tools/make_test_dataset.py` | Deterministic reduced samples of real graphs. |
| `/home/hollow/repo/CFG_bench/src/pairs_extractor.c` | Existing all-pairs reference-based pair/start-vertex extractor. |
| `/home/hollow/repo/CFG_bench/tools/run_experiment.py` | Coursework pipeline: extract candidates, run built-in algorithms, aggregate. Does not select RSM source variants. |
| `/home/hollow/repo/CFG_bench/tools/aggregate_experiment.py` | Coursework CSV aggregation, with different statistics from the lab runner. |
| `/home/hollow/repo/CFG_bench/tools/plot_scatter.py` | Existing generic raw-CSV scatter plotter. |
| `/home/hollow/repo/CFG_bench/tools/run_test_dataset.sh` | Limited-time reduced-dataset correctness sweep of linked built-in algorithms. |
| `/home/hollow/repo/LAGraph/experimental/test/test_CFPQ_RSM.c` | Additional existing C unit tests, built by the lab's `existing` target. |

## 3. Reproducibility fixes now present

1. **CFG_bench source and executable interface:** source support for `--rsm-lib`, optional adapter result hashes, the nine-column result CSV, and `-ldl` linkage has been restored. A new source build can now reproduce the interface that the lab runner requires. Rebuild from source; an old prebuilt executable alone is insufficient evidence.
2. **Profiler generation:** `tests/make_profile_variant.py` accepts both the current optimized source and the corrected frozen lab baseline. It preserves the complete input source, including the production file's opening cleanup macro, and accounts for bulk call-matrix construction. Anchors must still occur exactly once. A future structural rewrite may require a targeted update to the existing generator; never suppress an anchor failure. In profile mode, `run.py` now builds its uninstrumented control from the exact `--baseline` input it instruments, avoiding an unrelated `variants/baseline.c` shadowing that input.
3. **Test defaults:** `CANDIDATE` and `R2` default to the actual production source. `candidate_profile_r2` generates its profiling source from the selected `R2` into `BIN_DIR`; absent historical `correctness*.c` snapshots are no longer prerequisites. `make production` propagates failures.
4. **Input identity:** `run.py` now requires and hashes the `.rsm` file derived from every grammar path, in addition to graph, CNF, and supplied start files. It appears in `inputs.sha256` and `provenance.txt`.

These are working-tree changes across **two repositories**. When transferring the environment, preserve both repos' diffs and untracked experiment assets; a LAGraph commit alone does not capture the CFG_bench fixes.

## 4. Build and environment preflight

The verified environment has Python 3.11+ functionality available, a C compiler, Make, installed SuiteSparse:GraphBLAS headers under `/usr/local/include/suitesparse`, and these libraries:

- `/home/hollow/repo/LAGraph/build/src/liblagraph.so` and `build/src/test/liblagraphtest.so` for the C test harness; this build is configured as **Debug**.
- `/home/hollow/repo/LAGraph/build-exp-release/experimental/liblagraphx.so` for the full CFG_bench algorithm registry; this build is configured as **Release**.
- `/home/hollow/repo/GraphBLAS/build/libgraphblas.so`, used by the test Makefile's default search path.
- `/usr/local/lib/liblagraph.so` and `/usr/local/lib/libgraphblas.so`, used by default RSM variant builds. The actual dynamically resolved files must be checked in provenance; matching names do not prove identical builds.

Build a dedicated CFG_bench executable from the fixed source using the existing Makefile. This avoids overwriting the sibling checkout's default executable and explicitly selects the library with the required experimental symbols:

```bash
mkdir -p "$TOOLBIN"
make -B -C "$CFG" BUILD_DIR="$TOOLBIN" \
  INCLUDES="-I/usr/local/include/suitesparse -I$LAG/include -I$CFG/src -I$CFG/src/adapters" \
  LDLIBS="-L$LAG/build-exp-release/experimental -llagraphx -llagraph -lgraphblas -ldl -Wl,-rpath,$LAG/build-exp-release/experimental"
"$TOOLBIN/cfg_bench" -h
"$TOOLBIN/pairs_extractor" -h
ldd "$TOOLBIN/cfg_bench"
python3 "$LAB/run.py" --help
```

Expected outputs: `TOOLBIN/cfg_bench` and `TOOLBIN/pairs_extractor`; CLI help includes `--rsm-lib`, `--experiment`, and `--results-file`; `ldd` must show resolved libraries rather than `not found`. A plain `make` can select an installed `liblagraphx` lacking required symbols on this host, so retain the explicit link flags above.

All variant commands below pass both `--cfg-bench "$TOOLBIN/cfg_bench"` and `--skip-cfg-build`. Without the skip option, `run.py` builds the default `CFG/build` location; specifying a custom executable does not redirect its Make build. Its ordinary automatic build already supplies the appropriate includes, `-ldl`, and experimental library path.

The normal variant flags are `-O2 -fPIC -shared -std=c11`, with `-llagraph -lgraphblas`. Keep flags and resolved dependencies equal across RSM candidates. The runner records compiler identity, flags, source/header/library identities, and a build stamp. Do not rely on `--skip-variant-build` for new experiments: reused libraries require matching stamps, and normal output-directory protection/cleanup makes it unsuitable as a general resume mechanism.

Timing requires Linux `/proc/self/status` and writable `/proc/self/clear_refs`. CFG_bench resets `VmHWM` by writing `5` to `clear_refs`; a reset failure aborts the run. JIT compilation needs a writable cache. The runner sets `GRAPHBLAS_CACHE_PATH` to `OUT/jit-cache` by default and leaves GraphBLAS JIT control at its library default.

## 5. Know which baseline you are using

Verified SHA-256 values:

| Source | Identity and purpose | SHA-256 |
| --- | --- | --- |
| `LAB/variants/baseline.c` | Corrected older lab reference; **not** the requested bb7 historical commit. | `f3642839be04eee2d6411d3db9d6973c50c19f386d7e61f4d9ff5a926af0447b` |
| `LAB/variants/compare_bb7_current/baseline.c` | Source from commit `bb7dc96e66a99ca7628374c177f04d5dab36bd92`, verified byte-for-byte with `git show`. | `2eb47b26cfb729710424c5aee0dbad1fafb870d2cfefec450f9fc0c7bd8efae8` |
| `LAB/variants/compare_bb7_current/current.c` | Frozen optimized R13 source. | `c7efb94f21bbbddc220ab8371d01cd4c852e3ab110d5942367932676f8916928` |
| `PRODUCTION` | Live source; currently identical to the frozen R13 source, but will change during optimization. | `c7efb94f21bbbddc220ab8371d01cd4c852e3ab110d5942367932676f8916928` at verification |

Timing-mode source resolution checks `--variants-dir/<name>.c` **before** falling back to `--baseline` and patches. Thus `--baseline SOME_FILE --variants baseline,current` with the default variants directory can silently select the old lab `baseline.c`. Always use an explicit directory containing the intended named source snapshots.

For a new optimization, freeze the current production source **before editing it**:

```bash
EXPERIMENT=$(mktemp -d "$LAB/build/astra-XXXXXX")
mkdir -p "$EXPERIMENT/variants"
cp "$PRODUCTION" "$EXPERIMENT/variants/baseline.c"
```

After making the authorized algorithm change, snapshot it for tests and measurements:

```bash
cp "$PRODUCTION" "$EXPERIMENT/variants/current.c"
CANDIDATE="$EXPERIMENT/variants/current.c"
sha256sum "$EXPERIMENT/variants/baseline.c" "$CANDIDATE"
```

Refresh only the candidate snapshot when iterating; never replace the frozen control. Every changed candidate needs a fresh output directory. For comparison with bb7 instead, copy `LAB/variants/compare_bb7_current/baseline.c` as the control before running, and label that baseline explicitly in the report. Do not modify historical snapshots.

Patch variants use `--patches-dir` and names such as `A+B`, applying `A.patch` then `B.patch` to a copy of `--baseline`. Existing historical patches under `LAB/archive/2026-11-03/patches/` have specific parents and may not apply to the live source. Full candidate snapshots are the simplest existing mechanism for new work.

## 6. Inputs, datasets, and source selection

A CFG_bench config has no header; a row is:

```text
graph_path,grammar_cnf_path,expected_count_or_-1,start_vertices_path
```

The fourth column is optional outside source-chunk mode. Config paths may be absolute. Relative **input fields** resolve from the CFG_bench working directory, not the config file's directory. The RSM file is the CNF filename with suffix replaced by `.rsm`. Do not substitute a similarly named graph from a different layout (`java/` and `java_points_to/`, for example).

| Existing config | Cases and role |
| --- | --- |
| `CFG/configs/for_test.csv` | One tiny `wc.g` / `c_alias.cnf` case with starts; initial CLI/loader smoke and profiling validation. Total discovered input size is 6,256 bytes. |
| `CFG/tests/data/configs/all.csv` | 15 reduced real-graph cases, three per category: C alias, RDF, Java points-to, context-sensitive data flow, field-sensitive alias. Graph/CNF/RSM/starts all exist. Total discovered input size is 7,500,798 bytes. |
| `LAB/configs/lab.csv` | Seven performance workloads: avrora, eclass, go, go_hierarchy, mm, init, block. |
| `LAB/configs/lab_batik.csv` | Those seven plus Batik with the Java points-to query; the latest comparison's eight-workload matrix. |
| `CFG/configs/experiment.csv` | Eight coursework graph/query rows, without start pools; use the existing extraction pipeline before chunked runs. This is not interchangeable with `lab_batik.csv`. |

Verified `lab_batik.csv` graph sizes and start-pool line counts:

| Graph | Graph bytes | Start-pool entries |
| --- | ---: | ---: |
| avrora | 997,106 | 8,953 |
| eclass | 14,735,728 | 30,336 |
| go | 70,915,500 | 36,704 |
| go_hierarchy | 21,369,304 | 7,938 |
| mm | 78,572,016 | 1,692,161 |
| init | 75,609,822 | 1,630,815 |
| block | 107,367,376 | 2,282,155 |
| batik | 2,533,318 | 19,462 |

The start files are already named in both lab configs under `CFG/results/experiment_2026-10-26_20-59-13/candidates/`. Reuse them; extracting starts again can be expensive. Graph paths, grammars, RSMs, and these candidate files were all checked with `run.py`'s existing `check_config()`.

In `--experiment` mode CFG_bench shuffles the candidate pool deterministically with `--seed` (2026 here), then uses consecutive source chunks. If `chunks * source_size` exceeds the pool size, selection wraps cyclically. Pools contain vertices known to have reachable pairs, not a uniform sample of all graph vertices; chunks are not generally independent random experiments. Keep seed, source file, source size, and chunk IDs identical across variants. `--source-size 0` in `run.py` selects normal mode using all supplied starts; it is a different workload and cannot be used for profile mode.

## 7. Correctness gates: use the existing harness

Run the cheap Python tooling checks:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s "$LAB/tests" -p 'test_*.py'
```

Expected: unittest output ending in `OK` (34 tests at this verification).

Compile and smoke-test the exact candidate snapshot:

```bash
make -C "$LAB/tests" R2="$CANDIDATE" \
  BIN_DIR="$EXPERIMENT/tests" candidate_correctness_r2
"$EXPERIMENT/tests/candidate_correctness_r2" --smoke
```

Expected for the current source: `checks=453 failures=0` and `PASS: --smoke checks`. This is a quick gate, not the entire suite. Run broader checks as appropriate to the change:

```bash
make -C "$LAB/tests" R2="$CANDIDATE" BIN_DIR="$EXPERIMENT/tests" check
make -C "$LAB/tests" R2="$CANDIDATE" BIN_DIR="$EXPERIMENT/tests" check-cases N=500
make -C "$LAB/tests" R2="$CANDIDATE" BIN_DIR="$EXPERIMENT/tests" existing
make -C "$LAB/tests" R2="$CANDIDATE" BIN_DIR="$EXPERIMENT/tests" asan-fast
make -C "$LAB/tests" R2="$CANDIDATE" BIN_DIR="$EXPERIMENT/tests" asan
make -C "$LAB/tests" R2="$CANDIDATE" BIN_DIR="$EXPERIMENT/tests" alloc
```

These are alternatives/stages, not a requirement to rerun everything after every edit. `check` includes deterministic regressions plus 200 generated oracle cases; `check-cases` selects the count. For reproducible case replay, the compiled binary accepts `--cases N --seed S` (default seed 2026). Coverage includes recursion, epsilon, multiple/no finals, empty dimensions/graphs, repeated sources, stored false values, indexed labels, malformed inputs, and dense relations. The explicit oracle does not use a fixed path-length cutoff. A failure returns nonzero and prints diagnostics; preserve its seed.

`existing` runs the additional Acutest-based suite. `asan-fast` uses 20 random cases without leak detection; `asan` includes 200 cases and leak detection. The candidate/test driver are instrumented, but the linked LAGraph/GraphBLAS libraries are not. `alloc` sweeps allocation-failure budgets in child processes and checks cleanup/output validity; it can take longer than smoke tests. `CANDIDATE=` controls the older targets lacking `_r2`; `R2=` controls the commands above. Default `BUILD`, `GRAPHBLAS_LIBDIR`, and `GRAPHBLAS_INCDIR` are shown in `tests/Makefile` and can be overridden for another installation.

## 8. Tiny end-to-end timing smoke, then reduced correctness

Use `run.py` even for the initial timing smoke, so loading, hashes, result acceptance, manifests, and CSV parsing are all exercised:

```bash
python3 "$LAB/run.py" \
  --baseline "$EXPERIMENT/variants/baseline.c" \
  --variants-dir "$EXPERIMENT/variants" --variants baseline,current \
  --cfg-bench-dir "$CFG" --cfg-bench "$TOOLBIN/cfg_bench" --skip-cfg-build \
  --config "$CFG/configs/for_test.csv" \
  --source-size 1 --chunks 1 --rounds 1 --hot-rounds 1 \
  --seed 2026 --threads 1 --order blocked \
  --out "$EXPERIMENT/smoke"
```

Expected: two raw CSVs, a two-row `summary.csv`, status `ok`, equal nonzero RSM hashes/counts, and exit 0. This compiles `EXPERIMENT/smoke/libs/current.so`; use that exact candidate library for external correctness checks:

```bash
cd "$CFG"
env OMP_NUM_THREADS=1 GRAPHBLAS_CACHE_PATH="$EXPERIMENT/correctness-jit" \
  "$TOOLBIN/cfg_bench" -t -c configs/for_test.csv -a CFL_CFPQ_RSM \
  --rsm-lib "$EXPERIMENT/smoke/libs/current.so" --use-start-nodes --compute-results
env OMP_NUM_THREADS=1 GRAPHBLAS_CACHE_PATH="$EXPERIMENT/correctness-jit" \
  timeout 60 "$TOOLBIN/cfg_bench" -t -c tests/data/configs/all.csv -a CFL_CFPQ_RSM \
  --rsm-lib "$EXPERIMENT/smoke/libs/current.so" --use-start-nodes --compute-results
cd "$LAG"
```

Expected: one `[OK]` line per config, `Return code: 0`, and equal reported/reference counts. The reduced suite passed all 15 cases during fix verification. `-t` ignores `-r` and `--hot`; it does **not** produce the usual timing CSV. The optional timeout limits total reduced-check time; exit 124 means incomplete validation, not an algorithm timing result.

`--compute-results` runs `CFL_adv -efbl` on the same data to derive the reachable-vertex count. From the CFG_bench root it may reuse `.cache/computed_results.csv`. Its cache key uses graph/CNF/start paths and integer modification times, not content hashes or the derived `.rsm`; this is a CNF reference count, and agreement of CNF/RSM query semantics remains essential. Counts are weaker evidence than the lab's vertex-set hash and small explicit oracle. Preserve which checks used cached values.

The existing sweep helper can also be inspected or used:

```bash
bash "$CFG/tools/run_test_dataset.sh" -a CFL_CFPQ_RSM -t 10 --dry-run
```

Remove `--dry-run` to execute it. It uses `CFG/build/cfg_bench` and the **linked** implementation, with all-start and supplied-start variants; it has no `--rsm-lib` passthrough. Therefore it does not validate a new isolated candidate unless that candidate was separately built into its linked library. It writes `CFG/results/test_dataset_<timestamp>/log.txt`, reports per-variant `N/N OK`, and exits nonzero on failures. A 10 GiB memory limit is applied only when `systemd-run --user` works; otherwise it warns and runs without that limit. Prefer the explicit loaded-candidate commands above for optimization work.

## 9. Main performance comparison

This is a **full measurement**, not a cheap check. The current experiment design is eight `lab_batik` workloads, **five measured rounds plus one unmeasured warm-up per display/workload/chunk**, 30 chunks, seed 2026, one baseline series, candidate series, and fully optimized CFL. Do not duplicate the baseline merely because the runner's default is `baseline,baseline`.

```bash
SOURCE_SIZE=100
THREADS=0
TIMING_OUT="$EXPERIMENT/timing-s${SOURCE_SIZE}-t${THREADS}"
python3 "$LAB/run.py" \
  --baseline "$EXPERIMENT/variants/baseline.c" \
  --variants-dir "$EXPERIMENT/variants" --variants baseline,current \
  --include-cfl-multsrc-adv \
  --cfg-bench-dir "$CFG" --cfg-bench "$TOOLBIN/cfg_bench" --skip-cfg-build \
  --config "$LAB/configs/lab_batik.csv" \
  --source-size "$SOURCE_SIZE" --chunks 30 --rounds 5 --hot-rounds 1 \
  --seed 2026 --threads "$THREADS" --order blocked --keep-going \
  --out "$TIMING_OUT"
```

At these settings, expect 15 CFG_bench passes (3 series × 5 rounds), 240 measured rows per pass (8 workloads × 30 chunks), 3,600 raw rows total, and 720 chunk-level summary rows. Warm-ups are excluded from raw timing CSVs. The three display names are `baseline`, `current`, and `CFL_multsrc_adv`.

Important runner semantics:

- `--hot-rounds 1` adds CFG_bench `--hot` only to round 0. Each later measured round is a new process without another per-invocation warm-up. This matches the agreed experiment, but does not mean every process is warmed. The run-local JIT cache is shared across displays/rounds, so order and JIT state can influence results.
- `--order blocked` completes all baseline rounds, then candidate rounds, then CFL rounds. `rotated` is the runner default and rotates display order across rounds. Retain the agreed blocked design for historical comparability; report its exposure to time drift. A different order is a separately labelled experiment.
- `--threads 0` unsets `OMP_NUM_THREADS` and `OMP_THREAD_LIMIT`; it does not force serial operation, fix a core count, pin CPUs, or record actual active GraphBLAS threads. Positive values set `OMP_NUM_THREADS=N`; an inherited `OMP_THREAD_LIMIT` is not removed in that mode. Record and control relevant inherited environment when interpreting scaling.
- `--keep-going` retains failures and attempts later passes. It does not make a failed comparison acceptable. Exit code, `execution_status.tsv`, and completeness checks must all be reviewed.
- `--include-cfl-multsrc-adv` selects built-in `CFL_multsrc_adv -e -f -l -b` (all four optimizations, mask 15), without `--rsm-lib`. It is **timing-only**: execution/row completeness are checked, but CFL result count/hash equality is deliberately not checked. Its hash is 0. A CFL `ok` or `MATCH` is not a cross-algorithm correctness proof.
- For source-size coverage, rerun the same existing command separately with `SOURCE_SIZE=1` and `10`, each with a new output directory. For a thread comparison, use a separate labelled invocation with `THREADS=1` or another explicit setting; do not combine these samples with unrestricted runs.
- A provenance-only preflight uses the same timing command plus `--provenance-only` and a separate output directory. It builds and hashes inputs but runs no queries. Do not then reuse that nonempty directory for measurements.

Output protection rejects existing run-owned artifacts unless `--force` is supplied. `--force` deletes run-owned outputs; it is not resume. Choose a fresh directory and keep partial runs rather than filling missing rounds manually.

## 10. Timing, result, memory, and provenance formats

For any `OUT` from timing mode:

| Output | Format and interpretation |
| --- | --- |
| `raw/<display>/round0.csv` through `round4.csv` | One measured row per workload/chunk. Display names are sanitized for filenames. |
| `summary.csv` | One row per display/workload/source-size/chunk, aggregating the measured rounds. |
| `execution_status.tsv` | `display,spec,round,start,finish,exit_code,outcome,log`, tab-separated. One row per attempted child pass. |
| `logs/<display>/roundN.log` | Exact child command and combined stdout/stderr. Build logs are under `logs/build/`. |
| `libs/*.so`, `libs/*.stamp` | Actual measured shared libraries and source/dependency/compiler build keys. |
| `provenance.txt` | Source/binary/config/tool identities, machine/compiler/thread/JIT settings, resolved dependencies, input identities, repo state, and invocation. |
| `inputs.sha256` | **Not plain `sha256sum` format**: each line is `sha256:size:mtime  absolute_path`. Includes the fixed `.rsm` discovery. |

Raw CSV header:

```csv
algo_name,cnf,graph,source_size,chunk,result,result_hash,max_memory_kb,time_s
```

`time_s` is wall-clock seconds around `adapter.run()`. RSM adapter timing now includes `GrB_Vector_wait(..., GrB_MATERIALIZE)` to complete its final result. Parsing, graph/RSM adapter preparation, output initialization, result hashing, and adapter cleanup are outside that timer; algorithm-internal setup/cleanup remain inside its call. Large preparation costs may dominate total experiment duration even when the recorded kernel time is short. Do not report raw `time_s` as end-to-end query latency.

RSM `result` is the reachable-vector stored cardinality; its optional order-independent 64-bit hash combines that cardinality with true-valued reachable vertex indices. Hash equality is a useful guard against equal-count wrong sets, not a mathematical proof or a replacement for the oracle. The runner requires a nonzero, stable RSM hash/count matching the reference for every expected round and case. It rejects missing/extra/duplicate cases and missing rounds. Direct CFG_bench timing alone does not provide all these checks.

`max_memory_kb` is **whole-process** `VmHWM` sampled after the run, after an attempted peak reset and `malloc_trim(0)` before it. Prepared graph/RSM/runtime allocations still count. It is not isolated algorithm workspace or a portable allocator-byte measurement. Repeated preparation/allocator/JIT state and the memory-reset environment matter. The summary records memory medians/min/max and labels its scope.

Summary CSV header:

```csv
variant,graph,cnf,source_size,chunk,rounds_seen,n_runs,ref_median_s,median_s,iqr_s,min_s,max_s,ratio,result,result_hash,count_ok,hash_ok,status,median_memory_kb,min_memory_kb,max_memory_kb,memory_scope
```

For RSM, `ratio = median(candidate round times) / median(reference round times)` per chunk: below 1 is faster; speedup is its reciprocal. IQR uses inclusive quartiles. Console `geo_ratio` is the geometric mean of those chunk ratios. For timing-only CFL, the generic runner leaves correctness flags blank and its reference-ratio fields are not a usable speedup (currently zero); use the existing workload summarizer for CFL comparisons.

Provenance records installed library content identities and repo dirty/untracked status but does not copy every untracked file or capture every OS/runtime setting. Archive the actual candidate/reference snapshots with results. Do not treat the printed invocation as a shell-escaped replay script; historical shell wrappers separately save escaped `commands.sh` files.

## 11. Profiling: diagnose phases without using instrumented times as speedups

Use the existing generator and runner. First validate the candidate on the tiny config:

```bash
python3 "$LAB/run.py" --profile --baseline "$CANDIDATE" \
  --cfg-bench-dir "$CFG" --cfg-bench "$TOOLBIN/cfg_bench" --skip-cfg-build \
  --config "$CFG/configs/for_test.csv" \
  --source-size 1 --chunks 1 --rounds 1 --seed 2026 --threads 1 \
  --out "$EXPERIMENT/profile-smoke"
```

Expected: generated `profile/src/correctness_profile.c`, two isolated libraries (`baseline.so` and `profile.so`), matching result rows, `profile.csv` with one measured call, `profile-calls.csv` with two calls, and exit 0. `--baseline` here means the source to instrument, not necessarily the historical performance control. The fixed runner builds its reference from this exact source even if the default variants directory contains another baseline.

For workload diagnosis use the same command with `--config "$LAB/configs/lab_batik.csv"`, the desired positive `--source-size`, desired chunks, and a fresh output directory. Profile mode always makes one warm-up and one measured call per chunk in each reference/instrumented pass. `--rounds 5` does **not** create five profiling rounds. `--hot-rounds` does not alter its warm-up scheme. `--include-cfl-multsrc-adv` is rejected in profile mode.

The generator can also be exercised through the existing C correctness harness:

```bash
make -C "$LAB/tests" R2="$CANDIDATE" \
  BIN_DIR="$EXPERIMENT/profile-tests" candidate_profile_r2
CFPQ_RSM_PROFILE_FILE="$EXPERIMENT/profile-tests/smoke.txt" \
CFPQ_RSM_PROFILE_TAG=smoke \
  "$EXPERIMENT/profile-tests/candidate_profile_r2" --smoke
```

It writes append-only raw diagnostic streams when `CFPQ_RSM_PROFILE_FILE` is set. Without that variable, diagnostic recording is inactive. Fresh output paths prevent mixed-call records. If generated-source equivalence is in question, compile that generated `.c` through `candidate_correctness_r2` without defining `CFPQ_RSM_PROFILE`, adding `-I$LAB/variants` to `CFLAGS`, and run the same existing oracle tests.

For runner profile outputs, `SLUG` is `<graph_stem>__<grammar_stem>__s<S>`:

| Output under `OUT` | Content |
| --- | --- |
| `profile.csv` | Measured calls, phase/suboperation counters/times, dimensions, sanity fields, case/chunk/round identity. |
| `profile-calls.csv` | Warm-up and measured identities with `warmup=1/0`, total time, and sanity status. |
| `profile/profile-SLUG.txt` | Semicolon-separated `key=value` summary per instrumented call, tagged with workload/source size/seed. |
| `profile/profile-SLUG.detail.csv` | Annotated iteration/box records: frontier/visited/candidate/next counts, fresh/overlap counts, box workspace summaries. |
| `profile/profile-SLUG.matrix.csv` | Named persistent matrix dimensions, nvals, format/iso/orientation/index-width metadata, allocation bytes and availability status. |
| `profile/profile-SLUG.vector.csv` | Named vector dimensions/nvals/allocation bytes and status. |
| `profile/results-reference-SLUG.csv`, `profile/results-profile-SLUG.csv` | Nine-column CFG_bench result rows for correctness acceptance. |

The runner annotates diagnostic CSVs with `chunk,warmup,round` and removes their intermediate `.txt.detail.csv`, `.txt.matrix.csv`, and `.txt.vector.csv` files. Manual instrumented runs retain those intermediate names.

Important fields include `setup_s`, `activity_s`, `phase1_s`, `phase2_3_s`, `phase4_s`, `phase5_s`, `final_s`, `materialize_s`, `cleanup_s`, per-phase boundary waits, `diag_io_s`, `total_s`, `kernel_s`, and `unclassified_s`. `kernel_s = total_s - diag_io_s`; subtraction does not remove all instrumentation effects. Explicit waits change GraphBLAS execution scheduling. Interpret phase API time together with its completion-boundary wait, and distinguish nested suboperation times from exclusive phase totals.

Require valid tags, expected call counts and shapes, `sanity_ok=1`, and inspect `detail_truncated` plus byte-status fields. Reconciliation tolerance is `1e-4 + 0.02 * kernel_s`. Detail storage is bounded (65,536 iterations, 8,192 boxes). Matrix/vector bytes are snapshots for the named objects, not peak temporary memory; the current snapshot list does not comprehensively enumerate every optimized call-tuple buffer or matrix. A sane profile can still be incomplete for allocation attribution.

Timing builds must remain uninstrumented. Profile the optimized and reference sources separately if phase attribution is needed for both; do not infer a candidate speedup from instrumented/reference timing overhead ratios.

## 12. Reproduce and analyze the completed historical comparison

The wrapper is specifically frozen bb7 versus frozen R13 optimized RSM plus optimized CFL. It **does not benchmark future edits to `PRODUCTION`**. It checks the two source hashes and uses one baseline series, five rounds, one warm-up, 100 sources, 30 chunks, unrestricted threads, blocked order, and the eight-workload config.

```bash
bash "$LAB/run_current_vs_bb7.sh" --dry-run
```

Expected: the exact runner and summarizer commands, with no build or workload execution. To deliberately rerun the full historical experiment:

```bash
bash "$LAB/run_current_vs_bb7.sh" "$EXPERIMENT/historical-replay"
```

This wrapper uses the automatic/default CFG_bench build rather than `TOOLBIN`. It saves `commands.sh`, `command`, console logs, `exit_codes.tsv`, `snapshot_manifest.txt`, a config copy, runner artifacts, and `morning_summary.csv`/`.md`. It attempts summarization even after runner failure and preserves both exit codes.

Reuse the existing strict summarizer for a completed new comparison with exactly the main command's series names and eight-workload/100-source/30-chunk/five-round layout:

```bash
python3 "$LAB/summarize_current_vs_bb7.py" "$TIMING_OUT"
```

It writes 16 candidate/workload rows (current and CFL × 8) in `morning_summary.csv` and `.md`, checks execution/completeness, and returns nonzero unless all are `MATCH`. Its descriptive historical labels do not establish which source you used: accompany new results with their actual provenance. It is hardcoded for this layout; do not use it for smoke, 1/10-source, arbitrary candidate names, or a different workload config.

Workload `time_ratio` is the **median of paired candidate/reference chunk ratios**, where each chunk time is the median of five rounds. `current_over_cfl_ratio` uses the same paired procedure for current/CFL. Speedup is the reciprocal. These differ from the ratio of two overall workload medians and from the runner's geometric mean across chunks. `MATCH` for CFL means timing completeness, with `result_check` documenting the skipped correctness comparison. Failed/incomplete comparisons must not supply performance claims.

The completed original run is `HISTORICAL_RUN`; its raw data, summary, status, provenance, and morning summary were verified present. Existing overview reports are `/home/hollow/repo/LAGraph/CFPQ_RSM_COMPARISON_RESULTS.md`, `/home/hollow/repo/LAGraph/CFPQ_RSM_OPTIMIZATION_HISTORY.md`, and `/home/hollow/repo/LAGraph/CFPQ_RSM_OPTIMIZATION_REPORT.md`. Use raw data/provenance to substantiate a claim; these reports are historical analysis, not fresh instructions or universal ablation evidence.

Create the existing comparison figure from that completed run, without rerunning algorithms:

```bash
python3 "$LAB/figures/current_vs_bb7/plot_comparison.py" \
  --run "$HISTORICAL_RUN" --output "$EXPERIMENT/historical-bars"
```

Expected: `historical-bars.svg`, `historical-bars.png`, and `historical-bars_data.csv`. Requires NumPy and Matplotlib, available in the current `python3` environment. The plotter asserts the exact layout (three named series, eight specified graph stems, five round files, 240 rows/file, 100 sources, 30 chunks). Bars are medians of chunk medians; whiskers are min/max across chunk medians, **not confidence intervals**. It does not replace result validation. For an identically shaped new comparison, pass `--run "$TIMING_OUT"` and a fresh output prefix. The script's caption hardcodes bb7 as the baseline: reuse it unchanged only for that control; otherwise correct the caption in this existing plotter before publishing the figure.

## 13. Other existing experiment wrappers

These remain useful for reproducing older experiments, not as defaults for the new single-baseline design. Every listed shell wrapper accepts `--dry-run [OUTPUT_ROOT]` and `[OUTPUT_ROOT]`; every matching summarizer accepts that output root as its sole positional argument. For example:

```bash
bash "$LAB/run_optimized_v1.sh" --dry-run
python3 "$LAB/summarize_optimized_v1.py" --help
```

| Wrapper under `LAB` | Matching summarizer | Fixed design |
| --- | --- | --- |
| `run_overnight.sh` | `summarize_overnight.py` | ds20/ds21/ds22 × source sizes 1/10/100; 9 separate comparisons. |
| `run_r9_overnight.sh` | `summarize_r9_overnight.py` | 10 candidates × 3 source sizes; 30 comparisons. |
| `run_r10_overnight.sh` | `summarize_r10_overnight.py` | 13 candidates × 3 source sizes; 39 comparisons. |
| `run_s100_selected.sh` | `summarize_s100_selected.py` | Six selected candidates plus opening/closing baseline displays in one 100-source run. |
| `run_optimized_v1.sh` | `summarize_optimized_v1.py` | baseline, optimized_v1, baseline#2 in one 100-source run. |

All use the old corrected lab baseline and `configs/lab.csv`, five measured rounds, one warm-up, 30 chunks, seed 2026, and `--threads 0`. The first three have duplicated baseline displays within each comparison; the last two use blocked order and shared opening/closing baselines. Shell console/status/command outputs and `morning_summary.csv`/`.md` retain failures. Do not launch them simply to evaluate one new candidate: the direct `run.py` command above already does that without redundant baselines or unrelated candidates.

Earlier evidence is under `LAB/archive/2026-11-03/`. The archive's `README.md`, `variants.json`, and `historical_inventory.csv` identify source parents and incomplete runs. Integrity verification is read-only but may read a large archive:

```bash
cd "$LAB/archive/2026-11-03"
python3 verify.py
cd "$LAG"
```

Do not overwrite this archive or execute its old shared libraries as portable replacements for their recorded dependencies. Several historical directions were combined or used different baselines; their reported speedups are not additive and are not automatically standalone ablations of the current algorithm.

## 14. Existing data preparation and coursework tools

Reuse installed inputs for optimization measurements. The following operations are for missing/additional inputs or reproducing data preparation, and can download data or run expensive all-pairs queries.

### FLPQ_Data dependency and reduced samples

`CFG/pyproject.toml` pins `flpq-data==6.0.0.dev15`, with `CFG/uv.lock` and an explicit TestPyPI source for that package. **At verification, `CFG/.venv/bin/python` could not import `flpq_data`.** Do not assume the generator environment is ready. Installation was not attempted while writing this guide.

The existing dependency manager recipe is:

```bash
cd "$CFG"
uv sync --locked
uv run --locked python tools/download_graph.py --help
uv run --locked python tools/make_test_dataset.py --help
```

This may need network access. Download one graph with the existing converter, into a separate output directory:

```bash
uv run --locked python tools/download_graph.py \
  --graph batik --out-dir "$EXPERIMENT/downloaded-data"
```

It writes `graphs/<category>/<graph>.g` plus `grammars/<category>/<graph>/*.{cnf,rsm}`. A single `--graph` does not create a category config; `--category java_points_to` downloads that category and writes `configs/java_points_to.csv`. Output conversion emits forward/reverse edges and expands indexed labels consistently with the grammars. New converted paths need explicit new configs; do not point old configs at an unrelated downloaded layout.

To reproduce the existing reduced-dataset generation protocol in a separate output tree:

```bash
uv run --locked python tools/make_test_dataset.py \
  --category all --out-dir "$EXPERIMENT/reduced-data"
mkdir -p "$EXPERIMENT/reduced-data/start_nodes"
"$TOOLBIN/pairs_extractor" -c "$EXPERIMENT/reduced-data/configs/all.csv" \
  -o "$EXPERIMENT/reduced-data/start_nodes" --start-only
uv run --locked python tools/make_test_dataset.py \
  --category all --out-dir "$EXPERIMENT/reduced-data"
cd "$LAG"
```

The generator uses seed 2026, grammar-label filtering, BFS-based connected sampling with per-graph edge budgets, vertex renumbering, and compact indexed labels. It copies CNF/RSM grammars and writes graph/category configs. The second generation attaches extracted start files as fourth columns. Console output gives graph edge/vertex counts. Expected graph/sample pair counts are built into the existing generator; changing the sampling protocol invalidates them. This is a regression sample collection, not a synthetic scaling benchmark.

### Candidate extraction without inventing a selector

The extractor runs `CFL_adv` with all optimizations. Output directories must exist. It may be expensive on full graphs:

```bash
mkdir -p "$EXPERIMENT/candidates"
cd "$CFG"
"$TOOLBIN/pairs_extractor" -c "$LAB/configs/lab_batik.csv" \
  -o "$EXPERIMENT/candidates" --start-only
cd "$LAG"
```

Output files are `<grammar_stem>_<graph_stem>_start.result`, one unique source vertex per line without a header. Without `--start-only`, output is `<grammar_stem>_<graph_stem>.result`, one reachable `u v` pair per line. Do not replace the existing experiment pools silently: record their new hashes and use a new config if regeneration is necessary.

### Coursework pipeline and aggregation

`CFG/tools/run_experiment.py` is an alternative existing pipeline for built-in CFL_adv/RSM/CFL_multsrc_adv comparisons. It uses `CFG/build/cfg_bench` and `CFG/build/pairs_extractor`; it has no `--rsm-lib` or isolated-variant selection. It is therefore **not the primary tool for source optimization**. If intentionally reproducing this pipeline after building those default binaries with the verified flags:

```bash
cd "$CFG"
make -B -C "$CFG" \
  INCLUDES="-I/usr/local/include/suitesparse -I$LAG/include -I$CFG/src -I$CFG/src/adapters" \
  LDLIBS="-L$LAG/build-exp-release/experimental -llagraphx -llagraph -lgraphblas -ldl -Wl,-rpath,$LAG/build-exp-release/experimental"
python3 tools/run_experiment.py \
  --config "$LAB/configs/lab_batik.csv" \
  --reuse-candidates "$CFG/results/experiment_2026-10-26_20-59-13/candidates" \
  --source-sizes 1,10,100 --chunks 30 --rounds 5 --adv-rounds 10 \
  --seed 2026 --no-build --out-dir "$EXPERIMENT/coursework"
cd "$LAG"
```

This is another full experiment, not an additional gate to run automatically. It writes `config_with_sources.csv`, `metadata.txt`, `raw/<algorithm>.csv`, `per_chunk.csv`, and `summary.csv`. Without `--reuse-candidates`, it invokes its existing extractor first. It skips existing raw files unless `--force` is supplied, so do not mistake skips for fresh measurements. Its metadata is less complete than the lab runner's provenance.

Reaggregate only an existing coursework run with:

```bash
python3 "$CFG/tools/aggregate_experiment.py" \
  "$EXPERIMENT/coursework/raw/CFL_adv.csv" \
  "$EXPERIMENT/coursework/raw/CFL_CFPQ_RSM.csv" \
  "$EXPERIMENT/coursework/raw/CFL_multsrc_adv.csv" \
  --chunks 30 --repeats 5 --all-pairs-runs 10 \
  --output "$EXPERIMENT/coursework/reaggregated.csv" \
  --per-chunk "$EXPERIMENT/coursework/reaggregated-per-chunk.csv"
```

This averages repeated runs per chunk, then computes mean/median/std/min/max **over chunk means**. Its per-chunk CSV has `n_runs,mean_time_s,median_time_s,mean_result,mean_max_memory_kb` plus case keys. Its summary has `n_measurements,mean_time_s,median_time_s,std_time_s,min_time_s,max_time_s,mean_result,mean_max_memory_kb` plus algorithm/graph/CNF/source-size keys. It warns on incomplete sample counts but still returns 0, and does not enforce RSM hash equality. It cannot distinguish RSM source variants sharing `algo_name`, so do not feed several lab variant series into it as though they were different algorithms.

The generic plotter accepts raw nine-column CFG_bench CSVs:

```bash
python3 "$CFG/tools/plot_scatter.py" \
  "$EXPERIMENT/coursework/raw/CFL_CFPQ_RSM.csv" \
  --algos CFL_CFPQ_RSM --sort name --output "$EXPERIMENT/coursework/rsm-scatter"
```

Expected after satisfying dependencies: SVG and PNG. It needs pandas, NumPy, and Matplotlib, which are separate from the pinned data dependency. **The current `python3` environment lacks pandas**, so this optional scatter command currently fails with `ModuleNotFoundError`; no dependencies were installed for this handoff. The comparison bar plotter above works with the available NumPy/Matplotlib environment. Despite stale prose in the scatter script's docstring, current `load()` groups raw repeats using **mean**, and `--sum-chunks` also uses the same mean aggregation with chunk removed. Do not describe that option as a sum or use the plot's spread as confidence intervals.

## 15. What the setup can establish, and what remains outside it

The fixed setup is sufficient for controlled before/after comparisons on the installed graph/query workloads, explicit small-case correctness checks, source-size experiments, process-memory comparisons with stated scope, and phase-level diagnosis of supported source layouts. It is not by itself a complete performance characterization of the algorithm.

| Question | Existing support and limit |
| --- | --- |
| Is the candidate correct? | Small deterministic/generated oracle cases, RSM count/hash acceptance, and reduced real-data checks. Historical or CFL timing-only status is not proof of equivalence. |
| Is it faster on current workloads? | Five rounds, paired chunks, raw data, IQR/range, retained failures, and frozen sources. No automatic confidence intervals or formal significance test; blocked order and a single baseline series do not estimate time drift independently. |
| Does it scale with source count? | Existing runner supports 1/10/100 or another positive size, with separate runs. Pools wrap and are reachability-biased, so explain sample dependence. |
| Does it scale with graph/grammar complexity? | Multiple real workloads and small generated correctness cases exist. There is no systematic performance sweep over vertices, edges/density, RSM states/boxes, recursion shape, or label counts. Do not infer asymptotic scaling from this corpus. |
| What causes the runtime? | Existing phase waits/timers, operation counters, frontier/visited/freshness/overlap records. Instrumentation changes scheduling and not every new optimization-specific allocation is represented. |
| What memory is needed? | Reset process peak RSS and selected GraphBLAS allocation snapshots. No complete peak live-temporary/allocator attribution. |
| What is the threading behavior? | OMP environment controls; no measured active-thread counts, affinity/frequency/NUMA control, or automatic scaling analysis. |
| Is it end-to-end latency or cold start? | Parsing-only CLI support exists, but kernel timer excludes adapter preparation; one first-round warm-up and shared JIT cache do not constitute a controlled cold/warm end-to-end study. |
| Are hardware bottlenecks measured? | No dedicated perf/PAPI/cache-miss/bandwidth/callgrind collection integrated into the lab runner. Do not claim those measurements from phase times alone. |

For the existing parsing-only facility, use:

```bash
cd "$CFG"
"$TOOLBIN/cfg_bench" -c configs/for_test.csv -a CFL_CFPQ_RSM --bench-parse
cd "$LAG"
```

It prints grammar/graph parse times to the console rather than the normal nine-column timing CSV. It does not measure all adapter preparation or create an end-to-end benchmark. Reuse it only to answer the narrower parsing question.

When reporting an optimization, state the exact baseline, candidate hash, dataset/query, source size, seed/chunks/rounds/warm-up, thread/order/JIT mode, timing boundary, validation status, runtime and memory scope, and per-workload regressions. Distinguish measured individual changes from combinations and unmeasured ideas. Prefer a bounded conclusion such as “faster on these eight workloads under this configuration” over a claim of universally improved performance.

## 16. Verification record for this guide

The named source/config/script paths and dependencies were inspected in the live repos, all five named configs passed existing input discovery/existence validation, source hashes were recomputed, and the bb7 snapshot was compared to its Git object. All 27 Bash blocks passed syntax checks; nine documented Python CLI invocations passed parser checks, and ten Make invocations passed target/flag dry runs. All six lab shell wrappers also passed syntax and dry-run checks. Data-generator options and the generic scatter parser were checked from source because `flpq_data` and pandas are currently absent in their respective environments. Generated output paths are explicitly identified above; they need not exist before a command is run.

The existing historical summarizer was run on a temporary copy of saved inputs and produced 16 `MATCH` rows. The existing comparison plotter regenerated SVG, PNG, and a 24-row data CSV under `/tmp` from saved results. The parsing-only CLI was exercised on `configs/for_test.csv`. No algorithm timings were rerun for those analysis/output checks.

The preceding reproducibility-fix validation passed a fresh CFG_bench build, 34 Python tooling tests, 453 smoke checks each for ordinary, profile-enabled, and generated profile-disabled code, tiny current/frozen profiling passes with matching hashes and `sanity_ok=1`, a tiny CFL CSV check, and all 15 reduced correctness cases. Those checks establish tool functionality, not new algorithm speedups. No long performance run was needed to prepare this handoff.
