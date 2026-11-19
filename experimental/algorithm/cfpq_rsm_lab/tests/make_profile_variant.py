#!/usr/bin/env python3
"""Generate a profiling variant of the current RSM or corrected DS-01 reference.

This generator adds only `#ifdef CFPQ_RSM_PROFILE` hooks to the supplied source.
Compiling without that macro reproduces the input algorithm. The registered baseline and all active standalone/cumulative snapshots are supported.
The generator asserts every anchor occurs exactly once, so reference changes
cannot silently produce a stale profile variant.

R2 timing model (see cfpq_profile.h):
  * separate phase, sub-operation, conversion and diagnostic-I/O starts;
  * phase times are API elapsed time; each phase is followed by an explicit
    GrB wait completion boundary whose cost is recorded separately;
  * total_s spans entry to after kernel cleanup; kernel_s subtracts diagnostic
    I/O; setup/cleanup/unclassified buckets reconcile with the same scope.

Usage: make_profile_variant.py <reference.c> <profile.c>
"""

from pathlib import Path
import sys

EDITS: list[tuple[str, str]] = [
    # ------------------------------------------------------------------
    # helpers: conversion timing and complete clear/reshape counts
    # ------------------------------------------------------------------
    (
        "    H_TRY(GrB_mxm(T, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, A, M, GrB_DESC_T0));\n"
        "    H_TRY(GxB_Matrix_reshape(T, false, Q * V, V, GrB_NULL));",
        "    H_TRY(GrB_mxm(T, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, A, M, GrB_DESC_T0));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    double prof_shift_t = LAGraph_WallClockTime();\n"
        "#endif\n"
        "    H_TRY(GxB_Matrix_reshape(T, false, Q * V, V, GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    g_prof.conversion_s += LAGraph_WallClockTime() - prof_shift_t;\n"
        "    g_prof.reshapes++;\n"
        "    GrB_Index prof_scratch_n = 0;\n"
        "    if (GrB_Matrix_nvals(&prof_scratch_n, T) == GrB_SUCCESS &&\n"
        "        prof_scratch_n > g_prof.peak_scratch)\n"
        "        g_prof.peak_scratch = prof_scratch_n;\n"
        "#endif",
    ),
    (
        "    H_TRY(GrB_Matrix_clear(T));\n"
        "    H_TRY(GxB_Matrix_reshape(T, false, Q, V * V, GrB_NULL));",
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    double prof_unshift_t = LAGraph_WallClockTime();\n"
        "    g_prof.clears++;\n"
        "#endif\n"
        "    H_TRY(GrB_Matrix_clear(T));\n"
        "    H_TRY(GxB_Matrix_reshape(T, false, Q, V * V, GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    g_prof.conversion_s += LAGraph_WallClockTime() - prof_unshift_t;\n"
        "    g_prof.reshapes++;\n"
        "#endif",
    ),
    (
        "    H_TRY(GrB_Matrix_clear(T));\n"
        "    H_TRY(GrB_mxm(T, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, A, B, GrB_NULL));",
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    g_prof.clears++;\n"
        "#endif\n"
        "    H_TRY(GrB_Matrix_clear(T));\n"
        "    H_TRY(GrB_mxm(T, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, A, B, GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    {\n"
        "        GrB_Index prof_st_n = 0;\n"
        "        if (GrB_Matrix_nvals(&prof_st_n, T) == GrB_SUCCESS && prof_st_n > g_prof.peak_s_temp)\n"
        "            g_prof.peak_s_temp = prof_st_n;\n"
        "    }\n"
        "#endif",
    ),
    # ------------------------------------------------------------------
    # includes and the single static profile object
    # ------------------------------------------------------------------
    (
        "#include <string.h>\n#define H_TRY(expr)",
        "#include <string.h>\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        '#include "cfpq_profile.h"\n'
        "static cfpq_prof g_prof;\n"
        "#define CFPQ_PROF_ABORT() cfpq_prof_abort(&g_prof)\n"
        "#define CFPQ_PROF_FREE_WORK() do { GrB_free(&M_union); GrB_free(&M_union_keep); \\\n"
        "        GrB_free(&X_fresh); GrB_free(&X_keep); LAGraph_Free((void **)&w_touched, NULL); \\\n"
        "        LAGraph_Free((void **)&gnt_touched, NULL); } while (0)\n"
        "#else\n"
        "#define CFPQ_PROF_ABORT() ((void)0)\n"
        "#define CFPQ_PROF_FREE_WORK() ((void)0)\n"
        "#endif\n"
        "#define H_TRY(expr)",
    ),
    # ------------------------------------------------------------------
    # cleanup: free profile-only objects, abort diagnostics on error
    # ------------------------------------------------------------------
    (
        "        FREE_GRB_MATRIX_ARRAY(graph_nt, num_nonterm);                                              \\\n"
        "        FREE_GRB_MATRIX_ARRAY(W, num_nonterm);                                                     \\\n"
        "        FREE_GRB_MATRIX_ARRAY(rsm_start, num_nonterm);                                             \\\n"
        "    }",
        "        FREE_GRB_MATRIX_ARRAY(graph_nt, num_nonterm);                                              \\\n"
        "        FREE_GRB_MATRIX_ARRAY(W, num_nonterm);                                                     \\\n"
        "        FREE_GRB_MATRIX_ARRAY(rsm_start, num_nonterm);                                             \\\n"
        "        CFPQ_PROF_FREE_WORK();                                                                     \\\n"
        "    }",
    ),
    (
        "        if (reachable != NULL)                                                                     \\\n"
        "            GrB_free(reachable);                                                                   \\\n"
        "    }",
        "        if (reachable != NULL)                                                                     \\\n"
        "            GrB_free(reachable);                                                                   \\\n"
        "        CFPQ_PROF_ABORT();                                                                         \\\n"
        "    }",
    ),
    # ------------------------------------------------------------------
    # profile state at entry and profile-only working objects
    # ------------------------------------------------------------------
    (
        "    LG_CLEAR_MSG;\n\n    // working matrices / vectors",
        "    LG_CLEAR_MSG;\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    cfpq_prof *prof = &g_prof;\n"
        "    cfpq_prof_init(prof);\n"
        "    double prof_call_t0 = LAGraph_WallClockTime();\n"
        "    double prof_phase_t = 0.0;\n"
        "    double prof_sub_t = 0.0;\n"
        "    double prof_conv_t = 0.0;\n"
        "    double prof_box_t = 0.0;\n"
        "    double prof_conv0 = 0.0;\n"
        "    double prof_io_t = 0.0;\n"
        "    uint64_t prof_cand = 0;\n"
        "    uint64_t prof_cand_in_p = 0;\n"
        "    uint64_t prof_next = 0;\n"
        "    uint64_t prof_p_before = 0;\n"
        "    uint64_t prof_p_after = 0;\n"
        "#endif\n\n    // working matrices / vectors",
    ),
    (
        "    GrB_Vector rowvec = GrB_NULL; // length-Q row activity of M\n",
        "    GrB_Vector rowvec = GrB_NULL; // length-Q row activity of M\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    GrB_Matrix M_union = GrB_NULL;      // diagnostic: acc|M_call before subtraction\n"
        "    GrB_Matrix M_union_keep = GrB_NULL; // diagnostic: M_union intersect P\n"
        "    GrB_Matrix X_fresh = GrB_NULL;      // diagnostic: X \\ W_old\n"
        "    GrB_Matrix X_keep = GrB_NULL;       // diagnostic: X intersect W_old\n"
        "    bool *w_touched = NULL;\n"
        "    bool *gnt_touched = NULL;\n"
        "#endif\n",
    ),
    (
        "    GRB_TRY(GrB_Vector_new(&source_mask, GrB_BOOL, V));\n",
        "    GRB_TRY(GrB_Vector_new(&source_mask, GrB_BOOL, V));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    GRB_TRY(GrB_Matrix_new(&M_union, GrB_BOOL, Q, V * V));\n"
        "    GRB_TRY(GrB_Matrix_new(&M_union_keep, GrB_BOOL, Q, V * V));\n"
        "    GRB_TRY(GrB_Matrix_new(&X_fresh, GrB_BOOL, Q * V, V));\n"
        "    GRB_TRY(GrB_Matrix_new(&X_keep, GrB_BOOL, Q * V, V));\n"
        "    LG_TRY(LAGraph_Calloc((void **)&w_touched, num_nonterm, sizeof(bool), msg));\n"
        "    LG_TRY(LAGraph_Calloc((void **)&gnt_touched, num_nonterm, sizeof(bool), msg));\n"
        "#endif\n",
    ),
    # ------------------------------------------------------------------
    # call shape and setup timing (total spans the whole call)
    # ------------------------------------------------------------------
    (
        "        for (GrB_Index i = 0; i < Q; ++i)\n            active[i] = false;\n    }\n\n    // each source s starts a call",
        "        for (GrB_Index i = 0; i < Q; ++i)\n            active[i] = false;\n    }\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    prof->q = Q;\n"
        "    prof->v = V;\n"
        "    prof->nterm = num_term;\n"
        "    prof->nnonterm = num_nonterm;\n"
        "    prof->nsources = (uint64_t)num_sources;\n"
        "    for (size_t pi = 0; pi < num_nonterm; ++pi)\n"
        "    {\n"
        "        if (rsm_nonterm[pi] != GrB_NULL)\n"
        "            prof->nboxes_called++;\n"
        "        if (final_states[pi] != GrB_NULL)\n"
        "            prof->nboxes_final++;\n"
        "    }\n"
        "#endif\n\n    // each source s starts a call",
    ),
    (
        "    GRB_TRY(GrB_Matrix_dup(&P, M));\n\n    GrB_Index m_nvals = 0;",
        "    GRB_TRY(GrB_Matrix_dup(&P, M));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    prof->setup_s = cfpq_prof_now() - prof_call_t0;\n"
        "    prof->seed_entries = (uint64_t)num_sources;\n"
        "#endif\n\n    GrB_Index m_nvals = 0;",
    ),
    # ------------------------------------------------------------------
    # iteration boundary: incoming frontier and visited sets
    # ------------------------------------------------------------------
    (
        "    while (m_nvals > 0)\n    {\n        // G=2: mark rows of M",
        "    while (m_nvals > 0)\n    {\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof->iters++;\n"
        "        prof->sum_m += m_nvals;\n"
        "        if (m_nvals > prof->max_m)\n"
        "            prof->max_m = m_nvals;\n"
        "        if (m_nvals > prof->peak_m)\n"
        "            prof->peak_m = m_nvals;\n"
        "        {\n"
        "            GrB_Index prof_pn = 0;\n"
        "            GRB_TRY(GrB_Matrix_nvals(&prof_pn, P));\n"
        "            prof_p_before = prof_pn;\n"
        "        }\n"
        "        prof_cand = 0;\n"
        "        prof_cand_in_p = 0;\n"
        "        prof_next = 0;\n"
        "        for (size_t pi = 0; pi < num_nonterm; ++pi)\n"
        "        {\n"
        "            w_touched[pi] = false;\n"
        "            gnt_touched[pi] = false;\n"
        "        }\n"
        "        prof_phase_t = cfpq_prof_now();\n"
        "#endif\n        // G=2: mark rows of M",
    ),
    # activity prediction: count the vector clear and time the block
    (
        "            GRB_TRY(GrB_Vector_clear(rowvec));\n",
        "            GRB_TRY(GrB_Vector_clear(rowvec));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            prof->clears++;\n"
        "            prof_phase_t = cfpq_prof_now();\n"
        "#endif\n",
    ),
    (
        "            for (GrB_Index k = 0; k < nactive2; ++k)\n                active[active_idx[k]] = true;\n        }\n\n        // Phase 1:",
        "            for (GrB_Index k = 0; k < nactive2; ++k)\n                active[active_idx[k]] = true;\n        }\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof->activity_s += cfpq_prof_now() - prof_phase_t;\n"
        "        prof_phase_t = cfpq_prof_now();\n"
        "        prof_conv0 = prof->conversion_s;\n"
        "#endif\n\n        // Phase 1:",
    ),
    # ------------------------------------------------------------------
    # phase 1
    # ------------------------------------------------------------------
    (
        "            if (!term_active[i])\n                continue;\n",
        "            if (!term_active[i])\n            {\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                prof->term_static_skip++;\n"
        "#endif\n"
        "                continue;\n            }\n",
    ),
    (
        "            if (!predicted)\n                continue;\n",
        "            if (!predicted)\n            {\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                prof->term_pred_skip++;\n"
        "#endif\n"
        "                continue;\n            }\n",
    ),
    (
        "            LG_TRY(s_shift(scratch, rsm_term[i], M, Q, V));\n",
        "            LG_TRY(s_shift(scratch, rsm_term[i], M, Q, V));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            prof->term_shift++;\n"
        "#endif\n",
    ),
    (
        "            if (tn > 0)\n                LG_TRY(s_mul_acc(acc, scratch, graph_term[i], s_temp));\n",
        "            if (tn > 0)\n                LG_TRY(s_mul_acc(acc, scratch, graph_term[i], s_temp));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            if (tn > 0)\n"
        "                prof->term_product++;\n"
        "            else\n"
        "                prof->term_skip_product++;\n"
        "#endif\n",
    ),
    (
        "            LG_TRY(s_unshift(scratch, Q, V));\n        }\n\n        // Phases 2+3:",
        "            LG_TRY(s_unshift(scratch, Q, V));\n        }\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof->phase1_conv_s += prof->conversion_s - prof_conv0;\n"
        "        prof->phase1_s += cfpq_prof_now() - prof_phase_t;\n"
        "        prof_io_t = cfpq_prof_now();\n"
        "        GRB_TRY(GrB_Matrix_wait(acc, GrB_MATERIALIZE));\n"
        "        {\n"
        "            GrB_Index prof_acc_n = 0;\n"
        "            GRB_TRY(GrB_Matrix_nvals(&prof_acc_n, acc));\n"
        "            if (prof_acc_n > prof->peak_acc)\n"
        "                prof->peak_acc = prof_acc_n;\n"
        "        }\n"
        "        prof->boundary_p1_s += cfpq_prof_now() - prof_io_t;\n"
        "#endif\n\n        // Phases 2+3:",
    ),
    # ------------------------------------------------------------------
    # phases 2+3
    # ------------------------------------------------------------------
    (
        "        GRB_TRY(GrB_Matrix_clear(M_call));\n",
        "        GRB_TRY(GrB_Matrix_clear(M_call));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof->clears++;\n"
        "        prof_phase_t = cfpq_prof_now();\n"
        "        prof_conv0 = prof->conversion_s;\n"
        "#endif\n",
    ),
    (
        "            LG_TRY(s_shift(scratch, rsm_nonterm[i], M, Q, V));\n",
        "            LG_TRY(s_shift(scratch, rsm_nonterm[i], M, Q, V));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            prof->nt_shift++;\n"
        "#endif\n",
    ),
    (
        "            if (x_nvals > 0)\n            {\n                // call: every vertex v where X is nonzero enters nonterminal i",
        "            if (x_nvals > 0)\n            {\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                if (W[i] != GrB_NULL)\n"
        "                {\n"
        "                    double prof_x_t = cfpq_prof_now();\n"
        "                    GrB_Index prof_fresh_n = 0, prof_over_n = 0;\n"
        "                    GrB_Matrix_clear(X_fresh);\n"
        "                    GRB_TRY(GrB_apply(X_fresh, W[i], GrB_NULL, GrB_IDENTITY_BOOL, scratch,\n"
        "                                      GrB_DESC_RSC));\n"
        "                    GRB_TRY(GrB_Matrix_nvals(&prof_fresh_n, X_fresh));\n"
        "                    GrB_Matrix_clear(X_keep);\n"
        "                    GRB_TRY(GrB_apply(X_keep, W[i], GrB_NULL, GrB_IDENTITY_BOOL, scratch,\n"
        "                                      GrB_DESC_RS));\n"
        "                    GRB_TRY(GrB_Matrix_nvals(&prof_over_n, X_keep));\n"
        "                    prof->sum_x += x_nvals;\n"
        "                    prof->sum_x_fresh += prof_fresh_n;\n"
        "                    prof->sum_x_overlap += prof_over_n;\n"
        "                    if (i < CFPQ_PROF_MAX_BOX)\n"
        "                    {\n"
        "                        prof->box_x[i] += x_nvals;\n"
        "                        prof->box_fresh[i] += prof_fresh_n;\n"
        "                        prof->box_overlap[i] += prof_over_n;\n"
        "                    }\n"
        "                    else\n"
        "                        prof->detail_truncated = true;\n"
        "                    prof->diag_x_s += cfpq_prof_now() - prof_x_t;\n"
        "                }\n"
        "                prof_sub_t = cfpq_prof_now();\n"
        "                prof->nt_call++;\n"
        "                if (i < CFPQ_PROF_MAX_BOX)\n"
        "                    prof->box_calls[i]++;\n"
        "                else\n"
        "                    prof->detail_truncated = true;\n"
        "#endif\n                // call: every vertex v where X is nonzero enters nonterminal i",
    ),
    (
        "                                rsm_start[i], diag_entry, GrB_NULL));\n                GRB_TRY(GrB_free(&diag_entry));\n",
        "                                rsm_start[i], diag_entry, GrB_NULL));\n                GRB_TRY(GrB_free(&diag_entry));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                prof->callconstr_s += cfpq_prof_now() - prof_sub_t;\n"
        "#endif\n",
    ),
    (
        "                if (gnt_nvals > 0)\n                    LG_TRY(s_mul_acc(acc, scratch, graph_nt[i], s_temp));\n",
        "                if (gnt_nvals > 0)\n                    LG_TRY(s_mul_acc(acc, scratch, graph_nt[i], s_temp));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                if (gnt_nvals > 0)\n"
        "                    prof->nt_skip_product++;\n"
        "#endif\n",
    ),
    (
        "                // RETURN bookkeeping: W[i] == reshape(R_i' * P)\n                GRB_TRY(GrB_eWiseAdd(W[i], GrB_NULL, GrB_NULL, GrB_LOR, W[i], scratch, GrB_NULL));\n",
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                prof_sub_t = cfpq_prof_now();\n"
        "#endif\n"
        "                // RETURN bookkeeping: W[i] == reshape(R_i' * P)\n"
        "                GRB_TRY(GrB_eWiseAdd(W[i], GrB_NULL, GrB_NULL, GrB_LOR, W[i], scratch, GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                prof->wupdate_s += cfpq_prof_now() - prof_sub_t;\n"
        "                prof->w_updates++;\n"
        "                w_touched[i] = true;\n"
        "                if (i < CFPQ_PROF_MAX_BOX)\n"
        "                {\n"
        "                    GrB_Index prof_wn = 0;\n"
        "                    GRB_TRY(GrB_Matrix_nvals(&prof_wn, W[i]));\n"
        "                    if (prof_wn > prof->box_wmax[i])\n"
        "                        prof->box_wmax[i] = prof_wn;\n"
        "                }\n"
        "#endif\n",
    ),
    (
        "            LG_TRY(s_unshift(scratch, Q, V));\n        }\n\n        // Phase 4:",
        "            LG_TRY(s_unshift(scratch, Q, V));\n        }\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof->phase2_3_conv_s += prof->conversion_s - prof_conv0;\n"
        "        prof->phase2_3_s += cfpq_prof_now() - prof_phase_t;\n"
        "        prof_io_t = cfpq_prof_now();\n"
        "        GRB_TRY(GrB_Matrix_wait(acc, GrB_MATERIALIZE));\n"
        "        GRB_TRY(GrB_Matrix_wait(M_call, GrB_MATERIALIZE));\n"
        "        for (size_t pi = 0; pi < num_nonterm; ++pi)\n"
        "            if (w_touched[pi])\n"
        "                GRB_TRY(GrB_Matrix_wait(W[pi], GrB_MATERIALIZE));\n"
        "        {\n"
        "            GrB_Index prof_acc_n = 0;\n"
        "            GRB_TRY(GrB_Matrix_nvals(&prof_acc_n, acc));\n"
        "            if (prof_acc_n > prof->peak_acc)\n"
        "                prof->peak_acc = prof_acc_n;\n"
        "        }\n"
        "        prof->boundary_p2_3_s += cfpq_prof_now() - prof_io_t;\n"
        "#endif\n\n        // Phase 4:",
    ),
    # ------------------------------------------------------------------
    # phase 4
    # ------------------------------------------------------------------
    (
        "        // Phase 4: return transitions.",
        "#ifdef CFPQ_RSM_PROFILE\n        prof_phase_t = cfpq_prof_now();\n        prof_conv0 = prof->conversion_s;\n#endif\n        // Phase 4: return transitions.",
    ),
    (
        "            if (final_states[i] == GrB_NULL)\n                continue;\n",
        "            if (final_states[i] == GrB_NULL)\n                continue;\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            prof_box_t = cfpq_prof_now();\n"
        "#endif\n",
    ),
    (
        "            GRB_TRY(GrB_mxm(new_edges, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL,\n                            (GrB_Matrix)final_states[i], M, GrB_DESC_T0));\n",
        "            GRB_TRY(GrB_mxm(new_edges, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL,\n"
        "                            (GrB_Matrix)final_states[i], M, GrB_DESC_T0));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            {\n"
        "                GrB_Index prof_ne_n = 0;\n"
        "                GRB_TRY(GrB_Matrix_nvals(&prof_ne_n, new_edges));\n"
        "                if (prof_ne_n > prof->peak_new_edges)\n"
        "                    prof->peak_new_edges = prof_ne_n;\n"
        "            }\n"
        "#endif\n",
    ),
    (
        "            GRB_TRY(GxB_Matrix_reshape(new_edges, false, V, V, GrB_NULL));\n",
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            prof_conv_t = cfpq_prof_now();\n"
        "#endif\n"
        "            GRB_TRY(GxB_Matrix_reshape(new_edges, false, V, V, GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            prof->conversion_s += cfpq_prof_now() - prof_conv_t;\n"
        "#endif\n",
    ),
    (
        "            GrB_Index ne_vals = 0;\n            GRB_TRY(GrB_Matrix_nvals(&ne_vals, new_edges));\n            if (ne_vals == 0)\n                continue;\n",
        "            GrB_Index ne_vals = 0;\n            GRB_TRY(GrB_Matrix_nvals(&ne_vals, new_edges));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            if (ne_vals > 0)\n"
        "                prof->ret_extract++;\n"
        "#endif\n"
        "            if (ne_vals == 0)\n                continue;\n",
    ),
    (
        "                    GRB_TRY(GxB_Matrix_reshape(new_edges, false, 1, V*V, GrB_NULL));\n                    continue;",
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                    prof_conv_t = cfpq_prof_now();\n"
        "#endif\n"
        "                    GRB_TRY(GxB_Matrix_reshape(new_edges, false, 1, V*V, GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                    prof->conversion_s += cfpq_prof_now() - prof_conv_t;\n"
        "#endif\n                    continue;",
    ),
    (
        "            GRB_TRY(GrB_eWiseAdd(graph_nt[i], GrB_NULL, GrB_NULL, GrB_LOR, graph_nt[i], new_edges,\n                                 GrB_NULL));\n",
        "            GRB_TRY(GrB_eWiseAdd(graph_nt[i], GrB_NULL, GrB_NULL, GrB_LOR, graph_nt[i], new_edges,\n                                 GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            prof->summary_unions++;\n"
        "            prof->ret_dg++;\n"
        "            gnt_touched[i] = true;\n"
        "            if (i < CFPQ_PROF_MAX_BOX)\n"
        "            {\n"
        "                prof->box_dg[i] += ne_vals;\n"
        "                GrB_Index prof_gn = 0;\n"
        "                GRB_TRY(GrB_Matrix_nvals(&prof_gn, graph_nt[i]));\n"
        "                if (prof_gn > prof->box_gntmax[i])\n"
        "                    prof->box_gntmax[i] = prof_gn;\n"
        "            }\n"
        "            else\n"
        "                prof->detail_truncated = true;\n"
        "            prof->summary_s += cfpq_prof_now() - prof_box_t;\n"
        "#endif\n",
    ),
    (
        "            if (W[i] != GrB_NULL)\n            {\n                GrB_Index w_nvals = 0;\n                GRB_TRY(GrB_Matrix_nvals(&w_nvals, W[i]));\n                if (w_nvals > 0)\n                    LG_TRY(s_mul_acc(acc, W[i], new_edges, s_temp));\n            }\n",
        "            if (W[i] != GrB_NULL)\n            {\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                prof_sub_t = cfpq_prof_now();\n"
        "#endif\n"
        "                GrB_Index w_nvals = 0;\n                GRB_TRY(GrB_Matrix_nvals(&w_nvals, W[i]));\n                if (w_nvals > 0)\n                    LG_TRY(s_mul_acc(acc, W[i], new_edges, s_temp));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "                if (w_nvals > 0)\n"
        "                    prof->ret_product++;\n"
        "                prof->retprod_s += cfpq_prof_now() - prof_sub_t;\n"
        "#endif\n            }\n",
    ),
    (
        "            GRB_TRY(GxB_Matrix_reshape(new_edges, false, 1, V * V, GrB_NULL));\n        }\n\n        // Phase 5:",
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            prof_conv_t = cfpq_prof_now();\n"
        "#endif\n"
        "            GRB_TRY(GxB_Matrix_reshape(new_edges, false, 1, V * V, GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "            prof->conversion_s += cfpq_prof_now() - prof_conv_t;\n"
        "#endif\n        }\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof->phase4_conv_s += prof->conversion_s - prof_conv0;\n"
        "        prof->phase4_s += cfpq_prof_now() - prof_phase_t;\n"
        "        prof_io_t = cfpq_prof_now();\n"
        "        GRB_TRY(GrB_Matrix_wait(acc, GrB_MATERIALIZE));\n"
        "        for (size_t pi = 0; pi < num_nonterm; ++pi)\n"
        "            if (gnt_touched[pi])\n"
        "                GRB_TRY(GrB_Matrix_wait(graph_nt[pi], GrB_MATERIALIZE));\n"
        "        prof->boundary_p4_s += cfpq_prof_now() - prof_io_t;\n"
        "#endif\n\n        // Phase 5:",
    ),
    # ------------------------------------------------------------------
    # phase 5
    # ------------------------------------------------------------------
    (
        "        // Phase 5: M_new = (reshape(acc) | M_call) \\ P.  A structural visited",
        "#ifdef CFPQ_RSM_PROFILE\n        prof_phase_t = cfpq_prof_now();\n        prof_conv0 = prof->conversion_s;\n#endif\n        // Phase 5: M_new = (reshape(acc) | M_call) \\ P.  A structural visited",
    ),
    (
        "        GRB_TRY(GxB_Matrix_reshapeDup(&M_new, acc, false, Q, V * V, GrB_NULL));\n",
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof_conv_t = cfpq_prof_now();\n"
        "#endif\n"
        "        GRB_TRY(GxB_Matrix_reshapeDup(&M_new, acc, false, Q, V * V, GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof->conversion_s += cfpq_prof_now() - prof_conv_t;\n"
        "        {\n"
        "            double prof_u_t = cfpq_prof_now();\n"
        "            GRB_TRY(GrB_eWiseAdd(M_union, GrB_NULL, GrB_NULL, GrB_LOR, M_new, M_call, GrB_NULL));\n"
        "            {\n"
        "                GrB_Index prof_u = 0;\n"
        "                GRB_TRY(GrB_Matrix_nvals(&prof_u, M_union));\n"
        "                prof_cand = prof_u;\n"
        "                prof->sum_cand += prof_u;\n"
        "                if (prof_u > prof->max_cand)\n"
        "                    prof->max_cand = prof_u;\n"
        "                if (prof_u > prof->peak_union)\n"
        "                    prof->peak_union = prof_u;\n"
        "            }\n"
        "            GrB_Matrix_clear(M_union_keep);\n"
        "            GRB_TRY(GrB_apply(M_union_keep, P, GrB_NULL, GrB_IDENTITY_BOOL, M_union,\n"
        "                              GrB_DESC_RS));\n"
        "            {\n"
        "                GrB_Index prof_up = 0;\n"
        "                GRB_TRY(GrB_Matrix_nvals(&prof_up, M_union_keep));\n"
        "                prof_cand_in_p = prof_up;\n"
        "                prof->sum_cand_in_p += prof_up;\n"
        "                if (prof_up > prof->max_cand_in_p)\n"
        "                    prof->max_cand_in_p = prof_up;\n"
        "            }\n"
        "            prof->diag_union_s += cfpq_prof_now() - prof_u_t;\n"
        "        }\n"
        "#endif\n",
    ),
    (
        "        GRB_TRY(GrB_Matrix_clear(acc));\n",
        "        GRB_TRY(GrB_Matrix_clear(acc));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof->clears++;\n"
        "#endif\n",
    ),
    (
        "        GRB_TRY(GrB_eWiseAdd(M_new, P, GrB_NULL, GrB_LOR, M_new, M_call, GrB_DESC_RSC));\n",
        "        GRB_TRY(GrB_eWiseAdd(M_new, P, GrB_NULL, GrB_LOR, M_new, M_call, GrB_DESC_RSC));\n",
    ),
    (
        "        GRB_TRY(GrB_eWiseAdd(P, GrB_NULL, GrB_NULL, GrB_LOR, P, M_new, GrB_NULL));\n",
        "        GRB_TRY(GrB_eWiseAdd(P, GrB_NULL, GrB_NULL, GrB_LOR, P, M_new, GrB_NULL));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "        prof->phase5_conv_s += prof->conversion_s - prof_conv0;\n"
        "        prof->phase5_s += cfpq_prof_now() - prof_phase_t;\n"
        "        prof_io_t = cfpq_prof_now();\n"
        "        GRB_TRY(GrB_Matrix_wait(M_new, GrB_MATERIALIZE));\n"
        "        GRB_TRY(GrB_Matrix_wait(P, GrB_MATERIALIZE));\n"
        "        {\n"
        "            GrB_Index prof_mnew = 0;\n"
        "            GRB_TRY(GrB_Matrix_nvals(&prof_mnew, M_new));\n"
        "            prof_next = prof_mnew;\n"
        "            prof->sum_next += prof_mnew;\n"
        "            if (prof_mnew > prof->max_next)\n"
        "                prof->max_next = prof_mnew;\n"
        "        }\n"
        "        {\n"
        "            GrB_Index prof_p = 0;\n"
        "            GRB_TRY(GrB_Matrix_nvals(&prof_p, P));\n"
        "            prof_p_after = prof_p;\n"
        "            prof->sum_p += prof_p;\n"
        "            if (prof_p > prof->max_p)\n"
        "                prof->max_p = prof_p;\n"
        "            if (prof_p > prof->peak_p)\n"
        "                prof->peak_p = prof_p;\n"
        "        }\n"
        "        prof->boundary_p5_s += cfpq_prof_now() - prof_io_t;\n"
        "        if (prof->iters - 1 < CFPQ_PROF_MAX_ITER)\n"
        "        {\n"
        "            size_t pk = (size_t)(prof->iters - 1);\n"
        "            prof->iter_in_m[pk] = m_nvals;\n"
        "            prof->iter_p_before[pk] = prof_p_before;\n"
        "            prof->iter_cand[pk] = prof_cand;\n"
        "            prof->iter_cand_in_p[pk] = prof_cand_in_p;\n"
        "            prof->iter_next[pk] = prof_next;\n"
        "            prof->iter_p_after[pk] = prof_p_after;\n"
        "        }\n"
        "        else\n"
        "            prof->detail_truncated = true;\n"
        "#endif\n",
    ),
    # ------------------------------------------------------------------
    # final extraction, diagnostic snapshot, cleanup, emit
    # ------------------------------------------------------------------
    (
        "    // reachable(w) = OR_s source_mask(s) & graph_nt[start](s, w)\n",
        "#ifdef CFPQ_RSM_PROFILE\n    prof_phase_t = cfpq_prof_now();\n#endif\n"
        "    // reachable(w) = OR_s source_mask(s) & graph_nt[start](s, w)\n",
    ),
    (
        "                    graph_nt[start_nonterm], source_mask, GrB_DESC_T0));\n",
        "                    graph_nt[start_nonterm], source_mask, GrB_DESC_T0));\n"
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    prof->final_s = cfpq_prof_now() - prof_phase_t;\n"
        "    prof_phase_t = cfpq_prof_now();\n"
        "    GRB_TRY(GrB_Vector_wait(*reachable, GrB_MATERIALIZE));\n"
        "    prof->materialize_s = cfpq_prof_now() - prof_phase_t;\n"
        "#endif\n",
    ),
    (
        "    LG_FREE_WORK;\n    return GrB_SUCCESS;",
        "#ifdef CFPQ_RSM_PROFILE\n"
        "    cfpq_prof_snapshot(prof, M, P, acc, scratch, s_temp, new_edges, M_union, X_fresh, X_keep,\n"
        "                       entry_vec, source_mask, rowvec, W, graph_nt, num_nonterm);\n"
        "    prof_phase_t = cfpq_prof_now();\n"
        "    LG_FREE_WORK;\n"
        "    cfpq_prof_emit(prof, cfpq_prof_now() - prof_call_t0, cfpq_prof_now() - prof_phase_t);\n"
        "#else\n"
        "    LG_FREE_WORK;\n"
        "#endif\n    return GrB_SUCCESS;",
    ),
]

