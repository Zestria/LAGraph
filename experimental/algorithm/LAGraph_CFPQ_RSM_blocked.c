//------------------------------------------------------------------------------
// LAGraph_CFPQ_RSM_blocked: block-matrix CFPQ via RSM
//------------------------------------------------------------------------------

// LAGraph, (c) 2019-2025 by The LAGraph Contributors, All Rights Reserved.
// SPDX-License-Identifier: BSD-2-Clause
//
// For additional details (including references to third party source code and
// other files) see the LICENSE file or contact permission@sei.cmu.edu. See
// Contributors.txt for a full list of contributors. Created, in part, with
// funding and support from the U.S. Government (see Acknowledgments.txt file).
// DM22-0790

//------------------------------------------------------------------------------
//
// This is a C / GraphBLAS port of the python-graphblas "lacfpq_blocked"
// algorithm.  Every Python loop over RSM states and transitions is replaced by
// a small number of sparse matrix multiplications operating on the whole state
// space at once.
//
// Block matrix stacking
// ---------------------
// Instead of keeping one |V|x|V| matrix per RSM state:
//
//      P[q], M[q]  in {0,1}^{V x V}    (one pair per state q)
//
// all of them are stacked into a single  QV x V  matrix (QV = Q*V):
//
//      P_block[q*V + u, v] = P[q][u, v]
//      M_block[q*V + u, v] = M[q][u, v]
//
// where q is the 0-based RSM state index.  Row q*V+u records the context
// "box(q) was entered at graph vertex u", while column v is the graph vertex
// the RSM has currently reached.
//
// Routing matrix  K_l  (precomputed once per RSM)
// ----------------------------------------------
//      K_l = (N_l)^T kron I_V   in {0,1}^{QV x QV}
//
// where N_l is the QxQ RSM transition matrix for label l:
//
//      K_l[q'*V + i, q*V + i] = 1  whenever  q -l-> q',  for all i in [0,V).
//
// Key identity (replaces the per-state transition loop):
//
//      block-row q' of  K_l @ (M_block @ G^l)
//          =  OR_{q : N_l[q,q']=1} ( M[q] @ G^l )
//
// Selector matrices  (precomputed once per RSM)
// --------------------------------------------
//   Final_sel[A]  in {0,1}^{V x QV} -- I_V block at every final state of B_A.
//       ( Final_sel[A] @ M_block )[u,v] = OR_{q_f in F_A} M[q_f][u,v]
//     Replaces the per-final-state loop in Phase 1.
//
//   Caller_sel[A] in {0,1}^{V x QV} -- same structure for caller states
//     (states q_c with a q_c -A-> q_r transition).
//       Caller_sel[A] @ M_block  then reduced column-wise
//     gives the call-mask for Phase 3 with no per-transition loop.
//
// Phase summary
// -------------
//   Phase | Original loop                    | Replacement
//   ------+----------------------------------+---------------------------------
//     1   | for q in finals(A)               | Final_sel[A] @ M_block  (VxV)
//     2   | for q in states x labels         | K_l @ (M_block @ G^l), per label
//     3   | for t in nt_transitions          | Caller_sel[A] @ M_block, col-reduce
//     4   | for t in nt_transitions          | K_A @ (P_block @ dG[A]), per NT
//     5   | for q in states                  | single nvals check on New_block
//
// The result is the union, over all source vertices, of the corresponding rows
// of the start non-terminal summary matrix G_nt[start_nonterm].
//
//------------------------------------------------------------------------------

#include "LAGraph.h"
#include "LAGraphX.h"
#include "LG_internal.h"
#include <GraphBLAS.h>

#include <stddef.h>
#include <stdint.h>

//------------------------------------------------------------------------------
// helper macros / functions
//------------------------------------------------------------------------------

#define H_TRY(expr)                                                                                \
    {                                                                                              \
        GrB_Info _hi = (expr);                                                                     \
        if (_hi != GrB_SUCCESS)                                                                    \
        {                                                                                          \
            return _hi;                                                                            \
        }                                                                                          \
    }

