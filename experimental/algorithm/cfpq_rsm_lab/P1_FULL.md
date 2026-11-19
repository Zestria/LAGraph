# P1 without coordinate compression

`p1_full` derives from the frozen `p1_review` source. It removes observed-endpoint
coordinate IDs while retaining the parent's batched execution and support
filtering. Its [source](variants/active/p1_full__0f85bb5f1dc6.c) has SHA-256
`0f85bb5f1dc6b8e1af531010997362086e1463ef68091965c77c559e0d807e38`.
The [parent-relative patch](patches/active/p1_full.patch) applies to the
[archived P1 reviewed source](variants/archive/p1_review__1c5ad06ef855.c).

## What changed

| Property | `p1_review` | `p1_full` |
| --- | --- | --- |
| Forward coordinate | Compact ID for `(destination state, w)` | `destination_state*V+w` |
| Forward graph matrix | `V × C`, observed endpoint count `C` | `V × (Q*V)` |
| Dual coordinate | Compact ID for `(member, v)` | `member*V+v` |
| Dual graph matrix | `C × V` | `(K*V) × V` |
| Support lookup | Membership plus compact coordinate | Membership only |
| Output decoding | `col_member` / `col_vertex` tables | Division and remainder by `V` |

Forward coordinates use the destination RSM state, rather than the member,
to retain P1's endpoint sharing: different labels with the same continuation
and graph endpoint still share a coordinate. `GrB_Matrix_build_BOOL(..., GrB_LOR)`
merges duplicate entries at those coordinates. Other RSM states remain distinct.

For dual families, the member index remains essential: two transitions returning
to the same destination may require different graph labels. The full axis has
`K*V` positions even when `K > Q`; it must not be replaced by a state axis.

The support hash stores only `(global member ID, graph source vertex)` keys.
It answers whether the relevant graph relation has an outgoing edge there.
It does not assign or return matrix coordinates. Keeping this membership set
retains P1's filtering and expected-constant-time lookup while isolating the
removal of coordinate compression. Hashing has not been removed altogether.

The source shrinks from 1,560 to 1,440 lines. Removed machinery includes the
forward endpoint hash, compact-coordinate values, endpoint decoding arrays,
and their resizing/cleanup. GraphBLAS chooses storage and multiplication
strategy for the full logical axes; no dense format or thread count is forced.

## What remains the same

- Family discovery, ordering, and active-family dispatch.
- Forward endpoint sharing across identical continuations.
- One global frontier-tuple scan per relevant sweep for all dual families.
- Source-state dispatch and graph-support filtering before assembling inputs.
- Batched input construction and output routing over actual entries.
- Skipping dual multiplication when the filtered input is empty.
- Residual ordinary terminal transfers and first-contribution accumulation.
- Input validation and the nonterminal/call/return/fixed-point phases.
- One-shot timing, including construction and cleanup.

The source from `// Phases 2+3:` through the end is byte-identical to P1 reviewed.
This is checked by the registry regression suite. Dimension products are checked
before creating full-axis matrices; oversized families fall back to ordinary
terminal execution. Thus a family can fall back here even if P1's compact axis
would fit. Full logical dimensions still permit sparse/hypersparse storage.

## Files and historical versions

`variants/active/` and `patches/active/` contain `baseline` and `p1_full`.
The prior snapshots and their patches are registered in the archive. The
original validation evidence is preserved with the
[preceding campaign](archive/campaigns/cfpq-full-coordinates-validation-2026-11-17/CURRENT_CAMPAIGN.json).

## Reproduce the comparison against P1 reviewed

From the lab directory:

```bash
python3 run_simple.py --config configs/java_points_to.csv --allow-archived \
  --variants p1_review p1_full --reference p1_review \
  --source-sizes 1,10,100 --chunks 30 --rounds 5 --hot
```

Add `--dry-run` to inspect the commands, or `--build-only` to compile the two
libraries without running benchmarks. `configs/java_points_to.csv` has exactly
Avrora (`avrora.g`) and Batik (`batik.g`). Baseline is not added to this command.
Each variant runs in one `cfg_bench` process, with one warm-up followed by five
measurements per source chunk; the same source selection/seed is used for both.

The runner now accepts `--reference`; default behavior remains baseline when
it is selected, otherwise the first selected variant. Settings and summary CSVs
record the reference. Every repetition's count/hash must agree with that
reference, and ratios are candidate time divided by reference time. Reference
agreement is not a replacement for independent-oracle validation.

The experiment saves source/runtime/input hashes, inherited OpenMP environment,
CPU affinity, commands, logs, raw CSVs, and summaries to a fresh directory under
`runs/`. No OpenMP variable or GraphBLAS thread setting is changed. Construction
and cleanup remain inside the measured algorithm call. Old simple-run results
without a `reference` setting continue to analyze against baseline.

## Validation

These receipts record validation when `p1_full` was first added. Their paths
refer to that campaign. See [current validation](CURRENT_EXPERIMENT.md) for the
checks against the promoted source.