HEADER = """//------------------------------------------------------------------------------
// cfpq_rsm_lab/runs/<run>/profile/src: generated profiling variant
//------------------------------------------------------------------------------
//
// Generated from the supplied RSM source by tests/make_profile_variant.py.
// Do not edit by hand; regenerate instead.
// The only additions are CFPQ_RSM_PROFILE hooks.  Compile without that macro
// to reproduce the reference behavior; timing builds use the reference source
// directly and never contain this instrumentation.
//
// Profile runs add diagnostic operations (candidate-union probes, X/W probes,
// explicit completion boundaries) inside the phase timers.  Phase times are API
// elapsed time; boundary waits are recorded separately; total_s spans entry to
// after kernel cleanup.  Profile timings are attribution evidence only, not the
// performance verdict.
//
"""


def profile_edits(source: str) -> list[tuple[str, str]]:
    """Select hooks for the legacy products or optimized tuple-call layout.

    Use shared statement/phase boundaries instead of obsolete comment prose.
    Every selected anchor is still required exactly once by instrument().
    """
    edits = list(EDITS)
    # Cleanup no longer depends on the removed rsm_start array.
    free_w = next((line for line in source.splitlines(keepends=True)
                   if "FREE_GRB_MATRIX_ARRAY(W, num_nonterm);" in line), None)
    if free_w is None:
        raise SystemExit("missing anchor 'legacy W cleanup' (anchor occurs 0 times, expected 1)")
    edits[4] = (free_w, free_w +
                "        CFPQ_PROF_FREE_WORK();                                                                     \\\n")
    for index, anchor in ((9, "    // each source s starts a call"),
                          (13, "        // Phase 1:"),
                          (25, "        // Phase 4:")):
        edits[index] = (anchor, edits[index][1][edits[index][1].index("#ifdef"):])
    old, new = edits[11]
    edits[11] = (old.split("\n        //", 1)[0], new.rsplit("\n        //", 1)[0])
    edits[26] = tuple(s.replace("return transitions.", "return transitions") for s in edits[26])
    old, new = edits[35]
    edits[35] = tuple(s.replace(old, "        // Phase 5: M_new") for s in (old, new))

    # The optimized implementation removed the activity-vector clear.
    if "GrB_Vector_clear(rowvec)" not in source:
        edits[12] = ("", "")

    if "bool acc_empty = true;" in source:
        # All optimized products use pattern-only Boolean multiplication.
        edits = [(old.replace("GrB_LOR_LAND_SEMIRING_BOOL", "GxB_ANY_PAIR_BOOL"),
                  new.replace("GrB_LOR_LAND_SEMIRING_BOOL", "GxB_ANY_PAIR_BOOL"))
                 for old, new in edits]
        for before, after in (
            ("s_mul_acc(acc, scratch, graph_term[i], s_temp)",
             "s_mul_acc(&acc, &s_temp, scratch, graph_term[i], &acc_empty)"),
            ("s_mul_acc(acc, scratch, graph_nt[i], s_temp)",
             "s_mul_acc(&acc, &s_temp, scratch, graph_nt[i], &acc_empty)"),
            ("s_mul_acc(acc, W[i], new_edges, s_temp)",
             "s_mul_acc(&acc, &s_temp, W[i], new_edges, &acc_empty)"),
        ):
            edits = [(old.replace(before, after), new.replace(before, after))
                     for old, new in edits]

        # The product temporary is passed by pointer and can swap with acc.
        product = "    H_TRY(GrB_mxm(*T, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL, A, B, GrB_NULL));"
        hook = edits[2][1][edits[2][1].index("    H_TRY(GrB_mxm"):]
        hook = hook.replace("GrB_mxm(T,", "GrB_mxm(*T,")
        hook = hook.replace("(&prof_st_n, T)", "(&prof_st_n, *T)")
        clear = "    H_TRY(GrB_Matrix_clear(*T));\n"
        if clear in source:
            edits[2] = (clear + product,
                        "#ifdef CFPQ_RSM_PROFILE\n    g_prof.clears++;\n#endif\n" + clear + hook)
        else:
            edits[2] = (product, hook)

        call = "            LG_TRY(s_mul_acc(&acc, &s_temp, scratch, graph_term[i], &acc_empty));\n"
        if "            if (tn > 0)\n" not in source:
            edits[17] = (call, call +
                         "#ifdef CFPQ_RSM_PROFILE\n            prof->term_product++;\n#endif\n")

        # Tuple extraction replaces the old diagonal/reshape/call mxm chain.
        anchor = "                // skip over nonterminal i using the summary edges"
        edits[22] = (anchor,
                     "#ifdef CFPQ_RSM_PROFILE\n"
                     "                prof->callconstr_s += cfpq_prof_now() - prof_sub_t;\n"
                     "#endif\n" + anchor)
        build = ("            GRB_TRY(GrB_Matrix_build_BOOL(M_call, call_i, call_j, call_x, call_count,\n"
                 "                                          GrB_LOR));\n")
        edits.append((build,
                      "#ifdef CFPQ_RSM_PROFILE\n            prof_sub_t = cfpq_prof_now();\n#endif\n" +
                      build + "#ifdef CFPQ_RSM_PROFILE\n"
                      "            prof->callconstr_s += cfpq_prof_now() - prof_sub_t;\n#endif\n"))
    return [(old, new) for old, new in edits if old]


