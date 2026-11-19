# CFPQ RSM optimization decisions and measured effects

Report date: 4 November 2026. This report describes the optimizations actually retained in `experimental/algorithm/LAGraph_CFPQ_RSM.c`, their available measurements, and experiments or directions not selected for that implementation. It uses existing results only; no new benchmark was run.

The final implementation is **1.30–7.53× faster** than historical commit `bb7dc96e66a99ca7628374c177f04d5dab36bd92` across the eight tested workload summaries, with an equal-workload geometric mean speedup of **2.45×**. That is a combined result, not the measured contribution of any one change.

The strongest later isolated improvement was **empty-product/first-contribution handling**: about **27.5% less time on Java/avrora**. Direct call construction helped C-alias by roughly **5%**. ANY_PAIR gave smaller improvements, and removing one redundant clear was essentially neutral. Older structural experiments identify terminal filtering as an important earlier improvement.

## How to read the evidence

Three experiment generations must be kept separate:

1. **Historical I/S screens:** earlier sources and harness. The `stage1` screen has six RDF/C-alias workloads, 100 sources, one chunk per workload and 20 recorded timings per case; it contains no Java workload. Other stages use different source sizes and repetition counts. Their reported statuses and matched source/control timings are historical evidence, not a new correctness acceptance under the later harness.
2. **Corrected-reference ablations:** the later DS/D variants use the frozen corrected lab baseline, SHA-256 `f3642839be04eee2d6411d3db9d6973c50c19f386d7e61f4d9ff5a926af0447b`. It already contains earlier structural optimizations. Completed selected-candidate and combined runs at 100 sources cover seven workloads, 30 chunks, five measured rounds, one warm-up, and opening/closing baselines. Batik is absent from these ablations.
3. **Final historical comparison:** current source SHA-256 `c7efb94f21bbbddc220ab8371d01cd4c852e3ab110d5942367932676f8916928` versus the exact `bb7dc96` source, with eight workloads including Batik. It measures the entire implementation change, with one baseline series.

For later runs, a workload time ratio is the median across chunks of candidate five-round median / control five-round median. A ratio below 1 means faster; speedup below is its reciprocal. Historical table values use the geometric mean of matched case-median candidate/control ratios, then take its reciprocal. **Do not add percentages or multiply the factors across these tables to reconstruct the final speedup.** The baselines, workloads and interactions differ.

## Retained structural optimizations

`Q` is the number of RSM states; `V` is the number of graph vertices. The current implementation keeps frontier/visited/call matrices as `Q × V²`, while graph-product scratch, accumulated products and return prefixes use `QV × V`.

| Retained change | What the code does | Available measured effect and control |
| --- | --- | --- |
| Reuse shifted nonterminal frontier, S1a | Compute `X = reshape(R_iᵀ M)` once; reuse it for calls, summary-edge propagation and return-prefix bookkeeping. | Historical stage1: **1.053×** versus its baseline across six cases. |
| Incremental return prefix, S2 | Maintain `W[i] = reshape(R_iᵀ P)` by unioning new shifted frontier entries; avoid recomputing the full prefix when new summaries arrive. | Stage1: **1.030×** relative to S1a. Stage2a, seven 10-source cases: **1.024×** when added to S1a + active-row filtering. |
| One accumulator, S3a | Combine terminal, nonterminal and return products in one accumulator instead of separate phase-result matrices. | Stage1: **1.013×** relative to S1a + S2. |
| Keep accumulation in shifted layout, S5 | Keep `acc` as `QV × V` throughout the iteration; reshape/copy once for Phase 5 instead of reshaping graph-product outputs repeatedly. | Stage3a, seven 10-source cases: the S3b + S5 layout/Phase-5 replacement bundle was **1.031×** faster than the preceding S3c-based combination. This does **not** isolate S5 alone. |
| Reshape handling, I1 / S3b | Use in-place scratch reshapes; construct `M_new` with `reshapeDup` when a new frontier handle is required. | Stage1: I1 **1.014×** versus baseline; S3b **1.008×** relative to S1a + S2 + S3a. These are different changes/controls. |
| Merge and visited subtraction together, S3c idea | Phase 5 unions the accumulated contribution with calls under complemented structural `P`, removing visited configurations in that merge. | Stage1: **1.036×** relative to S1a + S2 + S3a. The current code combines this operation with the shifted-accumulator reshape. |
| Static terminal filtering, I3 | Precompute which terminal transitions and graph relations are present/nonempty; skip labels that cannot contribute to any iteration. | Stage1 standalone: **1.470×** geometric speedup; time ratios 0.517 on eclass and 0.204 on go, but about 0.997–0.998 on C-alias. On the later stage4f optimized combination, the incremental effect was only **1.027×** at 10 sources. |
| Frontier active-row filtering, S4 | Reduce `M` to active RSM rows; skip a terminal label unless one of its transition source states is active. | Stage1: **1.591×** relative to S1a; go time ratio 0.168. Stage2a, seven 10-source cases: **2.598×** relative to S1a + S2 + S3a + S3c. Benefits depend heavily on workload and existing filtering. |
| Process only fresh summary edges, I4 | Form `dG = new_edges \ graph_nt[i]`, skip an empty delta, and propagate return products from that delta. | No isolated positive speedup is established: stage1 was **0.988×** (about 1.2% more time). The delta-based return path is retained, but should not be credited with a demonstrated gain from this screen. |
| Remove frame machinery and simplify seeding, I2 / I5 / I6 | Use direct source-mask insertion and duplicate initial frontier into `P`; omit obsolete frame/mask intermediates. | Historical stage1 was essentially neutral: frame removal **0.999×**, direct source-mask seeding **0.999×**, and initial `P` duplication **1.001×** relative to direct seeding. Treat these as simplifications, not material measured wins. |

