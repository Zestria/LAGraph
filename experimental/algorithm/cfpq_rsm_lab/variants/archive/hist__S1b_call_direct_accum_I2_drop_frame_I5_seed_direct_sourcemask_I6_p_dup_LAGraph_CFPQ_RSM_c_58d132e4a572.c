#include "LAGraph.h"
#include "LG_internal.h"
#include <GraphBLAS.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LG_FREE_WORK                                                                               \
    {                                                                                              \
        GrB_free(&M);                                                                              \
        GrB_free(&acc);                                                                            \
        GrB_free(&P);                                                                              \
        GrB_free(&M_call);                                                                         \
        GrB_free(&M_new);                                                                          \
        GrB_free(&temp1);                                                                          \
        GrB_free(&temp2);                                                                          \
        GrB_free(&frame);                                                                          \
        GrB_free(&new_edges);                                                                      \
        GrB_free(&diag_entry);                                                                     \
        GrB_free(&entry_vec);                                                                      \
        GrB_free(&source_mask);                                                                    \
        if (W != NULL)                                                                             \
        {                                                                                          \
            for (size_t _i = 0; _i < num_nonterm; ++_i)                                            \
                GrB_free(&W[_i]);                                                                  \
            LAGraph_Free((void **)&W, msg);                                                        \
        }                                                                                          \
        GrB_free(&rowvec);                                                                         \
        LAGraph_Free((void **)&active, NULL);                                                      \
        LAGraph_Free((void **)&active_idx, NULL);                                                  \
        LAGraph_Free((void **)&active_vals, NULL);                                                 \
        if (term_src != NULL)                                                                      \
        {                                                                                          \
            for (size_t _i = 0; _i < num_term; ++_i)                                               \
                LAGraph_Free((void **)&term_src[_i], NULL);                                        \
            LAGraph_Free((void **)&term_src, NULL);                                                \
        }                                                                                          \
        LAGraph_Free((void **)&term_src_count, NULL);                                              \
        if (graph_nt != NULL)                                                                      \
        {                                                                                          \
            for (size_t _i = 0; _i < num_nonterm; ++_i)                                            \
                GrB_free(&graph_nt[_i]);                                                           \
            LAGraph_Free((void **)&graph_nt, msg);                                                 \
        }                                                                                          \
        if (rsm_start != NULL)                                                                     \
        {                                                                                          \
            for (size_t _i = 0; _i < num_nonterm; ++_i)                                            \
                GrB_free(&rsm_start[_i]);                                                          \
            LAGraph_Free((void **)&rsm_start, msg);                                                \
        }                                                                                          \
    }
#define LG_FREE_ALL                                                                                \
    {                                                                                              \
        LG_FREE_WORK;                                                                              \
        GrB_free(reachable);                                                                       \
    }

#define H_TRY(expr)                                                                                \
    {                                                                                              \
        GrB_Info _hi = (expr);                                                                     \
        if (_hi != GrB_SUCCESS)                                                                    \
            return _hi;                                                                            \
    }

static inline GrB_Info reshape( // input/output:
    GrB_Matrix *C,              // input/output matrix, reshaped in place
    bool by_col,                // true if reshape by column, false if by row
    GrB_Index nrows_new,        // new number of rows of C
    GrB_Index ncols_new,        // new number of columns of C
    const GrB_Descriptor desc   // to control # of threads used
)
{
    if (C == NULL || *C == NULL)
        return GrB_NULL_POINTER;

    GrB_Matrix temp = GrB_NULL;
    H_TRY(GxB_Matrix_reshapeDup(&temp, *C, by_col, nrows_new, ncols_new, desc));
    H_TRY(GrB_Matrix_free(C));

    *C = temp;
    return GrB_SUCCESS;
}

static GrB_Info s_shift(GrB_Matrix *temp1, GrB_Matrix A, GrB_Matrix M, GrB_Index Q,
                        GrB_Index V)
{
    H_TRY(GrB_Matrix_clear(*temp1));

    // temp1 = A^T * M == |Q|x|VV|
    H_TRY(GrB_mxm(*temp1, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, A, M, GrB_DESC_T0));

    // reshape: |Q|x|VV| -> |QV|x|V|
    H_TRY(reshape(temp1, false, Q * V, V, GrB_NULL));
    return GrB_SUCCESS;
}