def instrument_legacy(source: str) -> str:
    s = source
    for old, new in profile_edits(source):
        count = s.count(old)
        if count != 1:
            raise SystemExit(f"anchor occurs {count} times, expected 1:\n{old[:240]!r}")
        s = s.replace(old, new)
    # The production file starts with cleanup macros; keep its first line.
    return HEADER + s


# The active family uses semantic phase boundaries and balanced C function
# bodies. Whitespace/comment wording inside a statement is not an anchor.
import re


def _one(pattern: str, source: str, label: str):
    matches = list(re.finditer(pattern, source, re.M | re.S))
    if len(matches) != 1:
        raise SystemExit(f"missing/ambiguous anchor '{label}' (anchor occurs {len(matches)} times, expected 1)")
    return matches[0]


def _mask_c(source: str) -> str:
    # Preserve offsets while hiding comments, strings and character literals.
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: re.sub(r'[^\n]', ' ', m.group()), source, flags=re.S)


def _profile_returns(part: str, field: str, counter: str) -> str:
    matches = list(re.finditer(r'\breturn\s+([^;]+);', _mask_c(part)))
    for m in reversed(matches):
        expr = part[m.start(1):m.end(1)]
        part = part[:m.start()] + f'CFPQ_PROFILE_RETURN(prof_fn_t, {field}, {counter}, ({expr}));' + part[m.end():]
    return part


