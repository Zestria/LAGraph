**I would start with proposal 1.** It targets Java’s per-terminal work while keeping the current frontier/visited layout.

I read both documents first, inspected the reference’s matrix helpers and the RSM loader, and checked the installed Java inputs. No code changed and no benchmarks ran. The impact estimates below are structural predictions, not measured speedups.

The [Java RSM](/home/hollow/repo/CFG_bench/data/grammars/java_points_to.rsm:7) expands to **`Q = 4k + 7` states and `T = 4k + 4` terminals**:

| Input | Field indices `k` | States `Q` | Terminals `T` |
|---|---:|---:|---:|
| Avrora | 858 | 3,439 | 3,436 |
| Batik | 1,006 | 4,031 | 4,028 |

These are sparse matrices: reducing logical dimensions does **not** imply a proportional reduction in allocated memory or runtime.

| Rank | Proposal | Effort | Expected benefit |
|---|---|---|---|
| 1 | Group terminal transfers using compact state–vertex coordinates | Medium | Highest Java runtime potential |
| 2 | Remove unusable indexed branches, then compact states and terminals | Small–medium | Modest runtime gain; definite size reduction |
| 3 | Give each return-prefix matrix only its actual destination states | Medium | Smaller intermediate dimensions; runtime gain uncertain |
| 4 | Fuse nonterminal returns with single terminal continuations | High | Removes nearly half the Java frontier states; substantial correctness work |

1. **Group terminal transfers using compact state–vertex coordinates.**

   **Reference origin:** [`get_new_adj_matrices` and `get_new_rules`](/home/hollow/repo/LAGraph/experimental/algorithm/LAGraph_CFL_reachability_multsrc_advanced.c:472) preserve indexed families instead of executing exploded rules individually. The [binary-rule loop](/home/hollow/repo/LAGraph/experimental/algorithm/LAGraph_CFL_reachability_multsrc_advanced.c:900) preserves block identity through products.

   **Concrete change:** Replace the per-label terminal loop with grouped operations for transitions sharing a source or destination state. For `0 --load_i--> p_i`, construct
   `H[v, id(i,w)] = load_i[v,w]`, where `id(i,w)` exists only for observed label–destination pairs. Extract the frontier at state `0` once, multiply by `H` once, then decode each output column into `(p_i,w)` and scatter into the existing accumulator.

   For `q_i --store_i--> 0`, use the dual construction: gather frontier entries into columns identifying observed `(q_i,v)` pairs, then multiply by a compact incidence matrix to produce the contribution at state `0`. Apply the same scheme to the reverse families.

   **Expected impact:** A full sweep changes up to `4k` indexed terminal products into four family products, although current activity filtering already skips some. On Avrora, a conventional `kV` axis has **21,184,020 positions**; the compact load-output axis has at most **3,684**, and the store-input axis at most **977**, bounded by their edge counts. This offers the strongest prospect of a large Java speedup through fewer products, shifts, and repeated frontier copies.

   **Correctness risk: medium.** Preserve `(state,vertex)` identity until routing finishes. Unioning all loads and stores separately would incorrectly admit `load_i … store_j`. Topology-based grouping can work with the existing API without recovering terminal names. This differs from DS-31’s per-label eligibility checks and from simply allocating a wide block matrix.

2. **Remove unusable indexed branches before allocating the working matrices.**

   **Reference origin:** This is a derived extension of [`get_new_symbols_map` / `get_new_rules`](/home/hollow/repo/LAGraph/experimental/algorithm/LAGraph_CFL_reachability_multsrc_advanced.c:441) and the empty-operand principle in [`matrix_mxm_empty`](/home/hollow/repo/LAGraph/experimental/algorithm/LAGraph_CFL_optimized_matrix_opt.c:780). The reference does not itself implement this RSM trimming pass.

   **Concrete change:** After existing input validation, build a control graph using nonempty terminal relations and conservatively treating nonterminal calls as possible transitions. Remove states outside start-to-final control paths, then construct compact internal state/terminal maps and remap transitions, starts, and finals before allocating `M`, `P`, scratch, and `W`.

   The Java path `load_i → Alias → store_i` cannot contribute when either terminal relation is empty. Existing static terminal filtering skips individual operations but leaves the rest of that branch and its state numbering intact.

   **Expected impact:** Static input inspection found:

   | Input | Fields with both load and store | Resulting `Q` | State reduction |
   |---|---:|---:|---:|
   | Avrora | 708 | 2,839 | 17.4% |
   | Batik | 948 | 3,799 | 5.8% |

   Forward/reverse field supports agree on these inputs. Expect a modest runtime improvement, potentially less than those percentages because existing filters already suppress much dead work. Simple index renumbering alone would save nothing: both overall index domains are already dense.

   **Correctness risk: low–medium.** Preserve nullable paths and final states. Initially treat calls conservatively rather than attempting recursive productivity analysis. Validate the original input before trimming so malformed-input behavior remains intact.