// restore the scratch matrix to |Q|*|V*V|; its content is dead, so clear it first
static GrB_Info s_unshift(GrB_Matrix *temp1, GrB_Index Q, GrB_Index V)
{
    H_TRY(GrB_Matrix_clear(*temp1));
    H_TRY(reshape(temp1, false, Q, V * V, GrB_NULL));
    return GrB_SUCCESS;
}

static GrB_Info s_mul_acc(GrB_Matrix C,      // output accumulator |QV|*|V|
                          GrB_Matrix A,      // shifted frontier   |QV|*|V|
                          GrB_Matrix B,      // graph matrix       |V|*|V|
                          GrB_Matrix *temp2, // scratch            |QV|*|V|
                          GrB_Index Q, GrB_Index V)
{
    (void) Q; (void) V;

    H_TRY(GrB_Matrix_clear(*temp2));

    // temp2 = A * B -> |QV|*|V|, already in the accumulator's shape
    H_TRY(GrB_mxm(*temp2, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, A, B, GrB_NULL));

    // C |= temp2
    H_TRY(GrB_eWiseAdd(C, GrB_NULL, GrB_NULL, GrB_LOR, C, *temp2, GrB_NULL));
    return GrB_SUCCESS;
}

// C |= reshape(A^T * M) * B
static GrB_Info s_mxm_chain(GrB_Matrix C,      // output accumulator  |Q|*|VV|
                            GrB_Matrix A,      // RSM matrix          |Q|*|Q|
                            GrB_Matrix M,      // frontier            |Q|*|VV|
                            GrB_Matrix B,      // graph matrix        |V|*|V|
                            GrB_Matrix *temp1, // scratch             |Q|*|VV|
                            GrB_Matrix *temp2, // scratch             |Q|*|VV|
                            GrB_Index Q, GrB_Index V)
{
    H_TRY(s_shift(temp1, A, M, Q, V));
    H_TRY(s_mul_acc(C, *temp1, B, temp2, Q, V));
    H_TRY(s_unshift(temp1, Q, V));
    return GrB_SUCCESS;
}

#undef H_TRY
typedef struct
{
    GrB_Index state_count;            // Q
    GrB_Index terminal_count;         // num_terminals
    GrB_Index nonterminal_count;      // num_nonterminals
    GrB_Index start_nonterminal;      // start_nonterm
    GrB_Matrix *terminal_matrices;    // rsm_term
    GrB_Matrix *nonterminal_matrices; // rsm_nonterm
    GrB_Index *start_states;          // rsm_start
    GrB_Vector *final_states;         // rsm_final
} RSM;

