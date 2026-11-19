# CFPQ RSM patch experiment — source size 1

Analysis: 8 November 2026. Experiment: 7 November 2026, 21:55:51–22:32:06 Europe/Moscow.

**None of the four reviewed patches is a general performance improvement for the measured one-source workload mix.** P1 has useful Java gains concentrated in expensive chunks, but severe RDF/C-alias regressions and higher process memory. P2 and P4 regress overall. P3 remains close to baseline, with a consistent regression on `block`. All measured candidate outputs agree with baseline.

This is a completed full-recipe run for **source size 1 only**. Sizes 10 and 100 are unmeasured; the cumulative stack was not tested. These results do not establish behavior for larger source batches or combined patches.

**Evidence and method.** The run is [2026-11-07_21-55-51-397444_compare-full-s1](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1). Its completion receipt passes the existing verifier, including source/tool/artifact hashes and raw-result completeness. The entire current input/binary/runtime lock also verifies. Independent analysis checked all **6,000 raw measured rows**, 25 successful child passes, 240 expected graph/grammar/source/chunk cases per pass, five rounds per variant, positive finite times, nonzero result hashes and **4,800 candidate/baseline count-and-hash comparisons**. Baseline results are stable across rounds. All 1,200 summary rows match recomputed chunk medians and ratios. Count/hash agreement is evidence for these inputs, not a general correctness proof.

The eight workloads are Java points-to on avrora/batik, nested-parentheses RDF queries on eclass/go/go_hierarchy, and C-alias on mm/init/block. Each uses 30 source chunks, one source per chunk, seed 2026, five measured rounds, and an unmeasured warm-up per chunk in each variant’s first pass. Logs confirm `--hot` appears only in round 0 of each variant. Supplied source pools are pinned and reachability-biased; this is not a uniform sample of arbitrary graph vertices.

The machine is an AMD Ryzen 5 7640HS with CPU affinity 0–11. The run recorded an empty OpenMP environment and inherited runtime threading; the lab neither sets nor clears OpenMP variables. Actual active threads per operation were not recorded. An earlier initialization probe reported a GraphBLAS cap of 12, which does not establish that every timed operation used 12 threads.

Timing includes the selected kernel’s local setup, iteration, cleanup and final result materialization. Parsing, adapter preparation, result hashing, builds and diagnostic output are outside that timer. All candidates are standalone changes from the same frozen baseline; the experiment does not measure their combination.

**How to read the statistics.** For each case, take the median of its five measured times, then divide candidate median by baseline median. A ratio below 1 is faster. The geometric ratio averages these 240 paired ratios in log space; each workload has 30 cases and therefore equal weight. “Timed total” instead sums all 1,200 measured calls per variant, giving costly cases more weight. These answer different questions. Child wall time includes work outside the kernel timer, warm-ups and process overhead; it is not a kernel speedup statistic.

| Variant | Geometric time ratio | Faster chunks / 240 | Timed total, s | Timed-total ratio | Child wall total, s | Largest process peak, MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | 1.000 | — | 30.722 | 1.000 | 396 | 341.1 |
| p1_review | 22.333 | 42 | 199.567 | 6.496 | 609 | 848.6 |
| p2_review | 1.205 | 22 | 33.280 | 1.083 | 377 | 347.7 |
| p3_review | 1.018 | 108 | 30.966 | 1.008 | 378 | 340.4 |
| p4_review | 1.146 | 13 | 32.403 | 1.055 | 378 | 347.5 |

P1 is **22.33× slower** by the equally weighted geometric ratio and requires **6.50×** the timed work in this exact mix. P2 adds 20.5% to the geometric timing statistic, P3 adds 1.8%, and P4 adds 14.6%. Their timed-total increases are smaller: approximately 8.3%, 0.8% and 5.5%, respectively. No single one of these summary statistics is a universal speedup.

The full command lasted 36 minutes 15 seconds. Summed measured kernel time across all five variants is about 327 seconds. The remainder includes preparation, warm-ups, compilation/JIT, process and I/O overhead; these phases were not separately timed here. The child wall totals differ from kernel totals and also exclude gaps between child passes. This run therefore does not attribute end-to-end bottlenecks or justify optimizing only the kernel.