3. **Compact each `W[A]` around actual return destinations.**

   **Reference origin:** The reference chooses [rule-specific intermediate shapes](/home/hollow/repo/LAGraph/experimental/algorithm/LAGraph_CFL_reachability_multsrc_advanced.c:900), and [`matrix_mxm_block`](/home/hollow/repo/LAGraph/experimental/algorithm/LAGraph_CFL_optimized_matrix_opt.c:898) handles a block vector multiplied by a shared ordinary matrix without replicating that ordinary operand.

   **Concrete change:** For each nonterminal `A`, enumerate the distinct destination states of `rsm_nonterm[A]`. If there are `d_A`, build a `Q × d_A` routing matrix and change:
   - shifted nonterminal scratch from `QV × V` to `d_A V × V`;
   - `W[A]` to the same compact shape;
   - result accumulation to scatter through a local-to-global destination map.

   Keep incremental `W` updates and fresh-summary propagation exactly as currently implemented.

   **Expected impact:** Java has `d_PointsTo = 1`, `d_FlowsTo = 1`, and `d_Alias = 2k`. Across the three `W` matrices, the logical state-block count falls from `3Q` to `2k + 2`: **10,317 → 1,718 on Avrora**, about sixfold. Actual entries are mostly unchanged, so expect smaller, workload-dependent runtime savings; scatter overhead could cancel them.

   **Correctness risk: low–medium.** This is an exact coordinate change. The main hazards are destination-map mistakes and combining distinct continuation states. It preserves the current global configuration layout.

4. **Fuse `Alias` returns with their single terminal continuation.**

   **Reference origin:** The reference’s [binary-rule evaluation](/home/hollow/repo/LAGraph/experimental/algorithm/LAGraph_CFL_reachability_multsrc_advanced.c:904) keeps the intermediate product in temporary matrices and immediately consumes it in the second product.

   **Concrete change:** Recognize nonfinal, nonstart states with a single terminal exit. For Java:
   `p_i --Alias--> q_i --store_i--> 0`
   becomes a waiting continuation carrying `(field i, store_i, destination 0)`.

   When a new waiting prefix or fresh `Alias` summary arrives, compute its return contribution and immediately apply `store_i`; insert only the result at state `0`. Retain field identity in waiting records, but eliminate `q_i` from persistent frontier/visited configurations. Handle the reverse chain identically.

   **Expected impact:** Java’s persistent frontier state count becomes `2k + 7`: **3,439 → 1,723 on Avrora**, almost half. It also avoids scheduling each `q_i` intermediate through another frontier iteration. Waiting-prefix storage remains necessary, so total memory will not halve. Best pursued after proposal 1 makes grouped continuation products efficient.

   **Correctness risk: high.** Both event paths must remain complete: new waiting prefixes against existing summaries, and existing prefixes against new summaries. Recursion, nullable callees, and field matching need particular attention.

One scope constraint matters: the [loader already expands indexed states](/home/hollow/repo/CFG_bench/src/rsm_file.c:146), and the [adapter materializes per-terminal matrices](/home/hollow/repo/CFG_bench/src/adapters/adapter_CFL_CFPQ_RSM.c:146) before calling this kernel. These proposals can use private compact representations inside the current API; avoiding those initial allocations altogether would additionally require loader/adapter changes. Adapter preparation is outside the recorded kernel timer.

For whichever proposal you select, I would reuse the documented oracle/smoke harness and reduced-data correctness checks before a bounded `run.py` comparison. The existing indexed-family tests should also be extended with an explicit two-field recursive mismatch case for proposals 1 and 4.
