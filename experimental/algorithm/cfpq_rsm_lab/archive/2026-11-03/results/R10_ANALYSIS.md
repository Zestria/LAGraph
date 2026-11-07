# R10 interrupted overnight analysis — 2026-11-03

Analysis of retained artifacts only. No workload or benchmark was started. The only execution probe initialized/finalized the installed libraries and queried thread metadata. Sources 100 are incomplete and excluded from performance conclusions.

## Completion and runtime

| Sources | Completed comparisons | Matching workload rows | Wall time | Measured kernel sum | Measured / wall |
| --- | --- | --- | --- | --- | --- |
| 1 | 13/13 | 91/91 (2730 chunk comparisons) | 2.926 h | 14.92 min | 8.5% |
| 10 | 13/13 | 91/91 (2730 chunk comparisons) | 5.941 h | 166.06 min | 46.6% |

All 16,380 summary rows including both baseline displays pass count/hash checks. Wall time includes warm-ups, parsing, preparation, hashing, cleanup, process starts, provenance and builds; this analysis does not time those individually. The difference cannot all be assigned to preparation. Recorded kernel time is adapter.run(), including query-local setup/cleanup.

The harness calls prepare for every chunk (CFG_bench/src/test.c:321), and the RSM adapter copies/transforms the parsed graph/grammar and rebuilds adjacency matrices there (adapter_CFL_CFPQ_RSM.c:129–185). Its edge classification loops are ordinary serial C. Each comparison repeats this for 7 workloads x 30 chunks x 5 processes/rounds x 3 baseline/candidate displays = 3150 preparations; the completed 26 comparisons therefore performed 81,900 preparations. Warm-up shares that chunk preparation. Separate comparisons repeat the two baseline displays for every candidate.

All recorded measured kernel time across sources 1/10 totals 3.02 h, while total wall time is 8.87 h. Java/avrora accounts for 93.9% of recorded kernel time.

## Threads

The recorded runner uses --threads 0 and unsets OMP_NUM_THREADS/OMP_THREAD_LIMIT. It starts comparisons and chunks sequentially. The exact installed /usr/local/lib/liblagraph.so.1 and libgraphblas.so.10 are OpenMP-enabled; an initialization-only probe with those variables unset returns OpenMP max=12, LAGraph outer=1/inner=12, and actual GraphBLAS global thread limit=12. Outer=1 does not mean GraphBLAS is capped at one. No operation-level thread counts were recorded overnight, so the precise htop observation cannot be reconstructed.

SuiteSparse GB_nthreads chooses floor(work/chunk) threads clamped between 1 and the maximum; default chunk is 65536 for kernels using that control. Small operations choose one. RSM label loops and tuple-routing C loops are serial. Seeing one busy core is therefore compatible with a cap of 12. Large matrix dimensions alone do not establish large work. Serial preparation is a strong explanation for much of the observed one-core wall time; tiny kernel work can explain the rest, but this is an inference rather than a measured phase breakdown.

## Candidate results

Ratios below are medians of the seven workload-level median per-chunk candidate/baseline ratios (lower is better); this equal-workload summary is not a total-runtime speedup. Full per-workload ratios, duplicate-baseline controls, timings and process memory are in analysis_s1_s10_workloads.csv.

| Candidate | Sources 1 median ratio | Sources 10 median ratio | Java, sources 10 | C-alias median, sources 10 |
| --- | --- | --- | --- | --- |
| ds36_pair | 0.993 | 0.983 | 0.999 | 0.987 |
| ds37_fused | 1.003 | 1.074 | 1.157 | 1.081 |
| ds38_no_product_clear | 0.997 | 0.996 | 0.988 | 1.002 |
| ds39_direct_calls | 0.968 | 0.984 | 0.989 | 0.937 |
| ds30_assign | 1.002 | 1.059 | 1.003 | 1.060 |
| ds31_joint | 37.709 | 14.876 | 0.037 | 42.896 |
| ds32_empty | 0.928 | 0.929 | 0.731 | 0.929 |
| ds33_format | 1.000 | 1.002 | 1.270 | 1.000 |
| ds34_lazy_p | 0.987 | 1.029 | 1.004 | 1.058 |
| ds35_lazy_nt | 0.995 | 1.013 | 1.001 | 1.152 |
| d1_native | 0.968 | 0.988 | 0.999 | 0.960 |
| config_block | 0.655 | 0.667 | 0.631 | 0.659 |
| d3_masked_product | 0.992 | 1.015 | 1.010 | 1.018 |

Interpretation:

- config_block is the strongest broad candidate: source-10 ratios 0.631 Java, 0.670 eclass, 0.920 go, 0.750 go_hierarchy and 0.658–0.667 C-alias. It removes reshapes and replaces shifts/final extraction by tuple routing.
- ds32_empty consistently helps: Java ratios 0.802/0.731 at 1/10 sources, C-alias about 0.92–0.93. Empty/first contribution handling has value here; fused accumulation is not automatically better.
- ds31_joint is strongly workload-specific: Java 0.528/0.037 at 1/10 sources, but source-10 C-alias 38.5–61.9x slower. Its graph-support indexing is query-local and scans/sorts large graph supports, which overwhelms millisecond queries. No per-phase timer attributes the exact split.
- ds39_direct_calls and d1_native give modest C-alias improvements; D3 adds no convincing gain over D1. Comparing the separately baseline-normalized same-chunk ratios, D3/D1 at 10 sources is 1.012 Java and 1.057–1.078 C-alias. This is an indirect comparison, not an interleaved direct-control run.
- ds36_pair, ds38_no_product_clear and lazy P/summary variants show mostly small changes or regressions; lazy P/summary worsen C-alias at 10 sources by roughly 6%/15%. ds30_assign loses roughly 5–8% outside Java/go at 10 sources; ds37_fused and ds33_format regress Java by about 16%/27%.

## Next work, without running sources 100

First improve the harness: prepare immutable graph/RSM data once per workload/process, switch only source lists per chunk, and record the actual GraphBLAS thread limit after adapter setup. Preserve kernel timer boundaries, true-only semantics, case lists, counts/hashes, and baseline rotation. Cache no mutable kernel query state between runs. Keep the 30 source chunks if desired; preparation does not need repeating for them.

Also consider one run.py comparison per source count containing baseline,baseline plus all 13 variants. This preserves duplicate baselines but avoids 26 repeated baseline displays per source count. At unchanged rounds/chunks/workloads, measured invocations fall from 13*3 to 15 display groups, a 61.5% reduction; this is a work-count saving, not a promised wall-time or parallel speedup. The existing per-candidate summary driver would need updating to read that combined run. No runner/algorithm edit is made by this analysis.

Do not force every small GraphBLAS operation to use 12 threads as a presumed fix. The current cap is already 12 in the probe. Thread thresholds can be profiled separately later if a demonstrated kernel bottleneck warrants it. Keep config_block and ds32_empty as promising independent controls; treat joint filtering as Java-specific. Do not combine changes before measuring combinations.
