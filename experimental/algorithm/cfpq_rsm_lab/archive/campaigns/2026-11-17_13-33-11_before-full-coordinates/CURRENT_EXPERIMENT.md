# Current CFPQ RSM experiment

> Generated from execution receipts by `make status`; do not edit validation claims by hand.

Campaign: `cfpq-reviewed-inherited-openmp-validation-2026-11-07`. State: **NEEDS_ATTENTION**.

Latest recorded attempt: `2026-11-17T20:07:24.853921+03:00` (Europe/Moscow).

Reviewed standalone P1-P4 with inherited OpenMP threading. Full-recipe attempts are recorded separately by source size in VALIDATION.json. Production identity is checked against the agreed baseline.

Registry: 88 snapshots / 87 distinct source hashes. [Machine evidence](VALIDATION.json), [campaign definition](CURRENT_CAMPAIGN.json), [registry](variants/REGISTRY.md), [backup](BACKUP.md).

| Variant | Source SHA-256 (prefix) | Oracle + existing | Loaded smoke | Reduced data | Tiny profile | Full performance |
| --- | --- | --- | --- | --- | --- | --- |
| baseline | `c7efb94f21bb` | PASS | STALE | PASS | STALE | PARTIAL |
| p1_review | `1c5ad06ef855` | PASS | PASS | PASS | STALE | PARTIAL |
| p2_review | `dc0f7c50405b` | STALE | PASS | PASS | STALE | PARTIAL |
| p3_review | `35109ff0c008` | STALE | PASS | PASS | STALE | PARTIAL |
| p4_review | `7eefac364098` | STALE | PASS | PASS | STALE | PARTIAL |