These mechanisms are visible in the current source. Git history identifies shared shifting (`e34c5163`), incremental W (`63c4bd05`), shifted accumulation (`0cf18a8f`), scratch simplification (`4c5cd1a1`) and terminal filtering (`abf4dee8`). The historical screens support the direction of some changes; they cannot provide a precise decomposition of the final eight-workload result.

## Retained later optimizations: complete 100-source ablations

The selected-candidate run completed all **42 candidate/workload comparisons**. Independent inspection found 40 successful passes and 8,400 raw measured rows with matching baseline counts/hashes, including both baseline displays. Opening/closing workload-level drift ratios range from 0.994425 to 1.001844, about −0.56% to +0.18%.

### Changes and interpretation

- **DS-32, empty-product and first-contribution handling:** `s_mul_acc` returns when its product is empty. For the first nonempty contribution it swaps matrix handles into `acc`; later contributions use LOR union. This avoids unions that contribute nothing and avoids copying the first real contribution. It is a measured win on all seven workload summaries, largest on Java/avrora: **1.38× speedup / 27.5% less time**.
- **DS-39, direct flat call construction:** replace the diagonal entry matrix, its reshape and the start-state call product with reusable tuple/index buffers. Append `(start_state, v*V+v, true)` and build `M_call` once per iteration, combining duplicates with LOR. C-alias improves **1.05–1.06×**, about **4.9–5.4% less time**; Java and RDF are close to neutral apart from a small eclass gain. Buffer reuse is part of this variant, not an independently timed extra optimization.
- **DS-36, ANY_PAIR products:** use `GxB_ANY_PAIR_BOOL` for reachability products under the accepted true-only Boolean input contract. Keep reductions, unions and masks unchanged. Workload speedups range **1.003–1.077×**; the largest is go_hierarchy, about **7.1% less time**, while Java improves only about **0.34%**. This is a modest optimization, not the source of the multi-fold final gains.
- **DS-38, remove the product scratch clear:** the following unmasked, no-accumulator mxm overwrites that temporary. The isolated experiment ranges from **0.6% more time to 0.9% less time**, effectively neutral at this scale. The clear is removed in the final source for simplicity; it should not be described as a large speedup.

The following table reports the reciprocal of the opening-baseline paired ratio. Values above 1 mean faster. The combined column comes from a separate, fully checked run using the same corrected parent.

| Workload | ANY_PAIR, DS-36 | Remove clear, DS-38 | Direct calls, DS-39 | Empty/first, DS-32 | DS-36 + DS-39 + DS-32 |
| --- | ---: | ---: | ---: | ---: | ---: |
| avrora | 1.003× | 1.008× | 1.002× | 1.378× | 1.395× |
| eclass | 1.029× | 1.004× | 1.014× | 1.070× | 1.110× |
| go | 1.018× | 1.009× | 1.002× | 1.022× | 1.039× |
| go_hierarchy | 1.077× | 0.994× | 0.998× | 1.010× | 1.092× |
| mm | 1.016× | 1.005× | 1.058× | 1.049× | 1.118× |
| init | 1.015× | 0.999× | 1.051× | 1.043× | 1.120× |
| block | 1.016× | 0.999× | 1.058× | 1.047× | 1.126× |

### Combined result and final cleanup

The measured `optimized_v1` combination contains **DS-36 + DS-39 + DS-32**, not config_block. It improves all seven workload summaries by **1.039–1.395×** over the corrected parent, with an equal-workload geometric speedup of **1.138×**. Java time falls **28.3%**, C-alias **10.6–11.2%**, and RDF **3.7–9.9%**, using the opening control. The separate combination run has 15 successful passes and 3,150 matching raw measured rows; closing/opening baseline drift is +0.37% to +1.60%. Its results are stronger evidence of the combined effect than multiplying isolated factors.

