# Current CFPQ RSM experiment

> Generated from execution receipts by `make status`; do not edit validation claims by hand.

Campaign: `cfpq-full-coordinates-validation-2026-11-17`. State: **VALIDATED_FOR_CORRECTNESS_AND_TINY_PROFILING**.

Latest recorded attempt: `2026-11-17T22:45:16.708099+03:00` (Europe/Moscow).

P1 full-coordinate simplification with inherited OpenMP threading. Only bounded validation is recorded; no full-recipe performance receipt exists. Production identity is checked against the agreed baseline.

Registry: 89 snapshots / 88 distinct source hashes. [Machine evidence](VALIDATION.json), [campaign definition](CURRENT_CAMPAIGN.json), [registry](variants/REGISTRY.md), [backup](BACKUP.md).

| Variant | Source SHA-256 (prefix) | Oracle + existing | Loaded smoke | Reduced data | Tiny profile | Full performance |
| --- | --- | --- | --- | --- | --- | --- |
| baseline | `c7efb94f21bb` | PASS | PASS | PASS | PASS | NOT MEASURED |
| p1_review | `1c5ad06ef855` | PASS | PASS | PASS | PASS | NOT MEASURED |
| p1_full | `0f85bb5f1dc6` | PASS | PASS | PASS | PASS | NOT MEASURED |

Tooling tests: **PASS** (75 tests).

PASS means the latest matching attempt has exit 0, unchanged source/tool identities and artifacts, and passes result-completeness checks. A later failure or timeout supersedes an earlier pass; changed/missing evidence becomes STALE. Old logs without receipts are retained but cannot establish a current PASS.

Environment and all registered diff proofs: **PASS**.

The fixed recipes and current scope:

| Recipe/check | Config | Source size | Chunks | Measured rounds | State |
| --- | --- | --- | --- | --- | --- |
| smoke | `/home/hollow/repo/CFG_bench/configs/for_test.csv` | 1 | 1 | 1 | per-variant status above |
| profile_smoke | `/home/hollow/repo/CFG_bench/configs/for_test.csv` | 1 | 1 | 1 | per-variant status above |
| full | `/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/configs/lab_batik.csv` | 1,10,100 | 30 | 5 | per-source-size coverage in VALIDATION.json |
| profile_workloads | `/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/configs/lab_batik.csv` | 1,10,100 | 1 | 1 | per-source-size attempts in VALIDATION.json |
| reduced correctness | `/home/hollow/repo/CFG_bench/tests/data/configs/all.csv` | supplied starts | not chunked | test mode | 15-case count check |

All timed/profile recipes use inherited OpenMP threading. The tooling never sets or unsets OpenMP variables; GraphBLAS/OpenMP choose parallelism. Recipes pin seed 2026, blocked order, one warm-up, the executable path/hash, graphs/CNF/RSM/source pools, and runtime dependencies in `inputs/pinned.json`. Legacy threads=0 means inheritance, not a thread count. The 200-case oracle uses seed 2026. Reduced counts may use the CNF-reference cache; exact small-case oracle checks provide stronger set evidence.

Inherited OpenMP environment: `{}`. CPU affinity: `[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]`. An empty environment record means runtime defaults. Actual per-operation thread counts are not measured. Receipts from a different environment or affinity are STALE for this invocation.

Reproduce from the lab root:

```bash
make status
make status-check
make list
make verify
make tooling-tests
make correctness VARIANT=baseline
make correctness VARIANT=p1_review
make correctness VARIANT=p1_full
make profile VARIANT=p1_full
make compare RECIPE=full SIZES=100 VARIANTS=baseline,p1_review,p1_full DRY_RUN=1
```

The final command only prints the expensive full recipe. Removing DRY_RUN deliberately starts performance work; it is not part of the current validation.

Evidence selected for each check:

| Variant/check | Latest receipt | Result |
| --- | --- | --- |
| baseline/oracle_existing | [receipt](runs/2026-11-17_22-39-19-264338_full-control-baseline/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| baseline/timing_smoke | [receipt](runs/2026-11-17_22-39-51-698299_full-control-review/loaded-smoke.result.json) | PASS: receipt and artifacts verified |
| baseline/reduced | [receipt](runs/2026-11-17_22-39-19-264338_full-control-baseline/reduced15.result.json) | PASS: receipt and artifacts verified |
| baseline/profile_smoke | [receipt](runs/2026-11-17_22-40-28-437534_full-profile-baseline/command.result.json) | PASS: receipt and artifacts verified |
| p1_review/oracle_existing | [receipt](runs/2026-11-17_22-39-51-698299_full-control-review/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| p1_review/timing_smoke | [receipt](runs/2026-11-17_22-39-51-698299_full-control-review/loaded-smoke.result.json) | PASS: receipt and artifacts verified |
| p1_review/reduced | [receipt](runs/2026-11-17_22-39-51-698299_full-control-review/reduced15.result.json) | PASS: receipt and artifacts verified |
| p1_review/profile_smoke | [receipt](runs/2026-11-17_22-40-38-147835_full-profile-review/command.result.json) | PASS: receipt and artifacts verified |
| p1_full/oracle_existing | [receipt](runs/2026-11-17_22-37-54-526536_full-coordinates/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| p1_full/timing_smoke | [receipt](runs/2026-11-17_22-37-54-526536_full-coordinates/loaded-smoke.result.json) | PASS: receipt and artifacts verified |
| p1_full/reduced | [receipt](runs/2026-11-17_22-37-54-526536_full-coordinates/reduced15.result.json) | PASS: receipt and artifacts verified |
| p1_full/profile_smoke | [receipt](runs/2026-11-17_22-40-48-554661_full-profile-candidate/command.result.json) | PASS: receipt and artifacts verified |

Integrity receipt: [runs/2026-11-17_22-41-11-349485_integrity-check/integrity.result.json](runs/2026-11-17_22-41-11-349485_integrity-check/integrity.result.json).

Tooling receipt: [runs/2026-11-17_22-45-05-245957_tooling-tests/unittest.result.json](runs/2026-11-17_22-45-05-245957_tooling-tests/unittest.result.json).

Unmeasured work and limits:

- Threading is chosen by GraphBLAS/OpenMP. The lab never sets or unsets OpenMP variables; inherited settings and CPU affinity are recorded. No per-operation active-thread count is measured. Prior single-thread validation remains in the archived campaign.
- One-round smoke ratios are not speedups. Full-workload performance is measured only for source sizes with a complete accepted full-recipe receipt; coverage is recorded separately.
- Tiny profiles validate instrumentation and result agreement; they do not characterize full Java/RDF/C-alias bottlenecks. Workload-profile attempts are recorded separately by source size.
- Candidate-union and waiting-prefix-overlap probes are uncollected by active profiling and marked unavailable; legacy zero fields are not measurements.
- Matrix memory records are selected allocation snapshots, not complete C-buffer accounting or peak live workspace.
- Bounded sanitizer and allocation-failure checks are recorded separately in PATCH_REVIEW.md; they do not instrument the linked GraphBLAS/LAGraph libraries.
- Reduced-data checks compare counts against a CNF reference that may be cached; exact small-case oracle checks are stronger evidence.
- Historical archived variants and pre-receipt logs are preserved evidence, not newly validated results.
- The requested Avrora/Batik comparison uses run_simple.py with only p1_review and p1_full. Its full performance is not measured by bounded validation or tiny profiling; saved simple-run results are separate from legacy full-recipe receipts.

Failure history:

- No failed receipt in this campaign. Historical failed/partial runs remain in legacy evidence.

Requested Java-only comparison (separate simple-run results):

```bash
python3 run_simple.py --config configs/java_points_to.csv --variants p1_review p1_full --reference p1_review --source-sizes 1,10,100 --chunks 30 --rounds 5 --hot --dry-run
```

This selects only Avrora and Batik and the two named variants. Baseline is not added. Construction is included; remove --dry-run to run the comparison. Legacy full-recipe coverage above does not substitute for these Java results.