int LAGraph_CFPQ_RSM(GrB_Vector *reachable, const RSM *rsm, const GrB_Matrix *graph_term,
                     const GrB_Index *sources, size_t num_sources, GrB_Index V, char *msg)
{
    LG_CLEAR_MSG;

    // shortcuts
    GrB_Index VV = V * V;
    GrB_Index Q = rsm->state_count;
    GrB_Index num_term = rsm->terminal_count;
    GrB_Index num_nonterm = rsm->nonterminal_count;
    GrB_Index start_nonterm = rsm->start_nonterminal;
    GrB_Matrix *rsm_term = rsm->terminal_matrices;
    GrB_Matrix *rsm_nonterm = rsm->nonterminal_matrices;
    GrB_Index *start_states = rsm->start_states;
    GrB_Vector *final_states = rsm->final_states;

    // working matrices / vectors
    GrB_Matrix M = GrB_NULL;
    GrB_Matrix P = GrB_NULL;
    GrB_Matrix acc = GrB_NULL;
    GrB_Matrix M_call = GrB_NULL;
    GrB_Matrix M_new = GrB_NULL;
    GrB_Matrix temp1 = GrB_NULL;
    GrB_Matrix temp2 = GrB_NULL;
    GrB_Matrix frame = GrB_NULL;
    GrB_Matrix new_edges = GrB_NULL;
    GrB_Matrix diag_entry = GrB_NULL;
    GrB_Vector entry_vec = GrB_NULL;
    GrB_Vector source_mask = GrB_NULL;
    GrB_Vector rowvec = GrB_NULL;
    bool *active = NULL;
    GrB_Index *active_idx = NULL;
    bool *active_vals = NULL;
    GrB_Index **term_src = NULL;
    size_t *term_src_count = NULL;

    GrB_Matrix *graph_nt = NULL;
    GrB_Matrix *W = NULL; // |Q*V|*|V|, W[i] = rsm_nonterm[i]^T * P
    GrB_Vector *rsm_start = NULL; // rsm_start[S][i] = true, iff start_states[S] = i

    LG_ASSERT(reachable != NULL, GrB_NULL_POINTER);
    LG_ASSERT(rsm_term != NULL, GrB_NULL_POINTER);
    LG_ASSERT(rsm_nonterm != NULL, GrB_NULL_POINTER);
    LG_ASSERT(start_states != NULL, GrB_NULL_POINTER);
    LG_ASSERT(final_states != NULL, GrB_NULL_POINTER);
    LG_ASSERT(start_nonterm < num_nonterm, GrB_INVALID_VALUE);
    LG_ASSERT(num_sources <= V, GrB_INVALID_VALUE);

    *reachable = GrB_NULL;

    // allocate per-nonterminal arrays
    LG_TRY(LAGraph_Malloc((void **)&graph_nt, rsm->nonterminal_count, sizeof(GrB_Matrix), msg));
    LG_TRY(LAGraph_Malloc((void **)&W, rsm->nonterminal_count, sizeof(GrB_Matrix), msg));
    LG_TRY(LAGraph_Malloc((void **)&rsm_start, rsm->nonterminal_count, sizeof(GrB_Vector), msg));

    for (size_t i = 0; i < num_nonterm; ++i)
    {
        graph_nt[i] = GrB_NULL;
        W[i] = GrB_NULL;
        rsm_start[i] = GrB_NULL;
    }

    for (size_t i = 0; i < num_nonterm; ++i)
    {
        GRB_TRY(GrB_Matrix_new(&graph_nt[i], GrB_BOOL, V, V));
        if (rsm_nonterm[i] != GrB_NULL)
            GRB_TRY(GrB_Matrix_new(&W[i], GrB_BOOL, Q * V, V));
        GRB_TRY(GrB_Vector_new(&rsm_start[i], GrB_BOOL, Q));
        GrB_Vector_setElement_BOOL(rsm_start[i], true, start_states[i]);
    }

    // allocate working matrices
    GRB_TRY(GrB_Matrix_new(&M, GrB_BOOL, Q, VV));
    GRB_TRY(GrB_Matrix_new(&P, GrB_BOOL, Q, VV));
    GRB_TRY(GrB_Matrix_new(&acc, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&M_call, GrB_BOOL, Q, VV));
    GRB_TRY(GrB_Matrix_new(&M_new, GrB_BOOL, Q, VV));
    GRB_TRY(GrB_Matrix_new(&temp1, GrB_BOOL, Q, VV));
    GRB_TRY(GrB_Matrix_new(&temp2, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&frame, GrB_BOOL, Q, VV));
    GRB_TRY(GrB_Matrix_new(&new_edges, GrB_BOOL, 1, VV));
    // there is no need to create matrix for diag_entry
    GRB_TRY(GrB_Vector_new(&entry_vec, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(&source_mask, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(reachable, GrB_BOOL, V));

    // static per-terminal source-state patterns, computed once per call
    if (num_term > 0)
    {
        LG_TRY(LAGraph_Malloc((void **)&term_src, num_term, sizeof(GrB_Index *), msg));
        for (size_t i = 0; i < num_term; ++i)
            term_src[i] = NULL;

        LG_TRY(LAGraph_Malloc((void **)&term_src_count, num_term, sizeof(size_t), msg));
        for (size_t i = 0; i < num_term; ++i)
            term_src_count[i] = 0;

        for (size_t i = 0; i < num_term; ++i)
        {
            if (rsm_term[i] == GrB_NULL)
                continue;

            GrB_Index rn = 0;
            GRB_TRY(GrB_Matrix_nvals(&rn, rsm_term[i]));
            if (rn == 0)
                continue;

            LG_TRY(LAGraph_Malloc((void **)&term_src[i], rn, sizeof(GrB_Index), msg));
            GrB_Index rn2 = rn;
            GRB_TRY(GrB_Matrix_extractTuples_BOOL(term_src[i], NULL, NULL, &rn2, rsm_term[i]));
            term_src_count[i] = (size_t)rn2;
        }
    }

    if (Q > 0)
    {
        GRB_TRY(GrB_Vector_new(&rowvec, GrB_BOOL, Q));
        LG_TRY(LAGraph_Malloc((void **)&active, Q, sizeof(bool), msg));
        LG_TRY(LAGraph_Malloc((void **)&active_idx, Q, sizeof(GrB_Index), msg));
        LG_TRY(LAGraph_Malloc((void **)&active_vals, Q, sizeof(bool), msg));
        for (GrB_Index i = 0; i < Q; ++i)
            active[i] = false;
    }

    // seed initial frontier M and visited P
    {

        GrB_Index nstart = 0;
        GRB_TRY(GrB_Vector_nvals(&nstart, rsm_start[start_nonterm]));

        GrB_Index *seed_states = NULL;
        bool *seed_vals = NULL;
        LG_TRY(LAGraph_Malloc((void **)&seed_states, nstart, sizeof(GrB_Index), msg));
        LG_TRY(LAGraph_Malloc((void **)&seed_vals, nstart, sizeof(bool), msg));

        GrB_Info info = GrB_Vector_extractTuples_BOOL(seed_states, seed_vals, &nstart,
                                                      rsm_start[start_nonterm]);
        LAGraph_Free((void **)&seed_vals, GrB_NULL);

        if (info != GrB_SUCCESS)
        {
            LAGraph_Free((void **)&seed_states, GrB_NULL);
            LG_FREE_ALL;
            return info;
        }

        for (size_t i = 0; i < num_sources; ++i)
        {
            GrB_Index seed_col = sources[i] * V + sources[i];
            for (GrB_Index k = 0; k < nstart; ++k)
            {
                GrB_Index q = seed_states[k];
                GRB_TRY(GrB_Matrix_setElement_BOOL(M, true, q, seed_col));
                GRB_TRY(GrB_Matrix_setElement_BOOL(P, true, q, seed_col));
            }
        }

        LAGraph_Free((void **)&seed_states, GrB_NULL);
    }

    // main fixed-point loop
    GrB_Index m_nvals = 0;
    GRB_TRY(GrB_Matrix_nvals(&m_nvals, M));
    while (m_nvals > 0)
    {
        // the single accumulator holds terminal, nonterminal and return products
        GRB_TRY(GrB_Matrix_clear(acc));

        // PHASE 1: TERMINAL TRANSITIONS
        for (size_t i = 0; i < num_term; ++i)
        {
            if (rsm_term[i] == GrB_NULL || graph_term[i] == GrB_NULL)
                continue;
            LG_TRY(s_mxm_chain(acc, rsm_term[i], M, graph_term[i], &temp1, &temp2, Q, V));
        }

        // PHASE 2+3: NONTERMINAL TRANSITIONS AND CALLS
        GRB_TRY(GrB_Matrix_clear(M_call));
        for (size_t i = 0; i < num_nonterm; ++i)
        {
            if (rsm_nonterm[i] == GrB_NULL)
                continue;

            // temp1 = reshape(rsm_nonterm[i]^T * M): |Q|*|VV| -> |QV|*|V|
            LG_TRY(s_shift(&temp1, rsm_nonterm[i], M, Q, V));

            GrB_Index x_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&x_nvals, temp1));
            if (x_nvals > 0)
            {
                // CALL: entry_vec = column-OR of the shared shifted frontier
                GRB_TRY(GrB_Vector_clear(entry_vec));
                GRB_TRY(GrB_reduce(entry_vec, GrB_NULL, GrB_NULL, GrB_LOR_MONOID_BOOL, temp1,
                                   GrB_DESC_T0));

                GrB_Index ev_nvals = 0;
                GRB_TRY(GrB_Vector_nvals(&ev_nvals, entry_vec));
                if (ev_nvals > 0)
                {
                    // Build diag(entry_vec) and flatten to 1*|V*V|
                    GRB_TRY(GrB_Matrix_free(&diag_entry));
                    diag_entry = GrB_NULL;
                    GRB_TRY(GrB_Matrix_diag(&diag_entry, entry_vec, 0)); // |V|*|V|
                    GRB_TRY(reshape(&diag_entry, false, 1, VV, GrB_NULL));

                    // M_call |= rsm_start[i] * diag_entry
                    GRB_TRY(GrB_mxm(M_call, GrB_NULL, GrB_LOR, GxB_LOR_LAND_BOOL,
                                    (GrB_Matrix)rsm_start[i], diag_entry, GrB_NULL));
                }

                // NON-TERMINAL TRANSITIONS through already known summary edges
                GrB_Index gnt_nvals = 0;
                GRB_TRY(GrB_Matrix_nvals(&gnt_nvals, graph_nt[i]));
                if (gnt_nvals > 0)
                    LG_TRY(s_mul_acc(acc, temp1, graph_nt[i], &temp2, Q, V));

                // RETURN bookkeeping: W[i] == reshape(rsm_nonterm[i]^T * P)
                GRB_TRY(GrB_eWiseAdd(W[i], GrB_NULL, GrB_NULL, GrB_LOR, W[i], temp1, GrB_NULL));
            }
            LG_TRY(s_unshift(&temp1, Q, V));
        }

        // PHASE 4: RETURN TRANSITIONS
        for (size_t i = 0; i < num_nonterm; ++i)
        {
            if (final_states[i] == GrB_NULL || rsm_nonterm[i] == GrB_NULL)
                continue;

            // 1*|Q| * |Q|*|V*V| -> 1*|V*V|
            GRB_TRY(GrB_Matrix_clear(new_edges));
            GRB_TRY(GrB_mxm(new_edges, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL,
                            (GrB_Matrix)final_states[i], M, GrB_DESC_T0));

            GrB_Index ne_vals = 0;
            GRB_TRY(GrB_Matrix_nvals(&ne_vals, new_edges));
            if (ne_vals == 0)
                continue;

            GRB_TRY(reshape(&new_edges, false, V, V, GrB_NULL));
            GRB_TRY(GrB_eWiseAdd(graph_nt[i], GrB_NULL, GrB_NULL, GrB_LOR, graph_nt[i], new_edges,
                                 GrB_NULL));

            // M_return |= W[i] * dG, where W[i] == reshape(rsm_nonterm[i]^T * P)
            GrB_Index w_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&w_nvals, W[i]));
            if (w_nvals > 0)
                LG_TRY(s_mul_acc(acc, W[i], new_edges, &temp2, Q, V));

            GRB_TRY(reshape(&new_edges, false, 1, VV, GrB_NULL));
        }

        // PHASE 5: MERGE & MARK VISITED
        // GrB_assign has big overhead
        // M_new = (acc | M_call) & ~P
        GRB_TRY(GrB_Matrix_free(&M_new));
        GRB_TRY(GxB_Matrix_reshapeDup(&M_new, acc, false, Q, VV, GrB_NULL));
        GRB_TRY(GrB_Matrix_clear(acc));
        GRB_TRY(GrB_eWiseAdd(M_new, GrB_NULL, GrB_NULL, GrB_LOR, M_new, M_call, GrB_NULL));

        // M_new &= ~P
        GRB_TRY(GrB_assign(M_new, P, GrB_NULL, M_new, GrB_ALL, Q, GrB_ALL, VV, GrB_DESC_RSC));

        // P |= M_new
        GRB_TRY(GrB_eWiseAdd(P, GrB_NULL, GrB_NULL, GrB_LOR, P, M_new, GrB_NULL));

        GrB_Matrix swap = M;
        M = M_new;
        M_new = swap;

        GRB_TRY(GrB_Matrix_nvals(&m_nvals, M));
    }

    // Extract all reachable vertices
    {
        bool *vals = NULL;
        LG_TRY(LAGraph_Malloc((void **)&vals, num_sources, sizeof(bool), msg));
        for (size_t i = 0; i < num_sources; ++i)
            vals[i] = true;

        GrB_Info _bi =
            GrB_Vector_build_BOOL(source_mask, sources, vals, num_sources, GrB_SECOND_BOOL);
        LAGraph_Free((void **)&vals, GrB_NULL);
        GRB_TRY(_bi);
    }

    GRB_TRY(GrB_mxv(*reachable, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL,
                    graph_nt[start_nonterm], source_mask, GrB_DESC_T0));

    LG_FREE_WORK;
    return GrB_SUCCESS;
}