Commit `ef2ec6eb` integrates these three changes. Commit `fa6751b1` subsequently removes redundant `Q > 0` guards, the terminal scratch nonempty query guaranteed by active-row filtering, the entry-vector nonempty guard inside a nonempty-scratch path, unused tuple values, duplicate counters, redundant initialization and overwritten clears. **There is no isolated before/after timing measurement for that cleanup.** The DS-38 experiment covers just one removed product clear; it cannot quantify the rest of the refactor.

Input/shape/overflow validation and cleanup fixes are also present. They are correctness/robustness work and have no separately established performance gain.

The final R13 test against the older `bb7dc96` source measures the accumulated result of the earlier structural changes, the later combined optimization, robustness changes and cleanup. Its 2.45× geometric speedup must not be presented as the gain of DS-32, ANY_PAIR or the refactor alone. No single-change Batik measurements exist in the retained ablations.

## Optimizations not adopted: measurements and tradeoffs

“Not adopted” means absent from the current production RSM. It does not establish that a direction is useless for every graph or source size. Full 100-source results supersede preliminary claims based only on 1/10 sources.

| Experiment / direction | Measured result | Decision supported by the evidence |
| --- | --- | --- |
| **DS-20: permanent column/hypersparse return orientation** | Completed 100-source time ratios 1.012–1.630 across seven workloads; median workload ratio **1.122**. Go regresses about 63%. | Not adopted. The tested orientation scheme, including conversion costs, did not pay off. This does not reject every possible format policy. |
| **DS-21: waiting-state deduplication** | At 100 sources every workload is slightly slower; median ratio **1.034**. Earlier profile probes found zero `X ∩ W_old` on the sampled chunk/iterations. | Not adopted: the observed duplication opportunity was absent, while subtraction adds work. Avoid treating overlap with visited `P` as evidence of duplicate waiting states; these are different quantities. |
| **DS-22: earlier block visited layout** | Completed 100-source median workload ratio **1.277**; C-alias ratios **1.559–1.609**. | Not adopted. This version retained extra frontier/call reshapes, so it does not by itself settle leaner block-layout designs. |
| **D1: lean block visited/call matrices** | At 100 sources ratios are 1.003 Java, 1.182 eclass, 1.286 go, 1.027 go_hierarchy, and **1.470–1.540 C-alias**. All seven workload summaries regress. | Not adopted. Small earlier 1/10-source gains did not scale to the target 100-source regime. |
| **config_block: all configurations as QV × V, implicit RSM routing** | At 100 sources: Java **1.610× faster**, eclass 1.068× faster and go_hierarchy 1.187× faster; go **16.9% more time**, C-alias **27.4–33.3% more time**. Process peaks also increase in all seven workloads versus the opening control. | Not selected for the general current implementation. Earlier 1/10-source runs were promising, but full 100-source results show a workload tradeoff. It remains a possible Java-specific branch, not an established universal replacement. |
| **D3: complemented visited masks inside graph products** | Completed 10-source runs show no convincing gain over D1; indirectly baseline-normalized C-alias ratios are about **5–8% worse than D1**. A direct interleaved D3/D1 timing control is absent. | Not adopted. D3 must be compared to D1, its immediate parent, rather than credited with D1’s own layout changes. |
| **DS-30: masked scalar insertion instead of growing LOR unions** | At 10 sources, about **1.5–7.6% more time** on RDF and **5.9–6.5% more time** on C-alias; Java essentially neutral. | Not adopted. Replacing a union with scalar assign is not automatically cheaper in these workloads. |
| **DS-31: joint RSM-state/graph-vertex terminal eligibility** | At 10 sources Java time ratio **0.036528**, a **27.38× speedup**; RDF ratios **3.95–14.88**, C-alias **38.49–61.94**. | Declined as a general replacement. The additional graph-support indexing/scanning/sorting is part of the timed query. The result is strongly workload-specific; it does not justify dropping the simpler retained RSM-row filter. |
| **DS-33: adaptive orientation with an immutable opposite-format cache** | At 10 sources Java is **27.0% slower**; other workload changes are mostly near zero. | Not adopted. The tested heuristic/cache did not provide a broad benefit. |
| **DS-34: lazy visited-state bags** | At 10 sources C-alias is about **5.7–6.5% slower**, Java essentially neutral. | Not adopted. Deferring unions into disjoint bags, rank merges and sequential subtraction did not produce a broad gain. |
| **DS-35: lazy nonterminal-summary bags** | At 10 sources C-alias is about **15.0–16.0% slower**, Java essentially neutral. | Not adopted. Component products and repeated masking did not repay the deferred merges. |
| **DS-37: direct mxm accumulation into acc** | At 10 sources Java is **15.7% slower** and C-alias **7.4–8.4% slower**; go_hierarchy improves slightly. | Not adopted. Keep overwrite-to-temporary plus controlled union/first-contribution swap. This also agrees with the earlier `e903c10d` restoration of temporary + eWiseAdd accumulation. |