**Per-workload paired timing ratios.** Each cell is the geometric mean of 30 chunk-median ratios; below 1 is faster.

| Workload | P1 grouped transfers | P2 trimming | P3 local routing | P4 fusion |
| --- | ---: | ---: | ---: | ---: |
| avrora | 0.592 | 1.304 | 0.995 | 1.158 |
| batik | 0.661 | 1.323 | 0.977 | 1.146 |
| eclass | 52.069 | 1.022 | 1.001 | 1.114 |
| go | 9.790 | 1.012 | 0.996 | 1.002 |
| go_hierarchy | 35.338 | 1.086 | 0.967 | 1.084 |
| mm | 199.630 | 1.297 | 1.051 | 1.203 |
| init | 157.505 | 1.261 | 1.032 | 1.174 |
| block | 279.178 | 1.404 | 1.132 | 1.314 |

![Per-workload time ratios](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/workload_ratios.png)

Absolute times below are milliseconds: median across 30 chunk medians, with each chunk median computed from five rounds. Dividing these displayed aggregate medians need not reproduce the paired geometric ratios above.

| Workload | Baseline ms | P1 ms | P2 ms | P3 ms | P4 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| avrora | 25.783 | 24.309 | 36.648 | 25.482 | 31.053 |
| batik | 33.564 | 35.162 | 48.100 | 32.215 | 40.068 |
| eclass | 0.657 | 39.304 | 0.728 | 0.660 | 0.741 |
| go | 7.743 | 78.394 | 7.815 | 7.695 | 7.772 |
| go_hierarchy | 0.626 | 24.992 | 0.659 | 0.589 | 0.667 |
| mm | 1.328 | 306.069 | 1.740 | 1.386 | 1.565 |
| init | 1.658 | 296.220 | 2.088 | 1.685 | 2.009 |
| block | 1.501 | 462.169 | 2.164 | 1.682 | 2.035 |

**P1: useful Java mechanism, unsuitable universal path.** On avrora, P1 wins 29/30 chunks and its geometric ratio is 0.592, equivalent to a 1.69× geometric speedup. On Batik, it wins only 13/30 chunks, but its geometric ratio is 0.661, equivalent to 1.51×. The Batik median paired ratio is **1.041**, and its median chunk time rises from 33.564 to 35.162 ms. Thus “P1 improves Java” needs qualification: Batik’s expensive chunks improve greatly while most individual chunks do not.

Avrora chunk 11 falls from 1,005.745 to 48.284 ms (**20.83× faster**); Batik chunk 0 falls from 1,389.819 to 59.750 ms (**23.26× faster**). Both return a single reachable vertex, illustrating that output cardinality alone does not predict internal work. Across all measured Java calls, total time falls from about 28.46 to 10.47 seconds. The scatter plot shows where that reduction comes from.

![P1 Java chunk medians](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/p1_java_chunks.png)

Every tested non-Java chunk regresses under P1: **180/180** cases. Workload geometric slowdowns are 9.79–52.07× on RDF and 157.50–279.18× on C-alias. The worst individual case is `block`, chunk 26, at **561.25×** baseline. On `block`, the median chunk grows from 1.501 to 462.169 ms. The extra non-Java cost overwhelms the Java savings.

The code builds compact support maps inside every kernel invocation, extracts graph tuples and performs gather/scatter work. That design is consistent with substantial per-call cost on short searches, but this timing run does not separate setup from iteration or prove which operation dominates. The hypothesis to test is that rebuilding/grouping and scatter costs are insufficiently amortized. Workload profiles are required before assigning a percentage to any cause. Retain this as a research candidate; do not promote it as the general default on this evidence.

**P2: compaction does not repay its cost at S=1.** All eight workload geometric ratios exceed 1; only 22/240 individual chunks improve. Java slows by about 30–32% and C-alias by about 26–40%. RDF differences range from about 1–9%, with the smallest changes requiring caution. The optimization still builds a conservative control graph even when its no-remapping fast path avoids reconstruction. This makes setup overhead a plausible explanation for the short-call regressions, but no trimming counts or workload setup profiles were measured in this run. Larger-batch amortization remains unknown.

