# Current CFPQ RSM experiment

> Generated from execution receipts by `make status`; do not edit validation claims by hand.

Campaign: `cfpq-reviewed-standalone-validation-2026-11-07`. State: **VALIDATED_FOR_CORRECTNESS_AND_TINY_PROFILING**.

Latest recorded attempt: `2026-11-07T21:30:47.207391+03:00` (Europe/Moscow).

Reviewed standalone P1-P4 correctness and tiny-profile validation. Only bounded validation is recorded; no full-recipe performance receipt exists. Production identity is checked against the agreed baseline.

Registry: 86 snapshots / 85 distinct source hashes. [Machine evidence](VALIDATION.json), [campaign definition](CURRENT_CAMPAIGN.json), [registry](variants/REGISTRY.md), [backup](BACKUP.md).

| Variant | Source SHA-256 (prefix) | Oracle + existing | Loaded smoke | Reduced data | Tiny profile | Full performance |
| --- | --- | --- | --- | --- | --- | --- |
| baseline | `c7efb94f21bb` | PASS | PASS | PASS | PASS | NOT MEASURED |
| p1_review | `1c5ad06ef855` | PASS | PASS | PASS | PASS | NOT MEASURED |
| p2_review | `dc0f7c50405b` | PASS | PASS | PASS | PASS | NOT MEASURED |
| p3_review | `35109ff0c008` | PASS | PASS | PASS | PASS | NOT MEASURED |
| p4_review | `7eefac364098` | PASS | PASS | PASS | PASS | NOT MEASURED |

Tooling tests: **PASS** (57 tests).

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

All timed/profile recipes pin seed 2026, one thread, blocked order, one warm-up, the executable path/hash, graphs/CNF/RSM/source pools, and runtime dependencies in `inputs/pinned.json`. The 200-case oracle uses seed 2026. Reduced counts may use the CNF-reference cache; exact small-case oracle checks provide stronger set evidence.

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
| baseline/oracle_existing | [receipt](runs/2026-11-07_18-01-14-981286_review-baseline/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| baseline/timing_smoke | [receipt](runs/2026-11-07_21-30-30-950065_compare-smoke-s1/command.result.json) | PASS: receipt and artifacts verified |
| baseline/reduced | [receipt](runs/2026-11-07_18-01-14-981286_review-baseline/reduced15.result.json) | PASS: receipt and artifacts verified |
| baseline/profile_smoke | [receipt](runs/2026-11-07_21-18-28-578949_review-profile-fixed-baseline/command.result.json) | PASS: receipt and artifacts verified |
| p1_review/oracle_existing | [receipt](runs/2026-11-07_18-01-49-599748_review-p1_review/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| p1_review/timing_smoke | [receipt](runs/2026-11-07_21-30-30-950065_compare-smoke-s1/command.result.json) | PASS: receipt and artifacts verified |
| p1_review/reduced | [receipt](runs/2026-11-07_18-01-49-599748_review-p1_review/reduced15.result.json) | PASS: receipt and artifacts verified |
| p1_review/profile_smoke | [receipt](runs/2026-11-07_21-18-41-758154_review-profile-fixed-p1_review/command.result.json) | PASS: receipt and artifacts verified |
| p2_review/oracle_existing | [receipt](runs/2026-11-07_18-02-28-108137_review-p2_review/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| p2_review/timing_smoke | [receipt](runs/2026-11-07_21-30-30-950065_compare-smoke-s1/command.result.json) | PASS: receipt and artifacts verified |
| p2_review/reduced | [receipt](runs/2026-11-07_18-02-28-108137_review-p2_review/reduced15.result.json) | PASS: receipt and artifacts verified |
| p2_review/profile_smoke | [receipt](runs/2026-11-07_21-18-52-168932_review-profile-fixed-p2_review/command.result.json) | PASS: receipt and artifacts verified |
| p3_review/oracle_existing | [receipt](runs/2026-11-07_18-03-04-874222_review-p3_review/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| p3_review/timing_smoke | [receipt](runs/2026-11-07_21-30-30-950065_compare-smoke-s1/command.result.json) | PASS: receipt and artifacts verified |
| p3_review/reduced | [receipt](runs/2026-11-07_18-03-04-874222_review-p3_review/reduced15.result.json) | PASS: receipt and artifacts verified |
| p3_review/profile_smoke | [receipt](runs/2026-11-07_21-19-02-727654_review-profile-fixed-p3_review/command.result.json) | PASS: receipt and artifacts verified |
| p4_review/oracle_existing | [receipt](runs/2026-11-07_18-03-39-660251_review-p4_review/oracle-existing.result.json) | PASS: receipt and artifacts verified |
| p4_review/timing_smoke | [receipt](runs/2026-11-07_21-30-30-950065_compare-smoke-s1/command.result.json) | PASS: receipt and artifacts verified |
| p4_review/reduced | [receipt](runs/2026-11-07_18-03-39-660251_review-p4_review/reduced15.result.json) | PASS: receipt and artifacts verified |
| p4_review/profile_smoke | [receipt](runs/2026-11-07_21-19-13-257074_review-profile-fixed-p4_review/command.result.json) | PASS: receipt and artifacts verified |

Integrity receipt: [runs/2026-11-07_21-25-38-146152_integrity-check/integrity.result.json](runs/2026-11-07_21-25-38-146152_integrity-check/integrity.result.json).

Tooling receipt: [runs/2026-11-07_21-25-39-317949_tooling-tests/unittest.result.json](runs/2026-11-07_21-25-39-317949_tooling-tests/unittest.result.json).

Unmeasured work and limits:

- One-round smoke ratios are not speedups. Full-workload performance is measured only for source sizes with a complete accepted full-recipe receipt; coverage is recorded separately.
- Tiny profiles validate instrumentation and result agreement; they do not characterize full Java/RDF/C-alias bottlenecks. Workload-profile attempts are recorded separately by source size.
- Candidate-union and waiting-prefix-overlap probes are uncollected by active profiling and marked unavailable; legacy zero fields are not measurements.
- Matrix memory records are selected allocation snapshots, not complete C-buffer accounting or peak live workspace.
- Bounded sanitizer and allocation-failure checks are recorded separately in PATCH_REVIEW.md; they do not instrument the linked GraphBLAS/LAGraph libraries.
- Reduced-data checks compare counts against a CNF reference that may be cached; exact small-case oracle checks are stronger evidence.
- Fusion retains full global state dimensions and per-continuation work; the status does not imply a runtime or memory benefit.
- Historical archived variants and pre-receipt logs are preserved evidence, not newly validated results.

Failure history:

- [runs/2026-11-07_18-04-06-945507_review-profile-p4_review/command.result.json](runs/2026-11-07_18-04-06-945507_review-profile-p4_review/command.result.json): FAIL / exit 1 (profile_smoke).
