# Java points-to: baseline versus reviewed P1

Analysis date: 8 November 2026 (America/Chicago). Run: [2026-11-08_10-34-11_java_f4pnUW](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW).

**Reviewed P1 strongly improves the tested Java workloads at source sizes 10 and 100.** Geometric paired speedups are **12.38–16.58× at 10 sources** and **34.86–41.89× at 100 sources**, with every corresponding source chunk improving. At one source, Avrora improves on every chunk, while Batik has a mixture of large wins and small regressions. P1 increases reported process peak memory in all six workload/size combinations.

The saved measurements are complete and agree by result counts/hashes. However, this was the direct cfg_bench run: it did not retain command logs, binary/library identities or a run-time environment record. The quantitative comparison below is verified from the CSVs; exact execution provenance has the limitations described later.

**What was checked.** Both raw CSVs contain 900 measurements: two graphs × three source sizes (1, 10, 100) × 30 chunks × five samples. All 180 expected cases are present in each variant, with exactly five contiguous CSV observations per case. The order is Avrora S=1/10/100, then Batik S=1/10/100, with chunks 0–29 inside each size. All 900 P1 measurements match baseline by result count and nonzero result hash, and baseline results are stable across their five samples. All times are finite and positive. The original CSV/config hashes remained unchanged during analysis. Count/hash agreement on these cases is not a general correctness proof.

**Timing results.** Absolute times are milliseconds: first take the median of the five samples for each chunk, then the median of those 30 chunk medians. The speedup column instead takes the geometric mean of 30 paired baseline/P1 chunk-median ratios. Above 1 means P1 is faster. Dividing displayed aggregate times need not reproduce that speedup.

| Graph | Sources per chunk | Baseline median ms | P1 median ms | Geometric paired speedup | Faster chunks |
| --- | ---: | ---: | ---: | ---: | ---: |
| avrora | 1 | 25.459 | 16.481 | 2.16× | 30/30 |
| avrora | 10 | 551.077 | 27.147 | 16.58× | 30/30 |
| avrora | 100 | 3,925.953 | 117.358 | 34.86× | 30/30 |
| batik | 1 | 23.024 | 25.807 | 1.56× | 13/30 |
| batik | 10 | 414.478 | 33.583 | 12.38× | 30/30 |
| batik | 100 | 4,092.508 | 92.254 | 41.89× | 30/30 |

![Java timing comparison](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/java_results.png)

Whiskers in the left panel show minimum/maximum chunk medians, not confidence intervals. The right panel shows paired geometric speedups and the number of improved chunks.

**One source: distinguish expensive chunks from typical chunks.** Avrora improves on 30/30 chunks with a 2.16× geometric speedup. Batik improves on only 13/30 chunks, despite its 1.56× geometric speedup. Its displayed median rises from 23.024 to 25.807 ms (**12.1% slower**); the median paired time ratio is 1.098 (**9.8% slower**). Large improvements on a few costly cases outweigh those regressions in other statistics. Batik’s measured total drops from 14.783 to 4.078 seconds, while Avrora drops from 13.262 to 2.644 seconds. Therefore P1 is useful on this one-source Java mix, but is not a uniform improvement for one-source queries.

**Ten sources: consistent large gains.** P1 improves all 60 chunks across both graphs. Avrora’s geometric speedup is 16.58× and Batik’s is 12.38×. Even the least-improved chunks are 3.63× and 2.36× faster, respectively. These gains are not explained by a small number of exceptional chunks.

**One hundred sources: strongest and most uniform gains.** Again P1 improves all 60 chunks. Avrora’s chunk speedups range from 30.79× to 39.00×; Batik’s range from 28.78× to 48.35×. The geometric speedups are 34.86× and 41.89×. Typical kernel times move from about 3.93–4.09 seconds to 92–117 milliseconds per chunk.

Across both graphs, the equal-case geometric speedup is 1.84× at S=1, 14.32× at S=10 and 38.21× at S=100. Across all 180 cases it is **10.02×**, with **163/180** chunk medians improving. All 17 regressing cases are Batik S=1.

Summing all saved measured times gives **1,502.459 s for baseline** and **48.628 s for P1**: **30.90× less timed work**, or a 96.8% reduction in this exact workload mix. The sum weights expensive cases more heavily, whereas the geometric mean weights each case equally. It excludes preparation, parsing, any discarded warm-ups and other untimed work; it is not an end-to-end wall-time speedup. Start/end timestamps were not retained.

**Process memory.** Values are the maximum reported process peak among the 150 measurements in each graph/size group, converted from KiB to MiB. They include CFG_bench, graph/grammar/adapter state and runtime/allocator/cache effects, not just kernel workspace.