**P3: close to neutral, with a clear workload tradeoff.** Its geometric ratio is 1.018 and 108/240 chunks improve. Avrora, Batik, go and go_hierarchy improve by roughly 0.4–3.3% in the geometric statistic. C-alias regresses: mm by 5.1%, init by 3.2%, and block by 13.2%; block loses on all 30 chunks. The small overall differences do not establish a general advantage. Logical waiting-matrix dimensions can be smaller without proportional savings in allocated memory or runtime.

**P4: no measured general benefit from fusion.** Its geometric ratio is 1.146; only 13/240 chunks improve. The go result is effectively flat (+0.16% in the geometric statistic), while the other workload ratios are about 8–31% slower. This standalone variant retains the full global state dimensions and separate continuation products. Avoiding some intermediate frontier scheduling does not, on these measurements, compensate for the extra continuation work. This is not evidence of state-dimension halving, nor a test of fusion combined with grouped transfers.

**Process memory.** Each cell below is the maximum reported process peak among a workload’s 150 measured samples, in MiB (KiB divided by 1024). These are CFG_bench process peaks after the harness reset, including graph/grammar/adapter state, caches and allocator behavior. They are not isolated kernel workspace, C-buffer attribution, memory allocated over time or a proof of memory complexity.

| Workload | Baseline MiB | P1 MiB | P2 MiB | P3 MiB | P4 MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| avrora | 74.4 | 78.3 | 75.1 | 74.5 | 75.2 |
| batik | 141.2 | 149.5 | 143.0 | 141.2 | 142.8 |
| eclass | 100.8 | 155.7 | 107.4 | 100.1 | 107.6 |
| go | 190.9 | 287.5 | 197.8 | 191.0 | 198.4 |
| go_hierarchy | 103.2 | 156.1 | 109.9 | 102.7 | 110.0 |
| mm | 269.8 | 530.7 | 277.3 | 269.3 | 277.6 |
| init | 262.5 | 535.8 | 270.0 | 262.2 | 270.3 |
| block | 341.1 | 848.6 | 347.7 | 340.4 | 347.5 |

P1 raises observed peak memory on every workload, most sharply on C-alias: block rises from **341.1 to 848.6 MiB**, about **2.49×**. P2/P4 also raise every workload maximum in this run. P3 is almost unchanged: its largest observed peak is 340.4 MiB versus baseline’s 341.1 MiB, a difference too small to support a substantial workspace-saving claim. Smaller sparse dimensions should not be presented as measured peak-memory reductions.

**Variability and limits.** Excluding round 0 and recomputing medians from rounds 1–4 leaves the overall conclusion unchanged. Geometric time ratios become:

| Variant | All five rounds | Rounds 1–4 only |
| --- | ---: | ---: |
| p1_review | 22.333 | 22.120 |
| p2_review | 1.205 | 1.201 |
| p3_review | 1.018 | 1.015 |
| p4_review | 1.146 | 1.142 |

Across workloads, median within-chunk relative IQRs (inclusive quartiles, IQR divided by the five-round median) range from 1.4–10.7% for baseline, 2.2–7.5% for P1, 0.5–5.6% for P2, 1.1–5.8% for P3, and 1.1–4.5% for P4. These spreads are descriptive, not confidence intervals. P1’s non-Java regressions are far larger than this variation. The small P3/RDF effects warrant more caution.

Variants ran in contiguous blocks in the order baseline → P1 → P2 → P3 → P4, with no closing duplicate baseline or randomized crossover. A shared run-local JIT cache and elapsed machine conditions can affect comparisons. Five repetitions of the same source sets do not measure cross-session reproducibility or eliminate between-block drift. This single experiment cannot isolate the effect of OpenMP scaling; no per-operation thread counts or thread-scaling sweep were collected.

There are **no full-workload profiling receipts** for this campaign. The existing tiny profiles establish instrumentation/result agreement, not the phase bottlenecks of these eight workloads. Candidate-union and waiting-prefix-overlap counters are unavailable in active profiling; legacy zeroes must not be treated as measurements. No source-size-10/100 run, new combined stack, or CFL comparator is part of this result.