def _body(source: str, name: str):
    masked = _mask_c(source)
    m = _one(r'\b(?:static\s+)?(?:inline\s+)?(?:int|GrB_Info)\s+' + re.escape(name) +
             r'\s*\([^;{}]*\)\s*\{', masked, 'function ' + name)
    begin = m.end() - 1
    depth = 1
    for end in range(begin + 1, len(masked)):
        depth += (masked[end] == '{') - (masked[end] == '}')
        if depth == 0:
            return begin, end
    raise SystemExit(f"missing anchor 'closing brace of {name}'")


def _hook(text: str) -> str:
    return '\n#ifdef CFPQ_RSM_PROFILE\n' + text + '\n#endif\n'


def instrument_active(source: str) -> str:
    begin, end = _body(source, 'LAGraph_CFPQ_RSM')
    body = source[begin + 1:end]
    def replace(pattern, replacement, label):
        nonlocal body
        m = _one(pattern, body, label)
        body = body[:m.start()] + replacement(m) + body[m.end():]
    replace(r'\bLG_CLEAR_MSG\s*;', lambda m: m.group() + _hook('''
    cfpq_prof *prof = &g_prof;
    cfpq_prof_init(prof);
    double prof_call_t0 = cfpq_prof_now(), prof_phase_t = 0, prof_final_t = 0;
    bool prof_final_started = false;
'''), 'kernel entry LG_CLEAR_MSG')
    replace(r'GrB_Index\s+Q\s*=\s*rsm->state_count\s*;',
            lambda m: m.group() + _hook('prof->q = Q; prof->v = V; prof->nsources = num_sources;'),
            'original state count')
    loop = _one(r'while\s*\(\s*m_nvals\s*>\s*0\s*\)\s*\{', body, 'frontier loop')
    body = body[:loop.start()] + _hook('prof->setup_s = cfpq_prof_now() - prof_call_t0;') + body[loop.start():loop.end()] + _hook('''
        prof->iters++;
        prof_phase_t = cfpq_prof_now();
        GrB_Index prof_pn = 0;
        GrB_Matrix_nvals(&prof_pn, P);
        prof->sum_m += m_nvals; prof->sum_p += prof_pn;
        if (m_nvals > prof->max_m) prof->max_m = m_nvals;
        if (prof_pn > prof->max_p) prof->max_p = prof_pn;
        uint64_t prof_slot = prof->iters - 1;
        if (prof_slot < CFPQ_PROF_MAX_ITER) {
            prof->iter_in_m[prof_slot] = m_nvals; prof->iter_p_before[prof_slot] = prof_pn;
        } else { prof->detail_truncated = true; }
''') + body[loop.end():]
    phase_patterns = [r'^[ \t]*//\s*Phase\s+1\b[^\n]*',
                      r'^[ \t]*//\s*Phases?\s+2\s*\+\s*3\b[^\n]*',
                      r'^[ \t]*//\s*Phase\s+4\b[^\n]*',
                      r'^[ \t]*//\s*Phase\s+5\b[^\n]*']
    for pattern, previous, boundary in zip(phase_patterns,
            ['activity_s','phase1_s','phase2_3_s','phase4_s'],
            [None,'boundary_p1_s','boundary_p2_3_s','boundary_p4_s']):
        code = f'        prof->{previous} += cfpq_prof_now() - prof_phase_t;\n'
        if boundary:
            code += f'''        double prof_wait_{previous} = cfpq_prof_now();
        GrB_Matrix_wait(acc, GrB_MATERIALIZE);
        prof->{boundary} += cfpq_prof_now() - prof_wait_{previous};
'''
        if previous == 'phase2_3_s':
            code += '        GrB_Matrix_wait(M_call, GrB_MATERIALIZE);\n'
            if 'S_WaitBox *waitboxes' in body:
                code += '        for (size_t a=0; a<num_nonterm; ++a) if (waitboxes[a].w != GrB_NULL) GrB_Matrix_wait(waitboxes[a].w, GrB_MATERIALIZE);\n'
            else:
                code += '        for (size_t a=0; a<num_nonterm; ++a) if (W[a] != GrB_NULL) GrB_Matrix_wait(W[a], GrB_MATERIALIZE);\n'
        if previous == 'phase4_s':
            code += '        for (size_t a=0; a<num_nonterm; ++a) GrB_Matrix_wait(graph_nt[a], GrB_MATERIALIZE);\n'
        if boundary:
            line = f'        prof->{boundary} += cfpq_prof_now() - prof_wait_{previous};\n'
            code = code.replace(line, '') + line
        code += '        prof_phase_t = cfpq_prof_now();'
        replace(pattern, lambda m,c=code: _hook(c)+m.group(), previous+' boundary')
    # Capture the last frontier query, not the initial query before the loop.
    queries=list(re.finditer(r'GRB_TRY\s*\(\s*GrB_Matrix_nvals\s*\(\s*&m_nvals\s*,\s*M\s*\)\s*\)\s*;',body))
    if not queries:raise SystemExit("missing anchor 'frontier completion query'")
    m=queries[-1]
    body=body[:m.end()]+_hook('''
        prof->phase5_s += cfpq_prof_now() - prof_phase_t;
        double prof_wait_p5 = cfpq_prof_now();
        GrB_Matrix_wait(M, GrB_MATERIALIZE); GrB_Matrix_wait(P, GrB_MATERIALIZE);
        prof->boundary_p5_s += cfpq_prof_now() - prof_wait_p5;
        prof->sum_next += m_nvals;
        if (m_nvals > prof->max_next) prof->max_next = m_nvals;
        if (prof_slot < CFPQ_PROF_MAX_ITER) {
            GrB_Index pn=0; GrB_Matrix_nvals(&pn,P);
            prof->iter_next[prof_slot]=m_nvals; prof->iter_p_after[prof_slot]=pn;
        }
''')+body[m.end():]
    replace(r'^[ \t]*//\s*reachable\(w\)[^\n]*',lambda m:_hook('''
    prof_final_t = cfpq_prof_now(); prof_final_started = true;
''')+m.group(),'final reachable projection')
    replace(r'GRB_TRY\s*\(\s*GrB_Matrix_build_BOOL\s*\(\s*M_call\s*,[^;]*?\)\s*\)\s*;',
            lambda m:_hook('double prof_call_build_t = cfpq_prof_now();')+m.group()+_hook('prof->callconstr_s += cfpq_prof_now() - prof_call_build_t;'),
            'bulk call build')
    local_wait = 'S_WaitBox *waitboxes' in body
    old_wait = bool(re.search(r'GrB_Matrix\s*\*W\s*=',body))
    snapshots='''
    prof->compact_q = Q; prof->nterm = num_term; prof->nnonterm = num_nonterm;
    cfpq_prof_snapshot(prof, M, P, acc, scratch, s_temp, new_edges,
                       GrB_NULL, GrB_NULL, GrB_NULL, entry_vec, source_mask, rowvec,
                       '''+('W' if old_wait else 'NULL')+''', graph_nt, num_nonterm);
'''
    if local_wait:
        snapshots+='''
    if (prof->enabled && waitboxes != NULL) for (size_t a = 0; a < num_nonterm; ++a) {
        char name[80];
        prof->waiting_blocks += waitboxes[a].d;
'''
        for field in ('route','x','w','p'):
            snapshots+=f'        snprintf(name, sizeof(name), "wait[%zu].{field}", a); cfpq_prof_matrix(prof->matrix, prof->tag, prof->call_id, name, waitboxes[a].{field});\n'
        if 'S_Fuse *fuse' in source:
            snapshots+='        for (GrB_Index l = 0; l < waitboxes[a].d; ++l) if (waitboxes[a].fuse[l].fused) prof->fused_blocks++;\n'
        snapshots+='    }\n'
    if 'S_GroupSet groupset' in body:
        snapshots+='''
    if (prof->enabled) for (size_t a = 0; a < groupset.ngroups; ++a) {
        char name[80]; prof->group_count++; prof->group_columns += groupset.groups[a].ncols;
        snprintf(name, sizeof(name), "group[%zu].inc", a); cfpq_prof_matrix(prof->matrix, prof->tag, prof->call_id, name, groupset.groups[a].inc);
        snprintf(name, sizeof(name), "group[%zu].tmp", a); cfpq_prof_matrix(prof->matrix, prof->tag, prof->call_id, name, groupset.groups[a].tmp);
    }
'''
        if 's_group_build_blocks' in source:
            snapshots+='''
    if (prof->enabled) for (size_t a = 0; a < groupset.ngroups; ++a) {
        char name[96];
        snprintf(name, sizeof(name), "group[%zu].D", a);
        cfpq_prof_matrix(prof->matrix, prof->tag, prof->call_id, name, groupset.groups[a].D);
        if (groupset.groups[a].frontier_tiles != NULL)
            for (size_t m = 0; m < groupset.groups[a].ntrans; ++m) {
                snprintf(name, sizeof(name), "group[%zu].F[%zu]", a, m);
                cfpq_prof_matrix(prof->matrix, prof->tag, prof->call_id, name,
                                 groupset.groups[a].frontier_tiles[m]);
            }
    }
'''
    if re.search(r'S_(?:WaitBox|FusionBox)\s*\*fusion_boxes', body):
        snapshots+='''
    if (prof->enabled && fusion_boxes != NULL) for (size_t a = 0; a < num_nonterm; ++a)
        for (GrB_Index l = 0; l < fusion_boxes[a].d; ++l) if (fusion_boxes[a].fuse[l].fused) prof->fused_blocks++;
'''
    for name in ('gbuild','fscratch','dprod','bblk','bprod','rprod'):
        if re.search(r'\b'+name+r'\b',body):
            snapshots+=f'    if (prof->enabled) cfpq_prof_matrix(prof->matrix, prof->tag, prof->call_id, "{name}", {name});\n'
    # There can be an early success for an empty trimmed language.
    pattern=r'(?P<indent>^[ \t]*)LG_FREE_WORK\s*;\s*return\s+GrB_SUCCESS\s*;'
    successes=list(re.finditer(pattern,body,re.M))
    if not successes:raise SystemExit("missing anchor 'success cleanup'")
    for m in reversed(successes):
        code='''
    if (prof_final_started) {
        prof->final_s = cfpq_prof_now() - prof_final_t;
    } else { prof->setup_s = cfpq_prof_now() - prof_call_t0; }
    double prof_materialize_t = cfpq_prof_now();
    GrB_Vector_wait(*reachable, GrB_MATERIALIZE);
    prof->materialize_s = cfpq_prof_now() - prof_materialize_t;
    double prof_diag_before = prof->diag_io_s;
    double prof_snapshot_t = cfpq_prof_now();
'''+snapshots+'''
    prof->diag_io_s = prof_diag_before + cfpq_prof_now() - prof_snapshot_t;
    double prof_cleanup_t = cfpq_prof_now();
'''
        body=body[:m.start()]+_hook(code)+'\n'+m['indent']+'LG_FREE_WORK;'+_hook('cfpq_prof_emit(prof, cfpq_prof_now() - prof_call_t0, cfpq_prof_now() - prof_cleanup_t);')+'\n'+m['indent']+'return GrB_SUCCESS;'+body[m.end():]
    result=source[:begin+1]+body+source[end:]
    # Per-operation helper timings are nested subsets, never added to phase totals.
    for name,field,counter in [('s_apply_groups','grouped_s','grouped_calls'),
                               ('s_build_groups','group_setup_s','group_setup_calls'),
                               ('s_compact_build','compaction_s','compaction_calls'),
                               ('s_waitboxes_init','routing_setup_s','routing_setup_calls'),
                               ('s_shift_compact','routing_s','routing_calls'),
                               ('s_scatter_local','routing_s','routing_calls'),
                               ('s_build_fusion','fusion_setup_s','fusion_setup_calls'),
                               ('s_scatter_fused','fusion_s','fusion_calls')]:
        if not re.search(r'\bstatic\s+GrB_Info\s+'+name+r'\b',result):continue
        a,b=_body(result,name)
        part=result[a+1:b]
        part=_profile_returns(part,field,counter)
        result=result[:a+1]+_hook('double prof_fn_t = cfpq_prof_now();')+part+result[b:]
    header='''
#ifdef CFPQ_RSM_PROFILE
#include "cfpq_profile.h"
static cfpq_prof g_prof;
#define CFPQ_PROFILE_ABORT() cfpq_prof_abort(&g_prof)
#define CFPQ_PROFILE_RETURN(t, field, counter, value) do { \\
    g_prof.field += cfpq_prof_now() - (t); g_prof.counter++; return (value); } while (0)
#else
#define CFPQ_PROFILE_ABORT() ((void)0)
#define CFPQ_PROFILE_RETURN(t, field, counter, value) return (value)
#endif
'''
    # Abort open diagnostic streams through the existing error cleanup macro.
    result=result.replace('LG_FREE_WORK;'+(' '*78)+'\\','LG_FREE_WORK;'+(' '*78)+'\\')
    m=_one(r'(#define\s+LG_FREE_ALL\b[^#]*?)(if\s*\(reachable\s*!=\s*NULL\))',result,'error cleanup')
    result=result[:m.start(2)]+'CFPQ_PROFILE_ABORT(); \\\n        '+result[m.start(2):]
    return HEADER+header+result


def instrument(source: str) -> str:
    if 'bool acc_empty' in source:
        return instrument_active(source)
    try:
        return instrument_legacy(source)
    except StopIteration as error:
        raise SystemExit("missing anchor 'legacy source layout'") from error


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    out = Path(sys.argv[2])
    out.write_text(instrument(src.read_text()))
    print(f"wrote {out} ({len(out.read_text().splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
