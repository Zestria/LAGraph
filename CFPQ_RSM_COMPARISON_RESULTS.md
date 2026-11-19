# CFPQ RSM comparison — 4 November 2026

Current RSM improves the workload-level timing statistic on all eight tested graph/grammar pairs against commit `bb7dc96e66a99ca7628374c177f04d5dab36bd92`. Speedups range from **1.30× to 7.53×**, with an equal-workload geometric mean of **2.45×**. All 1,200 measured current-RSM outputs match the historical baseline by cardinality and nonzero reachable-set hash.

Against fully optimized `CFL_multsrc_adv`, current RSM is faster on the three RDF and three C-alias workloads. CFL is faster on both Java workloads. CFL was configured as a **timing-only comparator**: its result counts/hashes were not validated, so its `MATCH` labels establish timing completeness, not result equivalence.

## Experiment and verification

- Run: 4 November 2026, 00:17–02:42 Europe/Moscow; AMD Ryzen 5 7640HS, 12 logical CPUs, unrestricted runtime threading.
- Eight workloads: the seven original lab pairs plus Batik/Java points-to; 100 sources per chunk, 30 chunks, seed 2026.
- One baseline series, then current RSM, then CFL; five measured rounds and one unmeasured warm-up per algorithm/workload/chunk. The logs confirm `--hot` only in round 0 for each display.
- CFL used `-e -f -l -b`: EMPTY, FORMAT, LAZY and BLOCK, mask 15. Its commands have no RSM replacement library.
- All 15 passes succeeded; runner and summarizer both exited 0. Independently checked all 3,600 raw measured rows: exactly 240 expected cases per pass, five rounds per algorithm, positive finite timings, and correct algorithm labels. All 720 runner summary rows are present.
- Independently checked all 1,200 current/baseline count/hash comparisons and baseline stability across rounds. This establishes agreement on these inputs, not a general correctness proof.
- Snapshot hashes match run provenance: baseline `2eb47b26cfb729710424c5aee0dbad1fafb870d2cfefec450f9fc0c7bd8efae8`; current `c7efb94f21bbbddc220ab8371d01cd4c852e3ab110d5942367932676f8916928`.

## Runtime

Times below are milliseconds: median across the 30 chunk medians, each chunk median taken over five measured rounds. Speedups use the saved paired statistic: the reciprocal of the median of current/reference chunk ratios. Consequently, dividing the displayed aggregate times need not reproduce a displayed speedup.

| Workload | Baseline ms | Current RSM ms | CFL ms | RSM speedup vs baseline | CFL ms / RSM ms, paired |
| --- | ---: | ---: | ---: | ---: | ---: |
| avrora | 14,770.598 | 4,065.996 | 1,897.760 | 3.72× | 0.46× |
| eclass | 8.906 | 2.309 | 15.520 | 3.79× | 6.65× |
| go | 168.490 | 22.318 | 92.037 | 7.53× | 4.26× |
| go_hierarchy | 23.899 | 18.850 | 38.014 | 1.30× | 2.21× |
| mm | 32.935 | 23.700 | 617.608 | 1.39× | 27.57× |
| init | 27.577 | 25.158 | 449.412 | 1.37× | 20.34× |
| block | 29.972 | 23.971 | 646.095 | 1.37× | 28.17× |
| batik | 14,518.183 | 4,230.165 | 1,750.147 | 3.59× | 0.41× |

The final column exceeds 1 when current RSM is faster than CFL. Workload grammars are Java points-to for avrora/batik, nested parentheses over subClassOf/type for the three RDF graphs, and C-alias for mm/init/block.

**Java:** current RSM is 3.72× faster on avrora and 3.59× faster on Batik than the historical baseline, cutting the paired timing statistic by about 73% and 72%. CFL still leads current RSM by **2.19×** and **2.44×**, respectively. These are workload medians: RSM beats CFL on 4/30 avrora chunks and 3/30 Batik chunks, so a universal per-chunk winner would overstate the evidence.

**RDF:** current RSM has the strongest baseline gain on go (**7.53×**), followed by eclass (**3.79×**) and go_hierarchy (**1.30×**). It beats CFL by **4.26×**, **6.65×** and **2.21×**, respectively. Every one of the 30 chunk medians on each RDF workload favors current RSM over both references.