| Graph | Sources | Baseline peak MiB | P1 peak MiB | Increase MiB | Increase % |
| --- | ---: | ---: | ---: | ---: | ---: |
| avrora | 1 | 68.62 | 78.29 | 9.66 | 14.1% |
| avrora | 10 | 119.97 | 130.03 | 10.06 | 8.4% |
| avrora | 100 | 171.29 | 181.89 | 10.61 | 6.2% |
| batik | 1 | 233.83 | 250.38 | 16.55 | 7.1% |
| batik | 10 | 293.54 | 310.66 | 17.12 | 5.8% |
| batik | 100 | 354.42 | 371.46 | 17.04 | 4.8% |

P1 adds about **9.7–10.6 MiB on Avrora** and **16.6–17.1 MiB on Batik** in these process-peak observations. The largest observed peak is 371.46 MiB for P1 versus 354.42 MiB for baseline. Thus this experiment supports a substantial speed improvement at larger batches, accompanied by a modest process-memory increase; it does not demonstrate a memory reduction. Because source sizes and graphs execute sequentially in the same variant process, retained allocator/cache state can influence later groups. Treat these as observed process peaks, not independent per-query workspace measurements.

**Repeatability within the run.** The median relative IQR within each five-sample chunk is 0.34–0.62% for baseline across the six groups, and 0.46–0.94% for P1. Dropping the first saved sample and recomputing from the remaining four changes each workload geometric speedup by less than 0.3%; all conclusions remain the same. These are descriptive stability checks, not confidence intervals or proof that allocator/environmental effects disappeared. The intended harness still calls malloc_trim before measurements.

**What this suggests about P1.** Grouping removes repeated per-terminal work across Java’s large indexed families. The stronger improvement with larger source batches is consistent with that reduction and better amortization of per-call support construction. This is a source-based interpretation, not a measured phase attribution: no profiling was run, and the CSVs cannot separate setup, multiplication, gather or scatter costs. Different batch sizes also select different sets/combinations of sources, so the trend is not a complexity proof.

The results justify continuing P1 for Java-like indexed families, particularly at S=10/100. They do not establish a grammar-size cutoff or a safe universal activation policy. The mixed Batik S=1 outcome favors a structural/cost-based selection strategy over a grammar-name switch. RDF/C-alias and other Java graphs were not tested here; this result does not supersede the earlier non-Java regressions.

**Provenance limits.** The intended command from the conversation used `--experiment --source-sizes 1,10,100 --chunks 30 --seed 2026 --hot -r 5`, with baseline followed by reviewed P1. The CSV sizes, chunk identities and contiguous five-sample layout agree with that protocol. However, the original run directory saved only `java.csv`, `baseline.csv`, `p1_review.csv` and a JIT cache. It has no settings/command/environment log, completion status, input manifest or library hashes. In particular:

- Discarded warm-ups cannot be verified from measured CSVs alone; `--hot` is part of the intended command, not an independently audited fact for this run.
- The labels baseline/P1 reviewed follow the supplied filenames and command context. The exact loaded shared objects and binary cannot be bound to these CSVs from retained metadata.
- OpenMP settings, affinity, actual active threads, hardware conditions and execution start/end times were not recorded. Analysis does not reconstruct them from the current environment or infer timezone from the directory name.
- There is no recorded reverse-order/closing-baseline comparison or independent session repeat. Small one-source differences remain more sensitive to between-variant drift than the multi-fold gains at larger batches.

These omissions do not create missing or mismatching samples, but they limit how strongly this run can establish reproducibility and causal attribution. Future use of run_simple.py saves the command, input identities and inherited environment alongside the same per-chunk schedule; this report does not retrofit those records onto the older direct run.

**Evidence.** Original files are unchanged. Derived artifacts are in the run’s analysis directory; the run is Git-ignored and must be preserved with this report.

- [Baseline raw measurements](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/baseline.csv)
- [P1 reviewed raw measurements](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/p1_review.csv)
- [Java-only config](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/java.csv)
- [Analysis audit and full metrics](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/metrics.json)
- [Per-graph/source-size summary](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/summary.csv)
- [Per-chunk medians and paired ratios](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/chunks.csv)
- [Equal-case summary by source size](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/by_source_size.csv)
- [Editable comparison figure (SVG)](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/java_results.svg)
- [Analysis script](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/analyze_java.py)
- [Report rendering script](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/write_report.py)

Original file SHA-256 identities, computed at analysis:

| File | SHA-256 |
| --- | --- |
| java.csv | `1e8e0dbb025667a954aed6514e92732476817046a91d04a8ab011a59bc477ab9` |
| baseline.csv | `460671aa1c3a0404554a7a1fe5537f8c6dbb8a1f74e0c5d0e4f473a34a5714a8` |
| p1_review.csv | `cd5b8fe0b08040837e86c5bfcce36be76286bf706fc963d419618c92671d0a96` |

Recompute the analysis and report without executing kernels:

```bash
python3 /home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/analyze_java.py
python3 /home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/runs/2026-11-08_10-34-11_java_f4pnUW/analysis/2026-11-08/write_report.py
```

No benchmarks, profiling, kernel edits, production restoration or OpenMP overrides were performed during analysis.