// free an array of GrB_Matrix handles (allocated by LAGraph_Malloc)
static inline void free_matrix_array(GrB_Matrix **A, GrB_Index n)
{
    if (A == NULL || *A == NULL)
    {
        return;
    }
    for (GrB_Index i = 0; i < n; ++i)
    {
        GrB_free(&((*A)[i]));
    }
    LAGraph_Free((void **)A, NULL);
}

// replace_with_mask: C = A restricted to the entries that are structurally
// absent from Mask (C and A may alias).
static inline GrB_Info replace_with_mask(GrB_Matrix C, GrB_Matrix A, GrB_Matrix Mask,
                                         GrB_Index nrows, GrB_Index ncols)
{
    return GrB_Matrix_assign(C, Mask, GrB_NULL, A, GrB_ALL, nrows, GrB_ALL, ncols, GrB_DESC_RSC);
}

// build_routing: K = (N)^T kron I_V, of size QV x QV, where N is the QxQ RSM
// transition matrix for a single label.
static inline GrB_Info build_routing(GrB_Matrix *K, GrB_Matrix N, GrB_Matrix I_V, GrB_Index QV)
{
    *K = GrB_NULL;
    H_TRY(GrB_Matrix_new(K, GrB_BOOL, QV, QV));
    H_TRY(GrB_Matrix_kronecker_Semiring(*K, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, N, I_V,
                                        GrB_DESC_T0));
    return GrB_SUCCESS;
}

// build_selector: V x QV Boolean matrix with an identity block I_V at each
// listed state:  sel[i, s*V + i] = true for each state s in states[0..nstates-1]
// and each vertex i in [0, V).
static GrB_Info build_selector(GrB_Matrix *sel, const GrB_Index *states, GrB_Index nstates,
                               GrB_Index V, GrB_Index QV, GrB_Scalar true_scalar)
{
    *sel = GrB_NULL;
    H_TRY(GrB_Matrix_new(sel, GrB_BOOL, V, QV));
    if (nstates == 0)
    {
        return GrB_SUCCESS;
    }

    GrB_Index n = nstates * V;
    GrB_Index *rows = NULL;
    GrB_Index *cols = NULL;

    int info = LAGraph_Malloc((void **)&rows, n, sizeof(GrB_Index), NULL);
    if (info != GrB_SUCCESS)
    {
        return info;
    }
    info = LAGraph_Malloc((void **)&cols, n, sizeof(GrB_Index), NULL);
    if (info != GrB_SUCCESS)
    {
        LAGraph_Free((void **)&rows, NULL);
        return info;
    }

    GrB_Index k = 0;
    for (GrB_Index t = 0; t < nstates; ++t)
    {
        GrB_Index s = states[t];
        for (GrB_Index i = 0; i < V; ++i)
        {
            rows[k] = i;
            cols[k] = s * V + i;
            k++;
        }
    }

    GrB_Info bi = GxB_Matrix_build_Scalar(*sel, rows, cols, true_scalar, n);
    LAGraph_Free((void **)&rows, NULL);
    LAGraph_Free((void **)&cols, NULL);
    return bi;
}

//------------------------------------------------------------------------------
// LAGraph_CFPQ_RSM_blocked
//------------------------------------------------------------------------------

#undef LG_FREE_WORK
#undef LG_FREE_ALL

#define LG_FREE_WORK                                                                               \
    {                                                                                              \
        GrB_free(&M_block);                                                                        \
        GrB_free(&P_block);                                                                        \
        GrB_free(&New_block);                                                                      \
        GrB_free(&step);                                                                           \
        GrB_free(&prod);                                                                           \
        GrB_free(&fresh);                                                                          \
        GrB_free(&extracted);                                                                      \
        GrB_free(&inject);                                                                         \
        GrB_free(&caller_OR);                                                                      \
        GrB_free(&call_mask);                                                                      \
        GrB_free(&caller_mask);                                                                    \
        GrB_free(&source_mask);                                                                    \
        GrB_free(&true_scalar);                                                                    \
        GrB_free(&I_V);                                                                            \
        free_matrix_array(&K_term, num_term);                                                      \
        free_matrix_array(&K_nt, num_nonterm);                                                     \
        free_matrix_array(&Final_sel, num_nonterm);                                                \
        free_matrix_array(&Caller_sel, num_nonterm);                                               \
        free_matrix_array(&dG, num_nonterm);                                                       \
        free_matrix_array(&G_nt, num_nonterm);                                                     \
        LAGraph_Free((void **)&buf_rows, msg);                                                     \
        LAGraph_Free((void **)&buf_cols, msg);                                                     \
        LAGraph_Free((void **)&buf_idx, msg);                                                      \
        LAGraph_Free((void **)&buf_vals, msg);                                                     \
    }

