# P1: literal block matrices versus compact endpoint axes

`p1_blocks` is the literal-block reference derived from `p1_review`. The snapshot is archived.
Its frozen source is [p1_blocks__697452884c07.c](variants/archive/p1_blocks__697452884c07.c),
SHA-256 `697452884c07cc8b22e295ad97ab8eca99e00a97df217c22725fbb8151e7b280`.
The [parent-relative patch](patches/archive/p1_blocks.patch) applies to P1 reviewed,
SHA-256 `1c5ad06ef8558104c9c6f66a90e63aea50f7e187aedaa3a47630b3650a44da3f`.
The production algorithm and existing frozen snapshots are unchanged.

This experiment keeps P1's topology-based family selection, its residual
ordinary-terminal path, and its input validation. The source from the
`Phases 2+3` comment through the end is byte-identical to P1 reviewed, so
nonterminal calls, summaries, returns, deduplication, and final projection are
the same. It is outside the existing reviewed-P1–P4 campaign.

## What P1's compact representation means

For a family with `K` transitions and a graph with `V` vertices, the literal
formulas use a block axis with `K*V` positions. P1 replaces that axis with IDs
only for combinations observed in the graph relations:

| Family | Full block axis | P1's compact axis |
| --- | --- | --- |
| Shared source, forward | `(member, destination vertex)` | Observed `(destination RSM state, destination vertex)` pairs |
| Shared destination, dual | `(member, source vertex)` | Observed `(member, source vertex)` pairs |

For example, let `K=3`, `V=8`, and each relation contain one edge:
`G0: 1→7`, `G1: 4→2`, `G2: 6→5`. The full horizontal matrix has 24 columns;
its nonzeros are in columns `7`, `10`, and `21`. With distinct continuation
states, P1 maps these to just three compact columns and remembers how to decode
them. The dual matrix similarly compresses its 24-position row axis using
observed graph sources.

P1 builds the map by extracting graph tuples and hashing endpoint pairs.
At execution time it gathers frontier entries into the compact coordinates and
routes result tuples back into the global configuration layout. Forward P1 also
merges coincident endpoints from different labels when they have the same
continuation state. The literal variant retains their separate blocks and ORs
their results when assigning to that shared state.

These are algebraically equivalent transfers, with different data movement and
matrix dimensions. Full logical axes do **not** require dense storage: the new
variant leaves sparsity, orientation, and multiplication strategy to GraphBLAS.
Neither variant forces GraphBLAS or OpenMP thread counts.

## Reading the literal implementation

Read these helpers before the generic family-discovery code:

1. `s_group_build_blocks()` constructs the graph matrices once per one-shot call.
2. `s_frontier_slice()` extracts `F_q[u,v] = M[q,u*V+v]` as a `V × V` matrix.
3. `s_group_forward()` implements the shared-source formula.
4. `s_group_dual()` implements the shared-destination formula.
5. `s_accumulate_block()` ORs a `V × V` result into the requested RSM state.

`K` denotes the number of transitions selected for a family. In Java points-to
it can differ from the grammar's `B`, because topology-based discovery can group
fixed transitions alongside indexed transitions, or exclude labels with no
graph support. Both candidates use the same discovery ordering and tie-breaks.

### First type: one source, separate destinations

```text
s --t_m--> d_m

H = [G0 | G1 | ... | G(K-1)]       V × (K*V)
Y = F_s H                         V × (K*V)
```

`GxB_Matrix_concat(..., 1, K, ...)` builds `H`. A single `GrB_mxm` computes
`Y`. For each member `m`, `GrB_Matrix_extract` selects columns
`m*V .. (m+1)*V-1` and `GrB_Matrix_assign` ORs that block into state `d_m`.
The loop runs over member blocks, not individual nonzeros.

### Second type: separate sources, one destination

```text
r_m --t_m--> d

D = [F_r0 | F_r1 | ... | F_r(K-1)] V × (K*V)
J = [G0; G1; ...; G(K-1)]         (K*V) × V
Y = D J = OR_m(F_rm G_m)          V × V
```

`GxB_Matrix_concat(..., K, 1, ...)` builds the fixed `J`. Each live sweep
extracts complete frontier slices, then
`GxB_Matrix_concat(..., 1, K, ...)` constructs `D`. A single `GrB_mxm`
computes `Y`, and one block assignment accumulates it into state `d`.
Extraction replaces the previous contents, including empty positions, so
inactive members cannot retain frontier entries from an earlier sweep.