**C-alias:** workload-level speedups against baseline are modest, **1.37–1.39×**, while the gap to CFL is large: current RSM is **20.34–28.17× faster**. All 30 mm chunk medians improve against baseline, but init improves on only 21/30 and block on 23/30. The remaining 16 chunks regress; the worst slowdown is about **31%**. Thus all workload summaries improve, but not all source chunks.

**Actual measured workload mix:** summing the recorded timed adapter calls gives 4,417.98 s for baseline, 1,213.17 s for current RSM and 879.01 s for CFL, excluding warm-ups and setup outside the timer. Current reduces this sum by **72.5%** against baseline, yet takes **38.0% more measured time than CFL**. Java accounts for 98.2% of current-RSM measured time, so those two workloads outweigh its six other wins. These sums describe this exact workload mix; the 2.45× geometric mean weights the eight workloads equally. Neither statistic is a universal speedup.

## Peak process memory

Each value is the maximum reported process peak over the workload’s 150 measured samples, converted from KiB to MiB. This includes CFG_bench and retained graph/grammar/adapter state; it is not isolated algorithm workspace.

| Workload | Baseline MiB | Current RSM MiB | CFL MiB | Current reduction vs baseline |
| --- | ---: | ---: | ---: | ---: |
| avrora | 76.4 | 75.0 | 22.9 | 1.8% |
| eclass | 123.6 | 118.6 | 103.1 | 4.1% |
| go | 210.4 | 195.8 | 246.6 | 6.9% |
| go_hierarchy | 165.2 | 154.6 | 96.2 | 6.4% |
| mm | 279.7 | 270.4 | 466.1 | 3.3% |
| init | 272.7 | 264.7 | 447.8 | 2.9% |
| block | 350.1 | 341.8 | 620.2 | 2.4% |
| batik | 145.3 | 142.6 | 32.7 | 1.8% |

Current RSM lowers the observed peak on every workload, but the reduction is modest (**1.8–6.9%**), unlike some of the timing gains. CFL uses substantially less process memory on Java (22.9/32.7 MiB versus 75.0/142.6 MiB) and on eclass/go_hierarchy. Current RSM uses less on go and all three C-alias graphs. There is no single memory winner across workload families.

## Variability and interpretation

The median within-chunk relative interquartile range (IQR divided by the chunk median, inclusive quartiles) is **1.6–8.1% for current RSM**, versus **0.9–3.8% for baseline**. CFL’s C-alias timings are noisier, with median relative IQRs of **25.1–27.7%**. The multi-fold Java/RDF baseline gains and the large C-alias gap to CFL are much larger than these typical within-chunk spreads. The smaller baseline gains and individual regressions warrant more qualified claims.

All algorithms ran in separate contiguous time blocks, with a single baseline series and a shared run-local JIT cache. There is no closing baseline or randomized cross-algorithm order, so this run cannot quantify between-series machine drift or isolate cache-history effects. Five repeated measurements per case support the reported medians; they do not establish cross-session confidence intervals.

This comparison measures the complete current implementation against the historical implementation. The source diff includes changed products, shared shifted frontiers, terminal activity filtering, return bookkeeping, direct call tuple construction and accumulator handling. The experiment cannot attribute a speedup to a particular optimization or to the final redundant-check refactor; that would require separate ablations or profiling.

## Implications

- Retain current RSM as the improved RSM implementation: all eight workload summaries improve, all observed process peaks decrease, and all measured results agree with baseline.
- For this measured workload family, current RSM has a clear timing advantage on RDF and C-alias. Fully optimized CFL remains the faster Java comparator, subject to its unvalidated results in this run.
- Java is the main remaining RSM performance target because it dominates total time and remains behind CFL. If further investigation is desired, profile that gap and inspect the regressing init/block source chunks before claiming uniform source-level improvements.

## Evidence

- [Per-workload summary](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/current-vs-bb7-2026-11-04_00-17-29/morning_summary.csv)
- [Per-chunk summary](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/current-vs-bb7-2026-11-04_00-17-29/summary.csv)
- [Execution statuses](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/current-vs-bb7-2026-11-04_00-17-29/execution_status.tsv)
- [Build, source and runtime provenance](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/current-vs-bb7-2026-11-04_00-17-29/provenance.txt)
- [Exact experiment commands](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/current-vs-bb7-2026-11-04_00-17-29/commands.sh)
- [Raw measured CSVs](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/current-vs-bb7-2026-11-04_00-17-29/raw)

Analysis independently recomputed from retained CSVs and logs on 4 November 2026. No benchmarks were rerun, and experiment artifacts and algorithm sources were left unchanged.