DS-20/21/22 have complete 1/10/100-source experiments. DS-30/31/33/34/35/37 and D3 evidence above is from completed **10-source** comparisons; interrupted 100-source output is not used to claim a result. D1/config_block do have later complete 100-source evidence, so the archive’s earlier “strongest broad candidate” label for config_block is no longer the full assessment.

### Smaller historical alternatives that did not survive as separate optimizations

- **S2b, extra final-state guard on W bookkeeping:** stage1 ratio 1.004 relative to S1a + S2, effectively neutral/slower. The current code does not use that extra Phase-2 guard; Phase 4 still skips NULL final vectors.
- **S1b, direct accumulation of the old call mxm:** stage1 ratio 0.997 relative to S1a, too small to establish a material gain. That construction path is now superseded entirely by DS-39 tuple building. It must not be confused with DS-37 accumulation of graph products.
- **Blanket removal of clears/emptiness checks:** not an adopted strategy. The current code retains clears needed for reshape, build or accumulator lifecycle and checks that skip genuinely empty work. Historical S7 screens showed only around 0.7–0.8% aggregate improvement, and later DS-38 was near neutral. The final cleanup removes operations with an established invariant, not arbitrary guards.

## Directions removed or deferred without measured speedups

- **D4/D5 wide label packing:** explicitly removed from scope before implementation. The retained records contain no candidate timing, so there is no defensible speedup or slowdown to report. This differs from the implemented QV × V block-layout candidates.
- **Additional application-level parallelism:** discussed in this chat, then explicitly dropped by the user. No parallel variant was implemented or benchmarked. GraphBLAS’s existing internal parallelism remains available; no extra parallel-loop speedup is established.
- **Generic buffer reuse beyond the implemented call buffers, and prepare-once graph/RSM caching across source chunks:** discussed/deferred. The archived R10 analysis identifies repeated adapter preparation as a large wall-time concern, but does not measure an implemented cache. Do not report a kernel speedup from that proposal or confuse it with DS-39’s actual reusable tuple buffers.
- **Materialized Kronecker/wide packed-label alternatives:** absent from the measured candidate implementations. They cannot be assigned an experimental result from the existing records.

The resulting direction is to preserve the current representation, avoid unnecessary GraphBLAS operations and empty contributions, reuse information already computed, and retain simple eligibility filters. More invasive layout, format and lazy-state schemes were tested independently and generally not selected; the Java-specific results remain useful evidence rather than universal rejection of those ideas.

## Evidence and reproducibility

The completed selected ablations and combined run were independently checked from their raw CSVs for count/hash equality and execution success while preparing this report. Historical screening numbers and earlier negative experiments were recomputed from retained summary tables; their original sources/harness context and limitations remain attached. Existing snapshots and run files were not modified.

- [Current implementation](/home/hollow/repo/LAGraph/experimental/algorithm/LAGraph_CFPQ_RSM.c)
- [Full implementation comparison and limits](/home/hollow/repo/LAGraph/CFPQ_RSM_COMPARISON_RESULTS.md)
- [100-source isolated ablations, per-workload results](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/s100-selected-2026-11-03_12-21-29/morning_summary.csv)
- [100-source isolated ablations, provenance](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/s100-selected-2026-11-03_12-21-29/provenance.txt)
- [100-source combined optimization results](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/optimized-v1-2026-11-03_15-42-40/morning_summary.csv)
- [Historical stage1 screening results](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/stage1/summary.csv)
- [Historical stage2a filtering comparison](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/stage2a/summary.csv)
- [Historical stage3a layout comparison](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/stage3a/summary.csv)
- [Completed DS-20/21/22 experiments](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/archive/2026-11-03/results/ds20_21_22_morning_summary.csv)
- [Completed R10 1/10-source workload results](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/archive/2026-11-03/results/r10_workloads_s1_s10.csv)
- [Earlier profiling and experiment implementation reports](/home/hollow/repo/LAGraph/DEEPSEEK_FEEDBACK.md)
- [Immutable source/patch catalogue and interpretation notes](/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/archive/2026-11-03/README.md)
- [Deferred/discarded direction record](/home/hollow/repo/LAGraph/DEEPSEEK_REMAINDER.md)
