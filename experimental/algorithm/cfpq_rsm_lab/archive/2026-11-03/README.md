# CFPQ RSM experiment archive — 2026-11-03

This is an immutable local snapshot outside the ignored build directory. Do not overwrite it; create a dated successor for future measurements. No new benchmark was run to make this archive. No variant has been promoted or combined.

## Start here

- [Recent candidate catalogue](#recent-candidate-catalogue): exact source snapshots, parent-specific patches and conclusions.
- [Historical inventory](historical_inventory.csv): every variant/display name recovered from old summary files, including combinations, controls and deliberate wrong-result self-tests.
- [All recorded summary rows](results/all_recorded_summary_rows.csv): retained original status/ratio fields and evidence paths; these are not newly validated results.
- [R10 analysis](results/R10_ANALYSIS.md) and [per-workload 1/10-source results](results/r10_workloads_s1_s10.csv).
- [Completed DS-20/21/22 results](results/ds20_21_22_morning_summary.md) and [CSV](results/ds20_21_22_morning_summary.csv).
- [Machine-readable variant registry](variants.json), [snapshot metadata](context/snapshot.json), `evidence.tar.gz` and its [member manifest](evidence_manifest.jsonl).

## Recent candidate catalogue

Every row has passed the previously agreed smoke/lab correctness checks. This records acceptance; it does not reopen testing. Completed DS-20/21/22 timings cover 1/10/100 sources. The other 13 have complete 1/10-source measurements; the interrupted 100-source attempt is archived as partial, not accepted timing data.

| Variant | Exact change | Saved implementation / diff | Measured conclusion |
| --- | --- | --- | --- |
| ds20 — Return orientation | Permanent column/hypersparse W and summary delta; includes conversion/reshape costs. | [source](sources/ds20.c) · [diff vs baseline](patches/ds20.patch) | Negative completed experiment; retain without rerunning. |
| ds21 — Waiting-state deduplication | Subtract W from newly shifted waiting configurations before calls/products/union. | [source](sources/ds21.c) · [diff vs baseline](patches/ds21.patch) | Negative completed experiment; sampled overlap was absent. |
| ds22 — Earlier block visited layout | Block P with extra fresh_B and call reshapes; differs from lean D1. | [source](sources/ds22.c) · [diff vs baseline](patches/ds22.patch) | Negative completed experiment; does not settle D1. |
| ds30_assign — Masked scalar insertion | Replace growing P, graph_nt and W unions with structural masked scalar true assignment. | [source](sources/ds30_assign.c) · [diff vs baseline](patches/ds30_assign.patch) | Generally neutral/slower on completed 1/10-source runs. |
| ds31_joint — Joint terminal eligibility | Index outgoing labels by RSM state and graph vertex; skip impossible terminal chains. | [source](sources/ds31_joint.c) · [diff vs baseline](patches/ds31_joint.patch) | Very strong on Java, major regressions on RDF/C-alias; workload-specific. |
| ds32_empty — Empty/first-contribution paths | Skip empty product unions and swap the first nonempty product into acc. | [source](sources/ds32_empty.c) · [diff vs baseline](patches/ds32_empty.patch) | Promising broadly, especially Java; keep independent. |
| ds33_format — Adaptive terminal orientation | Operand-size heuristic and private opposite-format cache of immutable graph terminals. | [source](sources/ds33_format.c) · [diff vs baseline](patches/ds33_format.patch) | Mostly neutral/slower; Java regression at 10 sources. |
| ds34_lazy_p — Lazy visited state | Disjoint frontier bags with cached cardinalities, size-rank merges and sequential subtraction. | [source](sources/ds34_lazy_p.c) · [diff vs baseline](patches/ds34_lazy_p.patch) | No broad gain at 1/10 sources; C-alias regression at 10. |
| ds35_lazy_nt — Lazy summary relations | Disjoint summary bags, streaming component products, sequential masks and final component mxv. | [source](sources/ds35_lazy_nt.c) · [diff vs baseline](patches/ds35_lazy_nt.patch) | No broad gain at 1/10 sources; C-alias regression at 10. |
| ds36_pair — Pattern products | Replace BOOL LOR_LAND products with ANY_PAIR under the accepted true-only contract. | [source](sources/ds36_pair.c) · [diff vs baseline](patches/ds36_pair.patch) | Mostly small changes; no large demonstrated broad gain. |
| ds37_fused — Fused accumulation | Accumulate graph products directly into acc with mxm/LOR, avoiding temporary union. | [source](sources/ds37_fused.c) · [diff vs baseline](patches/ds37_fused.patch) | Regresses Java and C-alias at 10 sources. |
| ds38_no_product_clear — One redundant-clear removal | Remove only the product scratch clear before overwrite mxm. | [source](sources/ds38_no_product_clear.c) · [diff vs baseline](patches/ds38_no_product_clear.patch) | Mostly near baseline; not a blanket clear-removal experiment. |
| ds39_direct_calls — Direct flat calls | Collect call tuples and bulk-build flat M_call, replacing diagonal/reshape/call mxm. | [source](sources/ds39_direct_calls.c) · [diff vs baseline](patches/ds39_direct_calls.patch) | Modest improvements, especially C-alias. |
| d1_native — Lean block P and calls | Native block call tuples and visited subtraction before copying/reshaping only fresh frontier. | [source](sources/d1_native.c) · [diff vs baseline](patches/d1_native.patch) | Modest improvements; independent of earlier DS-22. |
| config_block — Consistent block configurations | QV x V configurations with frontier tuple buckets and implicit RSM routing; no reshape or Kronecker matrix. | [source](sources/config_block.c) · [diff vs baseline](patches/config_block.patch) | Strongest broad candidate so far; not promoted or combined. |
| d3_masked_product — Visited-masked graph products | Add complemented block P masks to D1 terminal/skip/return products. | [source](sources/d3_masked_product.c) · [diff vs d1_native](patches/d3_masked_product.patch) | No convincing improvement over D1; compare with D1, not only baseline. |

## Baselines, interpretation and scope

The corrected parent is [baseline.c](sources/baseline.c), SHA-256 `f3642839be04eee2d6411d3db9d6973c50c19f386d7e61f4d9ff5a926af0447b`. D3 alone uses d1_native as its immediate control; both hashes are in variants.json. Retained run provenance binds measured libraries to source and dependency hashes. Ratios are variant/baseline: lower is faster. Compare the duplicate baselines before interpreting small differences. Process peak memory includes adapter/runtime state and is not isolated kernel memory.

The older I1–I6/S1–S7 names and combinations are preserved in historical_inventory.csv and the full evidence archive, including stage0–stage5, correctness/port checks, DS-00/01 reports and negative-control self-tests. Their baselines, configurations and harness versions differ. Original `ok` fields are historical claims, not an independent review under the corrected baseline; do not pool them with R9/R10 or treat each combination as a standalone ablation. Names alone do not establish exactly which historical component survived; inspect the saved source/patch/provenance.

Discarded D4/D5 wide label packing is not listed as an implemented optimization. Generic buffer reuse and other discussion-only ideas must not be marked measured. Source snapshots of the advanced CFL implementation/wrapper are archived as research context, not as extra RSM benchmark candidates.

## Benchmark and threading record

Recent timing settings: all seven configs/lab.csv pairs; sources 1/10/100 planned; 30 chunks, 5 measured rounds, 1 unmeasured warm-up per chunk/display, seed 2026, duplicate baseline displays, --threads 0, uninstrumented timing. Preserve interrupted raw CSVs rather than filling missing rounds. All completed R10 1/10-source comparisons matched result counts and hashes.

The R10 runner unsets OMP_NUM_THREADS/OMP_THREAD_LIMIT but never recorded actual per-operation threads. On 2026-11-03 an initialization-only probe of the same installed libraries returned OpenMP max 12 and GraphBLAS cap 12; small GraphBLAS calls and serial adapter preparation can still occupy one core. This does not prove the overnight active-thread count. Repeated per-chunk graph/RSM preparation dominates much of elapsed time; see the saved analysis. No 100-source continuation is required or authorized by this archive.

## What evidence.tar.gz preserves

- All existing lab/build files except regenerable JIT-cache directories and Python bytecode caches: raw CSVs, logs, status/commands, summaries, provenance, source snapshots, generated variants, patches/rejects, profile outputs, build scripts, shared libraries and test executables. This includes partial/failed/interrupted runs and accepted smoke evidence.
- Current lab sources, configs, scripts and tests; reviewer work orders, DeepSeek feedback, current production RSM and advanced CFL/wrapper research sources.
- CFG_bench sources, Makefile, configs, tests sources/reduced fixtures and applicable instructions; selected GraphBLAS/LAGraph initialization/thread-control sources and headers.
- Input/dependency manifests remain in run provenance. Large external graph datasets, external installed dependency binaries and JIT caches are not copied. Their existing paths/hashes are retained where provenance recorded them; this is not a complete dependency installation or remote backup.

Archive paths beginning `repo/` are repository-relative; `external/CFG_bench/` and `external/GraphBLAS/` identify external checkout context. Archived shared objects/executables are provenance artifacts, not portable replacements for the recorded libraries. Do not execute old binaries merely to inspect results.

## Integrity and recovery

From this directory run `python3 verify.py` to verify SHA256SUMS and every evidence archive member against evidence_manifest.jsonl. This reads data only. List archive members with `tar -tzf evidence.tar.gz`. To restore, extract into a new empty directory, never over a live checkout:

```bash
mkdir /tmp/cfpq-rsm-archive-restore
tar -xzf evidence.tar.gz -C /tmp/cfpq-rsm-archive-restore
```

Original absolute paths inside logs/provenance are historical. The archive manifest maps them to saved member paths. Original sources and runs were not moved or deleted. Add this directory to version control or copy it with its checksum file when transferring it; no Git commit or external upload is performed by the snapshot operation.