#define LG_FREE_ALL                                                                                \
    {                                                                                              \
        LG_FREE_WORK;                                                                              \
        GrB_free(reachable);                                                                       \
    }

// LAGraph_CFPQ_RSM_blocked: single/multi-source context-free path querying by
// the block-matrix formulation of the RSM algorithm.
int LAGraph_CFPQ_RSM_blocked(GrB_Vector *reachable, const RSM *rsm, const GrB_Matrix *graph_term,
                             const GrB_Index *sources, size_t num_sources, GrB_Index V, char *msg)
{
    LG_CLEAR_MSG;

    // working matrices / vectors
    GrB_Matrix M_block = GrB_NULL;   // frontier,                 QV x V
    GrB_Matrix P_block = GrB_NULL;   // visited configurations,   QV x V
    GrB_Matrix New_block = GrB_NULL; // next frontier,            QV x V
    GrB_Matrix step = GrB_NULL;      // scratch,                  QV x V
    GrB_Matrix prod = GrB_NULL;      // scratch,                  QV x V
    GrB_Matrix fresh = GrB_NULL;     // scratch,                  QV x V
    GrB_Matrix extracted = GrB_NULL; // summary extraction,       V  x V
    GrB_Matrix inject = GrB_NULL;    // call injection seeds,     QV x V
    GrB_Matrix caller_OR = GrB_NULL; // caller activity,          V  x V
    GrB_Matrix I_V = GrB_NULL;       // identity matrix,          V  x V
    GrB_Scalar true_scalar = GrB_NULL;

    GrB_Vector call_mask = GrB_NULL;   // active entry vertices, V
    GrB_Vector caller_mask = GrB_NULL; // per-state caller flags, Q
    GrB_Vector source_mask = GrB_NULL; // source vertices,       V

    GrB_Matrix *K_term = NULL;     // per terminal label:    QV x QV
    GrB_Matrix *K_nt = NULL;       // per nonterminal label: QV x QV
    GrB_Matrix *Final_sel = NULL;  // per nonterminal:       V  x QV
    GrB_Matrix *Caller_sel = NULL; // per nonterminal:       V  x QV
    GrB_Matrix *dG = NULL;         // per nonterminal:       V  x V
    GrB_Matrix *G_nt = NULL;       // per nonterminal:       V  x V (summary)

    GrB_Index *buf_rows = NULL; // scratch, size V
    GrB_Index *buf_cols = NULL; // scratch, size V
    GrB_Index *buf_idx = NULL;  // scratch, size max(Q,V)
    bool *buf_vals = NULL;      // scratch, size max(Q,V)

    // shortcuts
    GrB_Index Q = 0;
    GrB_Index num_term = 0;
    GrB_Index num_nonterm = 0;
    GrB_Index QV = 0;

    LG_ASSERT(reachable != NULL, GrB_NULL_POINTER);
    LG_ASSERT(rsm != NULL, GrB_NULL_POINTER);
    LG_ASSERT(graph_term != NULL, GrB_NULL_POINTER);
    LG_ASSERT(sources != NULL, GrB_NULL_POINTER);

    Q = rsm->state_count;
    num_term = rsm->terminal_count;
    num_nonterm = rsm->nonterminal_count;
    QV = Q * V;

    GrB_Index start_nonterm = rsm->start_nonterminal;
    GrB_Matrix *rsm_term = rsm->terminal_matrices;
    GrB_Matrix *rsm_nonterm = rsm->nonterminal_matrices;
    GrB_Index *start_states = rsm->start_states;
    GrB_Vector *final_states = rsm->final_states;

    LG_ASSERT(rsm_term != NULL, GrB_NULL_POINTER);
    LG_ASSERT(rsm_nonterm != NULL, GrB_NULL_POINTER);
    LG_ASSERT(start_states != NULL, GrB_NULL_POINTER);
    LG_ASSERT(final_states != NULL, GrB_NULL_POINTER);
    LG_ASSERT(start_nonterm < num_nonterm, GrB_INVALID_VALUE);
    LG_ASSERT(num_sources > 0, GrB_INVALID_VALUE);
    LG_ASSERT(num_sources <= V, GrB_INVALID_VALUE);

    *reachable = GrB_NULL;

    //--------------------------------------------------------------------------
    // precompute: routing matrices K_l and selector matrices
    //--------------------------------------------------------------------------

    if (num_term > 0)
    {
        LG_TRY(LAGraph_Malloc((void **)&K_term, num_term, sizeof(GrB_Matrix), msg));
        for (GrB_Index t = 0; t < num_term; ++t)
            K_term[t] = GrB_NULL;
    }
    if (num_nonterm > 0)
    {
        LG_TRY(LAGraph_Malloc((void **)&K_nt, num_nonterm, sizeof(GrB_Matrix), msg));
        LG_TRY(LAGraph_Malloc((void **)&Final_sel, num_nonterm, sizeof(GrB_Matrix), msg));
        LG_TRY(LAGraph_Malloc((void **)&Caller_sel, num_nonterm, sizeof(GrB_Matrix), msg));
        LG_TRY(LAGraph_Malloc((void **)&dG, num_nonterm, sizeof(GrB_Matrix), msg));
        for (GrB_Index i = 0; i < num_nonterm; ++i)
        {
            K_nt[i] = GrB_NULL;
            Final_sel[i] = GrB_NULL;
            Caller_sel[i] = GrB_NULL;
            dG[i] = GrB_NULL;
        }
    }

    // scratch buffers
    LG_TRY(LAGraph_Malloc((void **)&buf_rows, V, sizeof(GrB_Index), msg));
    LG_TRY(LAGraph_Malloc((void **)&buf_cols, V, sizeof(GrB_Index), msg));
    GrB_Index bmax = (Q > V) ? Q : V;
    LG_TRY(LAGraph_Malloc((void **)&buf_idx, bmax, sizeof(GrB_Index), msg));
    LG_TRY(LAGraph_Malloc((void **)&buf_vals, bmax, sizeof(bool), msg));

    // scalar "true", used to build all-true (I,J) tuples
    GRB_TRY(GrB_Scalar_new(&true_scalar, GrB_BOOL));
    GRB_TRY(GrB_Scalar_setElement_BOOL(true_scalar, true));

    // I_V, the V x V identity matrix
    {
        GrB_Vector ones = GrB_NULL;
        GRB_TRY(GrB_Vector_new(&ones, GrB_BOOL, V));
        GRB_TRY(GrB_Vector_assign_BOOL(ones, GrB_NULL, GrB_NULL, true, GrB_ALL, V, GrB_NULL));
        GRB_TRY(GrB_Matrix_diag(&I_V, ones, 0));
        GrB_free(&ones);
    }

    // routing matrix per terminal label
    for (GrB_Index t = 0; t < num_term; ++t)
    {
        if (rsm_term[t] != GrB_NULL)
        {
            GRB_TRY(build_routing(&K_term[t], rsm_term[t], I_V, QV));
        }
        else
        {
            GRB_TRY(GrB_Matrix_new(&K_term[t], GrB_BOOL, QV, QV));
        }
    }

    // routing + selectors per nonterminal
    GRB_TRY(GrB_Vector_new(&caller_mask, GrB_BOOL, Q));
    for (GrB_Index i = 0; i < num_nonterm; ++i)
    {
        if (rsm_nonterm[i] != GrB_NULL)
        {
            GRB_TRY(build_routing(&K_nt[i], rsm_nonterm[i], I_V, QV));
        }
        else
        {
            GRB_TRY(GrB_Matrix_new(&K_nt[i], GrB_BOOL, QV, QV));
        }

        // final-state selector: states present in final_states[i]
        if (final_states[i] != GrB_NULL)
        {
            GrB_Index nfinal = Q;
            GRB_TRY(GrB_Vector_extractTuples_BOOL(buf_idx, buf_vals, &nfinal, final_states[i]));
            GRB_TRY(build_selector(&Final_sel[i], buf_idx, nfinal, V, QV, true_scalar));
        }
        else
        {
            GRB_TRY(GrB_Matrix_new(&Final_sel[i], GrB_BOOL, V, QV));
        }

        // caller-state selector: any row of rsm_nonterm[i] with an entry
        if (rsm_nonterm[i] != GrB_NULL)
        {
            GRB_TRY(GrB_Vector_clear(caller_mask));
            GRB_TRY(GrB_Matrix_reduce_Monoid(caller_mask, GrB_NULL, GrB_NULL, GrB_LOR_MONOID_BOOL,
                                             rsm_nonterm[i], GrB_NULL));
            GrB_Index ncaller = Q;
            GRB_TRY(GrB_Vector_extractTuples_BOOL(buf_idx, buf_vals, &ncaller, caller_mask));
            GRB_TRY(build_selector(&Caller_sel[i], buf_idx, ncaller, V, QV, true_scalar));
        }
        else
        {
            GRB_TRY(GrB_Matrix_new(&Caller_sel[i], GrB_BOOL, V, QV));
        }

        // summary edges discovered for this nonterminal
        GRB_TRY(GrB_Matrix_new(&dG[i], GrB_BOOL, V, V));
    }

    //--------------------------------------------------------------------------
    // allocate working matrices
    //--------------------------------------------------------------------------

    GRB_TRY(GrB_Matrix_new(&M_block, GrB_BOOL, QV, V));
    GRB_TRY(GrB_Matrix_new(&P_block, GrB_BOOL, QV, V));
    GRB_TRY(GrB_Matrix_new(&New_block, GrB_BOOL, QV, V));
    GRB_TRY(GrB_Matrix_new(&step, GrB_BOOL, QV, V));
    GRB_TRY(GrB_Matrix_new(&prod, GrB_BOOL, QV, V));
    GRB_TRY(GrB_Matrix_new(&fresh, GrB_BOOL, QV, V));
    GRB_TRY(GrB_Matrix_new(&extracted, GrB_BOOL, V, V));
    GRB_TRY(GrB_Matrix_new(&inject, GrB_BOOL, QV, V));
    GRB_TRY(GrB_Matrix_new(&caller_OR, GrB_BOOL, V, V));
    GRB_TRY(GrB_Vector_new(&call_mask, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(&source_mask, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(reachable, GrB_BOOL, V));

    // G_nt[i] = summary matrix for nonterminal i, initially empty
    LG_TRY(LAGraph_Calloc((void **)&G_nt, num_nonterm, sizeof(GrB_Matrix), msg));
    for (GrB_Index i = 0; i < num_nonterm; ++i)
    {
        GRB_TRY(GrB_Matrix_new(&G_nt[i], GrB_BOOL, V, V));
    }

    //--------------------------------------------------------------------------
    // seed the initial frontier and visited set
    //--------------------------------------------------------------------------

    {
        GrB_Index s_S = start_states[start_nonterm];
        for (size_t k = 0; k < num_sources; ++k)
        {
            GrB_Index u = sources[k];
            GrB_Index row = s_S * V + u;
            GRB_TRY(GrB_Matrix_setElement_BOOL(M_block, true, row, u));
            GRB_TRY(GrB_Matrix_setElement_BOOL(P_block, true, row, u));
        }
    }

    //--------------------------------------------------------------------------
    // main fixed-point loop
    //--------------------------------------------------------------------------

    bool keep_going = true;
    while (keep_going)
    {
        // PHASE 1: summary extraction (independent of terminal labels)
        // dG[A][u,w] = 1 iff box B_A was entered at u and just exited at w.
        for (GrB_Index i = 0; i < num_nonterm; ++i)
        {
            GrB_Index sel_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&sel_nvals, Final_sel[i]));
            if (sel_nvals == 0)
            {
                GRB_TRY(GrB_Matrix_clear(dG[i]));
                continue;
            }

            // extracted = Final_sel[i] * M_block  -> V x V
            GRB_TRY(GrB_mxm(extracted, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, Final_sel[i],
                            M_block, GrB_NULL));

            // dG[i] = extracted & ~G_nt[i]
            GRB_TRY(replace_with_mask(dG[i], extracted, G_nt[i], V, V));

            GrB_Index dg_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&dg_nvals, dG[i]));
            if (dg_nvals > 0)
            {
                // G_nt[i] |= dG[i]
                GRB_TRY(
                    GrB_eWiseAdd(G_nt[i], GrB_NULL, GrB_NULL, GrB_LOR, G_nt[i], dG[i], GrB_NULL));
            }
        }

        // PHASE 2: frontier propagation
        // K_l @ (M_block @ G^l) routes each state-block through label-l edges
        // and delivers results to the correct destination state blocks.
        for (GrB_Index t = 0; t < num_term; ++t)
        {
            if (graph_term[t] == GrB_NULL)
            {
                continue;
            }
            GrB_Index k_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&k_nvals, K_term[t]));
            if (k_nvals == 0)
            {
                continue;
            }
            GrB_Index g_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&g_nvals, graph_term[t]));
            if (g_nvals == 0)
            {
                continue;
            }

            // step = M_block * G^t  -> QV x V
            GRB_TRY(GrB_mxm(step, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, M_block,
                            graph_term[t], GrB_NULL));

            // prod = K_t * step  -> QV x V
            GRB_TRY(GrB_mxm(prod, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, K_term[t], step,
                            GrB_NULL));

            // fresh = prod & ~P_block
            GRB_TRY(replace_with_mask(fresh, prod, P_block, QV, V));

            GrB_Index f_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&f_nvals, fresh));
            if (f_nvals > 0)
            {
                GRB_TRY(GrB_eWiseAdd(New_block, GrB_NULL, GrB_NULL, GrB_LOR, New_block, fresh,
                                     GrB_NULL));
            }
        }

        for (GrB_Index i = 0; i < num_nonterm; ++i)
        {
            GrB_Index k_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&k_nvals, K_nt[i]));
            if (k_nvals == 0)
            {
                continue;
            }
            GrB_Index g_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&g_nvals, G_nt[i]));
            if (g_nvals == 0)
            {
                continue;
            }

            GRB_TRY(GrB_mxm(step, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, M_block, G_nt[i],
                            GrB_NULL));
            GRB_TRY(GrB_mxm(prod, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, K_nt[i], step,
                            GrB_NULL));
            GRB_TRY(replace_with_mask(fresh, prod, P_block, QV, V));

            GrB_Index f_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&f_nvals, fresh));
            if (f_nvals > 0)
            {
                GRB_TRY(GrB_eWiseAdd(New_block, GrB_NULL, GrB_NULL, GrB_LOR, New_block, fresh,
                                     GrB_NULL));
            }
        }

        // PHASE 3: call injection
        // Any current-graph-vertex v active in a caller state spawns a fresh
        // exploration of B_A seeded at context (s_A, v, v).
        for (GrB_Index i = 0; i < num_nonterm; ++i)
        {
            GrB_Index cs_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&cs_nvals, Caller_sel[i]));
            if (cs_nvals == 0)
            {
                continue;
            }

            // caller_OR = Caller_sel[i] * M_block  -> V x V
            GRB_TRY(GrB_mxm(caller_OR, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL,
                            Caller_sel[i], M_block, GrB_NULL));

            GrB_Index co_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&co_nvals, caller_OR));
            if (co_nvals == 0)
            {
                continue;
            }

            // call_mask = column-wise OR of caller_OR  -> V
            GRB_TRY(GrB_Vector_clear(call_mask));
            GRB_TRY(GrB_Matrix_reduce_Monoid(call_mask, GrB_NULL, GrB_NULL, GrB_LOR_MONOID_BOOL,
                                             caller_OR, GrB_DESC_T0));

            GrB_Index cm_nvals = 0;
            GRB_TRY(GrB_Vector_nvals(&cm_nvals, call_mask));
            if (cm_nvals == 0)
            {
                continue;
            }

            // inject[s_A*V + v, v] = true for every active vertex v
            GrB_Index nv = V;
            GRB_TRY(GrB_Vector_extractTuples_BOOL(buf_idx, buf_vals, &nv, call_mask));
            GrB_Index s_A = start_states[i];
            for (GrB_Index k = 0; k < nv; ++k)
            {
                buf_rows[k] = s_A * V + buf_idx[k];
                buf_cols[k] = buf_idx[k];
            }
            GRB_TRY(GrB_Matrix_clear(inject));
            GRB_TRY(GxB_Matrix_build_Scalar(inject, buf_rows, buf_cols, true_scalar, nv));

            // fresh = inject & ~P_block
            GRB_TRY(replace_with_mask(fresh, inject, P_block, QV, V));

            GrB_Index f_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&f_nvals, fresh));
            if (f_nvals > 0)
            {
                GRB_TRY(GrB_eWiseAdd(New_block, GrB_NULL, GrB_NULL, GrB_LOR, New_block, fresh,
                                     GrB_NULL));
            }
        }

        // PHASE 4: retroactive propagation
        // Old call-site contexts x newly discovered summary edges.  Structurally
        // identical to Phase 2, using P_block and dG[A].
        for (GrB_Index i = 0; i < num_nonterm; ++i)
        {
            GrB_Index dg_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&dg_nvals, dG[i]));
            if (dg_nvals == 0)
            {
                continue;
            }
            GrB_Index k_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&k_nvals, K_nt[i]));
            if (k_nvals == 0)
            {
                continue;
            }

            // step = P_block * dG[i]  -> QV x V
            GRB_TRY(GrB_mxm(step, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, P_block, dG[i],
                            GrB_NULL));

            // prod = K_i * step  -> QV x V
            GRB_TRY(GrB_mxm(prod, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, K_nt[i], step,
                            GrB_NULL));

            // fresh = prod & ~P_block
            GRB_TRY(replace_with_mask(fresh, prod, P_block, QV, V));

            GrB_Index f_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&f_nvals, fresh));
            if (f_nvals > 0)
            {
                GRB_TRY(GrB_eWiseAdd(New_block, GrB_NULL, GrB_NULL, GrB_LOR, New_block, fresh,
                                     GrB_NULL));
            }
        }

        // PHASE 5: commit
        GrB_Index new_nvals = 0;
        GRB_TRY(GrB_Matrix_nvals(&new_nvals, New_block));
        if (new_nvals == 0)
        {
            break;
        }

        // P_block |= New_block
        GRB_TRY(GrB_eWiseAdd(P_block, GrB_NULL, GrB_NULL, GrB_LOR, P_block, New_block, GrB_NULL));

        // M_block = New_block; New_block is recycled
        GrB_Matrix swap = M_block;
        M_block = New_block;
        New_block = swap;
        GRB_TRY(GrB_Matrix_clear(New_block));
    }

    //--------------------------------------------------------------------------
    // extract the result: union of the source rows of G_nt[start_nonterm]
    //--------------------------------------------------------------------------

    {
        bool *vals = NULL;
        LG_TRY(LAGraph_Malloc((void **)&vals, num_sources, sizeof(bool), msg));
        for (size_t i = 0; i < num_sources; ++i)
        {
            vals[i] = true;
        }
        GrB_Info bi =
            GrB_Vector_build_BOOL(source_mask, sources, vals, num_sources, GrB_SECOND_BOOL);
        LAGraph_Free((void **)&vals, GrB_NULL);
        GRB_TRY(bi);
    }

    GRB_TRY(GrB_mxv(*reachable, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, G_nt[start_nonterm],
                    source_mask, GrB_DESC_T0));

    LG_FREE_WORK;
    return GrB_SUCCESS;
}
