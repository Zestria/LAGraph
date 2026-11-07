# Four-patch review

Reviewed standalone p1–p4 against the unchanged `c7efb94f21bb` production baseline.
Original snapshots, original patches, and `stack_p1234` are preserved. The revised
standalone candidates are the default `make compare` selection; the stack has
not been recomposed. No full performance experiment was run.

| Candidate | Changes made | Remaining limit |
| --- | --- | --- |
| p1_review | Forward support columns share identical destination-state/vertex pairs; the temporary forward hash is freed after setup instead of retained in the runtime dual hash. Discarded groups are freed. Tuple, edge-sum, and hash sizing arithmetic is checked. | Group construction still occurs inside each kernel call; small groups may not amortize setup. Field/state identity is preserved. |
| p2_review | Reuse terminal slices of the extracted control-edge arrays instead of extracting each transition matrix two more times. Skip rebuilding all input matrices when every state and terminal is already live. | Conservative control reachability still has setup cost and does not prove recursive productivity. |
| p3_review | Scatter a single destination block with a GraphBLAS range assignment, avoiding tuple export/remap/build. | Multiple destinations still use tuple scatter; runtime benefit is unmeasured. |
| p4_review | Track eligible destinations per callee, visit only fused blocks present in each return product, and use two-index ranges instead of length-V row arrays. Ordinary products use the existing first-contribution merge. Remove unused local-routing fields and identity maps; allocate fusion scratch only when needed. | Global configuration/waiting dimensions remain unchanged; continuation products remain separate. This does not implement the previously proposed state-dimension halving. |

All four revisions also reject oversized tuple-buffer requests before geometric
growth. The existing oracle driver gained regressions for shared endpoints with
different continuation states and nullable recursive calls mixing ordinary and
fused returns. No benchmark, oracle, or aggregation replacement was introduced.

Tiny profiling exposed a generator bug: its return-statement regex rewrote the
word `return` inside a comment. The generator now masks C comments and literals
before matching statements; a regression covers comments and semicolons in
string literals. The failed profiling receipt remains in campaign history.

## Exact candidate artifacts

- **p1_review**: [patch](patches/active/p1_review.patch), [source](variants/active/p1_review__1c5ad06ef855.c), SHA-256 `1c5ad06ef8558104c9c6f66a90e63aea50f7e187aedaa3a47630b3650a44da3f`.
- **p2_review**: [patch](patches/active/p2_review.patch), [source](variants/active/p2_review__dc0f7c50405b.c), SHA-256 `dc0f7c50405bd2cb1ee3d2b9aa7407eecd88bbdcf4318a9e81b05e2bc85e9270`.
- **p3_review**: [patch](patches/active/p3_review.patch), [source](variants/active/p3_review__35109ff0c008.c), SHA-256 `35109ff0c008ba62b095c1500a7df6d1111e505be8dd636dec2e89831a0857b0`.
- **p4_review**: [patch](patches/active/p4_review.patch), [source](variants/active/p4_review__7eefac364098.c), SHA-256 `7eefac364098f9f1d6ae353962d4d9baf7be9ed1d96a30160e4698871a671823`.

## Validation

The current [receipt-derived status](CURRENT_EXPERIMENT.md) and
[VALIDATION.json](VALIDATION.json) report all five campaign sources (baseline
plus four revisions) passing the exact oracle, 10 existing C tests, loaded tiny
smoke, all 15 reduced-data cases, and tiny profiling with matching results.
Each full oracle run completed 19,462 checks with zero failures, including 200
generated cases. All 57 Python tooling tests pass; all 86 registered patch/hash
proofs and the pinned input/runtime lock pass.

Additional checks used the existing test Makefile and harness, with logs in
[runs/2026-11-07_patch-review-fX4HZS/](runs/2026-11-07_patch-review-fX4HZS/):

- ASan/UBSan: deterministic tests plus 20 generated cases, 6,542 checks per revised
  source, zero failures. Leak detection was disabled; linked GraphBLAS/LAGraph
  libraries were not instrumented. The first ASan invocation aborted on the
  deliberate giant allocation; the documented `allocator_may_return_null=1`
  setting allowed the bounded rerun to pass. Both logs are retained.
- Allocation-failure sweeps: zero failures for all four. First successful
  allocation budgets were 1,040 / 937 / 1,078 / 1,059 for p1/p2/p3/p4 respectively;
  the existing harness stops after its success streak. This checks the harness's
  allocation scenario, not every possible execution path.

The previous campaign definition is preserved under
[archive/campaigns/](archive/campaigns/). No source or data lock was relaxed.
Smoke ratios and profile times are not performance evidence. The new candidates
are ready for controlled timing; speed and peak-memory improvements remain
unmeasured.