Tooling tests: **FAIL**.

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
make correctness VARIANT=all
make profile VARIANT=p4_review
make compare RECIPE=full SIZES=100 VARIANTS=baseline,p1_review,p2_review,p3_review,p4_review DRY_RUN=1
```

The final command only prints the expensive full recipe. Removing DRY_RUN deliberately starts performance work; it is not part of the current validation.

Evidence selected for each check:

| Variant/check | Latest receipt | Result |
| --- | --- | --- |
| baseline/oracle_existing | [receipt](runs/2026-11-17_20-06-35-594193_literal-blocks-control/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| baseline/timing_smoke | [receipt](runs/2026-11-17_20-04-00-592232_literal-blocks/loaded-smoke.result.json) | STALE: source identity differs from this campaign |
| baseline/reduced | [receipt](runs/2026-11-07_21-39-58-991878_openmp-correctness-baseline/reduced15.result.json) | PASS: receipt and artifacts verified |
| baseline/profile_smoke | [receipt](runs/2026-11-07_21-42-47-507946_openmp-profile-baseline/command.result.json) | STALE: missing or changed evidence/tool: tests/make_profile_variant.py |
| p1_review/oracle_existing | [receipt](runs/2026-11-17_20-06-48-388939_literal-blocks-compact-control/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| p1_review/timing_smoke | [receipt](runs/2026-11-07_21-40-30-580882_openmp-correctness-p1_review/loaded-smoke.result.json) | PASS: receipt and artifacts verified |
| p1_review/reduced | [receipt](runs/2026-11-07_21-40-30-580882_openmp-correctness-p1_review/reduced15.result.json) | PASS: receipt and artifacts verified |
| p1_review/profile_smoke | [receipt](runs/2026-11-07_21-42-58-920729_openmp-profile-p1_review/command.result.json) | STALE: missing or changed evidence/tool: tests/make_profile_variant.py |
| p2_review/oracle_existing | [receipt](runs/2026-11-07_21-41-02-128649_openmp-correctness-p2_review/oracle-existing.result.json) | STALE: missing or changed evidence/tool: tests/Makefile |
| p2_review/timing_smoke | [receipt](runs/2026-11-07_21-41-02-128649_openmp-correctness-p2_review/loaded-smoke.result.json) | PASS: receipt and artifacts verified |
| p2_review/reduced | [receipt](runs/2026-11-07_21-41-02-128649_openmp-correctness-p2_review/reduced15.result.json) | PASS: receipt and artifacts verified |
| p2_review/profile_smoke | [receipt](runs/2026-11-07_21-43-10-132860_openmp-profile-p2_review/command.result.json) | STALE: missing or changed evidence/tool: tests/make_profile_variant.py |
| p3_review/oracle_existing | [receipt](runs/2026-11-07_21-41-35-548236_openmp-correctness-p3_review/oracle-existing.result.json) | STALE: missing or changed evidence/tool: tests/Makefile |
| p3_review/timing_smoke | [receipt](runs/2026-11-07_21-41-35-548236_openmp-correctness-p3_review/loaded-smoke.result.json) | PASS: receipt and artifacts verified |
| p3_review/reduced | [receipt](runs/2026-11-07_21-41-35-548236_openmp-correctness-p3_review/reduced15.result.json) | PASS: receipt and artifacts verified |
| p3_review/profile_smoke | [receipt](runs/2026-11-07_21-43-21-246405_openmp-profile-p3_review/command.result.json) | STALE: missing or changed evidence/tool: tests/make_profile_variant.py |
| p4_review/oracle_existing | [receipt](runs/2026-11-07_21-42-12-515468_openmp-correctness-p4_review/oracle-existing.result.json) | STALE: missing or changed evidence/tool: tests/Makefile |
| p4_review/timing_smoke | [receipt](runs/2026-11-07_21-42-12-515468_openmp-correctness-p4_review/loaded-smoke.result.json) | PASS: receipt and artifacts verified |
| p4_review/reduced | [receipt](runs/2026-11-07_21-42-12-515468_openmp-correctness-p4_review/reduced15.result.json) | PASS: receipt and artifacts verified |
| p4_review/profile_smoke | [receipt](runs/2026-11-07_21-43-38-458910_openmp-profile-p4_review/command.result.json) | STALE: missing or changed evidence/tool: tests/make_profile_variant.py |

Integrity receipt: [runs/2026-11-17_20-07-17-010691_integrity-check/integrity.result.json](runs/2026-11-17_20-07-17-010691_integrity-check/integrity.result.json).

Tooling receipt: [runs/2026-11-17_20-07-03-920010_tooling-tests/unittest.result.json](runs/2026-11-17_20-07-03-920010_tooling-tests/unittest.result.json).

Unmeasured work and limits:

- Threading is chosen by GraphBLAS/OpenMP. The lab never sets or unsets OpenMP variables; inherited settings and CPU affinity are recorded. No per-operation active-thread count is measured. Prior single-thread validation remains in the archived campaign.
- One-round smoke ratios are not speedups. Full-workload performance is measured only for source sizes with a complete accepted full-recipe receipt; coverage is recorded separately.
- Tiny profiles validate instrumentation and result agreement; they do not characterize full Java/RDF/C-alias bottlenecks. Workload-profile attempts are recorded separately by source size.
- Candidate-union and waiting-prefix-overlap probes are uncollected by active profiling and marked unavailable; legacy zero fields are not measurements.
- Matrix memory records are selected allocation snapshots, not complete C-buffer accounting or peak live workspace.
- Bounded sanitizer and allocation-failure checks are recorded separately in PATCH_REVIEW.md; they do not instrument the linked GraphBLAS/LAGraph libraries.
- Reduced-data checks compare counts against a CNF reference that may be cached; exact small-case oracle checks are stronger evidence.
- Fusion retains full global state dimensions and per-continuation work; the status does not imply a runtime or memory benefit.
- Historical archived variants and pre-receipt logs are preserved evidence, not newly validated results.

Failure history:

- No failed receipt in this campaign. Historical failed/partial runs remain in legacy evidence.