The implementation does not extract graph/frontier/product tuples for these
transfers, build endpoint hash tables, or manually renumber sparse support.
Tuple extraction remains in setup to discover transitions in the RSM matrices.
Repeated labels and repeated continuation states retain separate member blocks.

Before building a family, checked products bound `K*V` and `V*(K*V)` using P1's
GraphBLAS limit. Oversized families use the ordinary terminal path. Thus the
same input can remain usable when the full axis cannot be represented even
though a compact one would fit.

## Comparison scope and commands

Both `p1_review` and `p1_blocks` expose the same one-shot API. Their graph-block
construction, frontier movement, multiplication, accumulation, and cleanup are
inside each algorithm call. This intentionally includes the cost of maintaining
the different representations. Use the default one-shot pinned manifest, not
the historical prepared API manifest.

From the lab directory:

```bash
make verify
# Inspect the Java comparison, then remove --dry-run to execute it.
python3 run_simple.py --allow-archived --variants baseline p1_review p1_blocks --dry-run
```

The final command retains the existing default two Java graphs, source sizes
1/10/100, 30 chunks, one warm-up and five measurements per chunk. It can be
expensive. All inherited OpenMP settings and CPU affinity are recorded by the
runner; the variant does not change them. `run_simple.py` compares count/hash
evidence for every sample. Exact small-case oracle checks provide stronger
correctness evidence than count/hash agreement.

The later full Java comparison is documented in the [saved analysis](runs/2026-11-17_p1_review_vs_blocks_analysis/analysis.md). The initial bounded validation below did not establish a speedup. The literal variant retains full block axes, duplicates graph
relations when a label occurs in several member blocks, and uses GraphBLAS
extraction/concatenation/assignment for data movement. Those costs are part of
the requested comparison.

## Validation

Validation receipts and final results are recorded below after the frozen
snapshot is checked. The tests include field mismatches, repeated labels and
destinations, recursion, empty relations, multiple sources, and a new case with
three permuted continuation states and graph vertices at both block boundaries.

The following table is derived from the saved successful receipts; their
tool/source and artifact hashes were rechecked when writing this table.

| Check | Outcome | Receipt |
| --- | --- | --- |
| Literal variant: deterministic + 200 oracle cases + 10 existing tests | PASS | [receipt](runs/2026-11-17_20-04-00-592232_literal-blocks/oracle-existing.result.json) |
| Loaded shared-library smoke | PASS | [receipt](runs/2026-11-17_20-04-00-592232_literal-blocks/loaded-smoke.result.json) |
| All 15 reduced datasets | PASS | [receipt](runs/2026-11-17_20-04-00-592232_literal-blocks/reduced15.result.json) |
| Allocation-failure budgets 0..1134; first success 1130 | PASS | [receipt](runs/2026-11-17_20-04-00-592232_literal-blocks/allocation-sweep.result.json) |
| Tiny profile and result agreement | PASS | [receipt](runs/2026-11-17_20-04-50-823762_literal-blocks-profile/command.result.json) |
| Baseline control: updated oracle/existing suite | PASS | [receipt](runs/2026-11-17_20-06-35-594193_literal-blocks-control/oracle-existing.result.json) |
| P1 reviewed control: updated oracle/existing suite | PASS | [receipt](runs/2026-11-17_20-06-48-388939_literal-blocks-compact-control/oracle-existing.result.json) |
| 71 Python tooling tests | PASS | [receipt](runs/2026-11-17_20-07-03-920010_tooling-tests/unittest.result.json) |
| Pinned inputs/runtime and all 88 frozen-source patch proofs | PASS | [receipt](runs/2026-11-17_20-07-17-010691_integrity-check/integrity.result.json) |

The literal and both control variants each pass 19,646 checks with zero
failures. Reduced dataset counts use the pinned CNF-reference cache; the
independent generated oracle checks compare exact reachable sets. Smoke and
tiny-profile runs verify loading/instrumentation, not performance.

An ASan/UBSan build of the candidate and test driver also passed the complete
19,646-check suite with leak detection outside the sandbox. The linked
GraphBLAS/LAGraph libraries were not sanitizer-instrumented. The first sanitizer
invocation stopped at the suite's intentional oversized allocation; the rerun
used `ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1`, so that expected
allocation failure returns NULL. The sandbox then prevented LeakSanitizer's
thread scan; the same check completed successfully outside it. No OpenMP
environment variables or GraphBLAS thread counts were changed.

The comparison command was also checked with `--dry-run`. The existing full
campaign remains separate; these receipts do not claim full performance
coverage for `p1_blocks` or repair unrelated stale campaign checks.
