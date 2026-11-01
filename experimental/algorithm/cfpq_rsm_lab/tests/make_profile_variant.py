#!/usr/bin/env python3
"""Generate the profiling variant of the corrected reference for DS-01-R2.

`variants/correctness_r2.c` is the frozen DS-00-R2 reference; this generator
adds only `#ifdef CFPQ_RSM_PROFILE` hooks.  Compiling the generated file
without that macro reproduces the reference (verified by the DS-00 harness).
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
// cfpq_rsm_lab/variants/correctness_profile.c: generated profiling variant
//------------------------------------------------------------------------------
//
// Generated from variants/correctness_r2.c (the frozen DS-00-R2 reference) by
// tests/make_profile_variant.py.  Do not edit by hand; regenerate instead.
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


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    out = Path(sys.argv[2])
    s = src.read_text()
    for old, new in EDITS:
        count = s.count(old)
        if count != 1:
            raise SystemExit(f"anchor occurs {count} times, expected 1:\n{old[:240]!r}")
        s = s.replace(old, new)
    lines = s.splitlines(keepends=True)
    if not lines[0].startswith("//---"):
        raise SystemExit("unexpected reference header")
    out.write_text(HEADER + "".join(lines[1:]))
    print(f"wrote {out} ({len(out.read_text().splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