| Check | Outcome | Receipt |
| --- | --- | --- |
| Candidate oracle + existing tests | PASS | [receipt](runs/2026-11-17_22-37-54-526536_full-coordinates/oracle-existing.result.json) |
| Candidate loaded-library smoke | PASS | [receipt](runs/2026-11-17_22-37-54-526536_full-coordinates/loaded-smoke.result.json) |
| Candidate: all 15 reduced datasets | PASS | [receipt](runs/2026-11-17_22-37-54-526536_full-coordinates/reduced15.result.json) |
| Baseline control: oracle + existing + reduced | PASS | [receipt](runs/2026-11-17_22-39-19-264338_full-control-baseline/reduced15.result.json) |
| P1 reviewed control: oracle + existing + reduced | PASS | [receipt](runs/2026-11-17_22-39-51-698299_full-control-review/reduced15.result.json) |
| Baseline tiny profile | PASS | [receipt](runs/2026-11-17_22-40-28-437534_full-profile-baseline/command.result.json) |
| P1 reviewed tiny profile | PASS | [receipt](runs/2026-11-17_22-40-38-147835_full-profile-review/command.result.json) |
| Candidate tiny profile | PASS | [receipt](runs/2026-11-17_22-40-48-554661_full-profile-candidate/command.result.json) |
| Allocation sweep: budgets 0..1051, first success 1047 | PASS | [receipt](runs/2026-11-17_22-37-54-526536_full-coordinates/allocation-sweep.result.json) |
| Two-variant Java smoke: one 100-source chunk per graph | PASS | [receipt](runs/2026-11-17_22-37-54-526536_full-coordinates/java-pair-smoke.result.json) |
| Exact requested comparison command: dry run | PASS | [receipt](runs/2026-11-17_22-37-54-526536_full-coordinates/comparison-dry-run.result.json) |
| 75 tooling tests | PASS | [receipt](runs/2026-11-17_22-45-05-245957_tooling-tests/unittest.result.json) |
| Pinned runtime/input lock and all 89 diff proofs | PASS | [receipt](runs/2026-11-17_22-41-11-349485_integrity-check/integrity.result.json) |

The final oracle/existing suite reports 19,795 checks with zero failures,
including 200 generated oracle cases and the new K>Q dual-axis regression.
All 10 existing tests and all 15 reduced datasets pass. Reduced dataset counts
use the pinned CNF-reference cache; the oracle compares exact reachable sets.

The exact frozen algorithm bytes also passed preflight ASan/UBSan and leak
checks outside the sandbox on the preceding 19,646-check suite (200 generated
cases included). The linked libraries were not instrumented. The sanitizer used
`ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1` because the invalid-input
suite intentionally attempts an oversized allocation. The subsequently added
K>Q case is included in the final normal oracle run.

The first tooling receipt had a valid unittest exit but was rejected by the
strict log checker because buffered fixture output followed the OK summary.
Fixture stdout is now captured within the tests; the fresh 75-test receipt
above and `make status-check` pass. The earlier receipt/log are retained.

The Java smoke saved exactly the two requested variants, with P1 reviewed as
reference, and exactly Avrora/Batik. Both candidate result counts/hashes match.
It used one chunk and one measurement with warm-ups disabled. The completed
30-chunk comparison is reported below.

## Avrora and Batik results

Both variants completed 180 matched chunks and 900 measurements. The recorded
result counts and nonzero hashes agree in every repetition. The frozen source
hashes and pinned inputs matched, and the saved summaries were recomputed from
the raw timings. [Saved run summary](runs/2026-11-17_22-55-06-188826_simple/summary.csv)

| Graph | Sources | P1 reviewed median (ms) | P1 full median (ms) | Paired time change | P1 full wins |
| --- | ---: | ---: | ---: | ---: | ---: |
| Avrora | 1 | 16.95 | 16.39 | −8.0% | 22/30 |
| Avrora | 10 | 29.85 | 28.79 | −1.6% | 16/30 |
| Avrora | 100 | 119.02 | 120.64 | −0.5% | 15/30 |
| Batik | 1 | 28.06 | 24.94 | −13.4% | 28/30 |
| Batik | 10 | 33.30 | 34.26 | −0.6% | 16/30 |
| Batik | 100 | 93.43 | 95.89 | +1.4% | 10/30 |

Displayed times are medians of chunk medians. Paired changes use the geometric
mean of matching chunk ratios, so their sign can differ from the ratio of the
displayed medians. Across the 180 chunks, the geometric ratio is 0.961 and
`p1_full` wins 107 chunks. The largest reported process RSS in each
graph/source-size group is 1.8–2.9 MiB lower with `p1_full`.

The median paired single-source change is −1.5% on Avrora and −10.8% on Batik.
Several large wins contribute to Avrora's stronger geometric mean. Batik's
win appears in 28 of its 30 matched single-source chunks.

The single-source results favor using full coordinates with P1's batched,
support-filtered transfers. The 10- and 100-source results are close, with
Batik at 100 sources slightly favoring the compact layout. Together with the
earlier [literal block comparison](P1_BLOCKS.md), this suggests preserving
batching and testing other data-movement costs before restoring coordinate
compression. Each variant ran in one process, P1 reviewed first. The timings
include setup and execution. Phase measurements and a reversed-order repeat
would help distinguish construction savings, execution changes, and run-order
effects.