**Current workspace status.** The saved full-recipe receipt is PASS for source size 1; campaign performance coverage is PARTIAL because sizes 10/100 are NOT_RUN. The overall campaign status is NEEDS_ATTENTION for a separate reason: the current production source matches frozen `p1_review` (`1c5ad06ef855…`), while the agreed control is `baseline` (`c7efb94f21bb…`). The saved run explicitly loaded hash-verified frozen `.so` files for each variant, so the present production-copy mismatch does not invalidate its measurements. Analysis left that file and all original experiment artifacts unchanged.

**Next research steps.** Keep the frozen baseline as the performance control. Prioritize profiling P1 on expensive Java cases and on the regressing C-alias paths, separating `group_setup_s`, `grouped_s`, gather/scatter and the remaining phases. Investigate safe reuse of immutable support construction across repeated source batches and a structural cost-based choice between grouping and the ordinary path; neither change is implemented or proven by this report. P2/P4 need evidence that setup or continuation cost is amortized before promotion. P3 deserves a controlled repeat focused on its block regression and small Java/RDF gains. Use a closing baseline or rotated/crossover order to assess drift. Source sizes 10/100 and any recomposed stack remain separate future experiments.

A larger diagnostic command can be inspected without executing it:

```bash
cd /home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab
make profile VARIANT=p1_review RECIPE=profile_workloads SIZES=1 DRY_RUN=1
```

**Evidence and reproducibility.** Original run artifacts are retained; derived tables and figures are under the run’s analysis directory. They are not added to the original completion receipt or used to rewrite it.

- [Completion receipt](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/command.result.json)
- [Original chunk summary](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/summary.csv)
- [Original execution statuses](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/execution_status.tsv)
- [Provenance and exact command](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/provenance.txt)
- [Pinned input/binary/runtime manifest](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/pinned-inputs.json)
- [Raw measured results](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/raw)
- [Analysis audit and full numerical metrics](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/metrics.json)
- [Per-workload statistics](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/workloads.csv)
- [Aggregate statistics](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/overall.csv)
- [Per-chunk statistics](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/chunks.csv)
- [Per-round timed totals](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/round_totals.csv)
- [Analysis source](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/analyze_results.py)
- [Report rendering source](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/write_report.py)
- [Editable ratio figure (SVG)](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/workload_ratios.svg)
- [Editable Java scatter (SVG)](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/p1_java_chunks.svg)

Exact source SHA-256 identities:

| Variant | SHA-256 |
| --- | --- |
| baseline | `c7efb94f21bbbddc220ab8371d01cd4c852e3ab110d5942367932676f8916928` |
| p1_review | `1c5ad06ef8558104c9c6f66a90e63aea50f7e187aedaa3a47630b3650a44da3f` |
| p2_review | `dc0f7c50405bd2cb1ee3d2b9aa7407eecd88bbdcf4318a9e81b05e2bc85e9270` |
| p3_review | `35109ff0c008ba62b095c1500a7df6d1111e505be8dd636dec2e89831a0857b0` |
| p4_review | `7eefac364098f9f1d6ae353962d4d9baf7be9ed1d96a30160e4698871a671823` |

Input manifest SHA-256: `9738c30c725c0bfbb0738a571195073574590cd7cf908e218b3fd2c54629d7b1`.
Original summary SHA-256: `48132fee24f599b62654bbf7bfcf3084f0eb4b2fbb115021b5a599ae15d31e26`.
Completion receipt SHA-256: `a7f41776e7f3beeeb5a70f8ad333eb47e73503b760c47c481585b69b19b9d827`.

To recompute the derived metrics/figures and render this report from retained evidence:

```bash
python3 /home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/analyze_results.py
python3 /home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-07_21-55-51-397444_compare-full-s1/analysis/2026-11-08/write_report.py
```

These commands analyze saved files only. No benchmark, kernel edit, production restoration, OpenMP override or new workload profile was performed for this report. The run directory is Git-ignored; preserve it with the report so its evidence links and derived figures remain available.
