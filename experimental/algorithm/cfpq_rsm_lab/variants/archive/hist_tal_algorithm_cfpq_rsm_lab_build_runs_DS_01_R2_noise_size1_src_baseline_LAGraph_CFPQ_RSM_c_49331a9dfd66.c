//------------------------------------------------------------------------------
// cfpq_rsm_lab/variants/correctness.c: corrected CFPQ RSM reference (DS-00)
//------------------------------------------------------------------------------

// Parent source: experimental/algorithm/LAGraph_CFPQ_RSM.c at repository commit
// b1f7d708b62d4c7b13a3dfb98ad07b7ef0d6a5ee, sha256
// 2ed679bedd62f53bf45fdb89c1edd39f87faa94664d01deef6027032c3503ce7.
// Instruction revision: R2 (2026-11-01).
//
// R2 revision of the frozen R1 reference (variants/correctness.c, checksum
// bba6ee09...): adds input matrix/final-vector shape validation before any
// tuple extraction or native index read, and documents the fast-path
// precondition at the entry point.  The R1 file is preserved unchanged.
//
// Correctness/contract-only reference.  It is a full stand-in for
// LAGraph_CFPQ_RSM.c (the production file is left untouched) with the DS-00
// fixes described below.  The algorithm, its schedule, and the set of
// GraphBLAS kernels are unchanged; this file is the parent for every later
// ablation in cfpq_rsm_lab.
//
// DS-00 changes, keyed to the R1 task list:
//   (1) Phase 4 summary extraction no longer depends on incoming calls: every
//       box with a non-NULL final vector contributes its new summary edges,
//       including the start box when rsm_nonterm[start] is NULL.  The W*dG
//       return product is guarded separately because W[i] is NULL for a box
//       that can never be called.
//   (2) Separate destructors: GraphBLAS handles (graph_nt/W/rsm_start) are
//       released with GrB_free; ordinary term_src[k] buffers with
//       LAGraph_Free.  All matrix-array slots are zeroed at allocation with
//       LAGraph_Calloc, so a later allocation failure cannot reach cleanup with
//       uninitialized slots.  LG_FREE_ALL no longer dereferences a NULL
//       caller output pointer.
//   (3) Boundary validation: source bounds, start-state/entry bounds,
//       V*V / Q*V / Q*V*V overflow, and GraphBLAS dimension limits are checked
//       before any allocation.  Failure leaves *reachable NULL and every
//       caller-owned input intact.
//   (4) Contract boundary (no kernel change): the existing structural masks
//       are kept.  Products use LOR_LAND, so a stored false never becomes
//       true, but a stored false in an internal relation is structurally
//       present and can hide a later true entry.  That behavior predates
//       DS-00; stored false entries are therefore outside the fast-path
//       contract and are rejected by the construction-time validator in
//       tests/test_ds00.c.  A general-caller policy (reject / normalize /
//       define pattern semantics) is raised as Q-001, and a value-sensitive
//       visited subtraction would need its own termination design: a valued
//       complement mask lets a stored false frontier entry be regenerated on
//       every iteration.
//
// The semantic contract is unchanged: *reachable is a length-V BOOL vector
// whose true entries are exactly the destinations accepted from the sources.

#define FREE_MATRIX_ARRAY(array, len)                                                              \
    {                                                                                              \
        if ((array) != NULL)                                                                       \
        {                                                                                          \
            for (size_t _k = 0; _k < (len); ++_k)                                                  \
                LAGraph_Free((void **)&(array)[_k], NULL);                                         \
            LAGraph_Free((void **)&(array), NULL);                                                 \
        }                                                                                          \
    }

// Free an array of GraphBLAS handles.  Each owned handle is released with
// GrB_free, which is not interchangeable with LAGraph_Free: the GraphBLAS
// object owns internal arrays that only its destructor releases.
#define FREE_GRB_MATRIX_ARRAY(array, len)                                                          \
    {                                                                                              \
        if ((array) != NULL)                                                                       \
        {                                                                                          \
            for (size_t _k = 0; _k < (len); ++_k)                                                  \
                GrB_free(&(array)[_k]);                                                            \
            LAGraph_Free((void **)&(array), NULL);                                                 \
        }                                                                                          \
    }

#define LG_FREE_WORK                                                                               \
    {                                                                                              \
        GrB_free(&M);                                                                              \
        GrB_free(&P);                                                                              \
        GrB_free(&acc);                                                                            \
        GrB_free(&M_call);                                                                         \
        GrB_free(&M_new);                                                                          \
        GrB_free(&scratch);                                                                        \
        GrB_free(&s_temp);                                                                         \
        GrB_free(&rowvec);                                                                         \
        LAGraph_Free((void **)&term_active, NULL);                                                 \
        FREE_MATRIX_ARRAY(term_src, num_term);                                                     \
        LAGraph_Free((void **)&term_src_count, NULL);                                              \
        LAGraph_Free((void **)&active, NULL);                                                      \
        LAGraph_Free((void **)&active_idx, NULL);                                                  \
        LAGraph_Free((void **)&active_vals, NULL);                                                 \
        GrB_free(&new_edges);                                                                      \
        GrB_free(&diag_entry);                                                                     \
        GrB_free(&entry_vec);                                                                      \
        GrB_free(&source_mask);                                                                    \
        FREE_GRB_MATRIX_ARRAY(graph_nt, num_nonterm);                                              \
        FREE_GRB_MATRIX_ARRAY(W, num_nonterm);                                                     \
        FREE_GRB_MATRIX_ARRAY(rsm_start, num_nonterm);                                             \
    }

// The caller output pointer is nullable (it is checked by the first
// LG_ASSERT), so it must not be dereferenced by the cleanup macro.
#define LG_FREE_ALL                                                                                \
    {                                                                                              \
        LG_FREE_WORK;                                                                              \
        if (reachable != NULL)                                                                     \
            GrB_free(reachable);                                                                   \
    }

#include "LAGraph.h"
#include "LG_internal.h"
#include <GraphBLAS.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define H_TRY(expr)                                                                                \
    {                                                                                              \
        GrB_Info _hi = (expr);                                                                     \
        if (_hi != GrB_SUCCESS)                                                                    \
            return _hi;                                                                            \
    }

// Largest dimension accepted by the linked GraphBLAS.  SuiteSparse:GraphBLAS
// caps both dimensions at GB_NMAX = 2^60 (GxB_INDEX_MAX).  A vanilla
// GraphBLAS has no GxB_INDEX_MAX, so use the same conservative value; the
// integer checks below also keep every product inside int64_t, which is what
// GraphBLAS uses for nentries.
#if defined(GxB_SUITESPARSE_GRAPHBLAS)
#define CFPQ_MAX_DIM ((GrB_Index)GxB_INDEX_MAX)
#else
#define CFPQ_MAX_DIM ((GrB_Index)(1ULL << 60))
#endif

// true iff a * b does not fit in a value bounded by limit (limit > 0)
static inline bool s_mul_exceeds(GrB_Index a, GrB_Index b, GrB_Index limit)
{
    return (b != 0) && (a > limit / b);
}

// T = reshape(A' * M), QV x V.  A is Q x Q, M is Q x VV.
// T must be Q x VV on entry; its content is overwritten.
static GrB_Info s_shift(GrB_Matrix T, GrB_Matrix A, GrB_Matrix M, GrB_Index Q, GrB_Index V)
{
    H_TRY(GrB_mxm(T, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, A, M, GrB_DESC_T0));
    H_TRY(GxB_Matrix_reshape(T, false, Q * V, V, GrB_NULL));
    return GrB_SUCCESS;
}

// Restore T to an empty Q x VV matrix; its content is dead, so clear it first
static GrB_Info s_unshift(GrB_Matrix T, GrB_Index Q, GrB_Index V)
{
    H_TRY(GrB_Matrix_clear(T));
    H_TRY(GxB_Matrix_reshape(T, false, Q, V * V, GrB_NULL));
    return GrB_SUCCESS;
}

// C |= A * B, using one reused QV x V product temporary.
static GrB_Info s_mul_acc(GrB_Matrix C, GrB_Matrix A, GrB_Matrix B, GrB_Matrix T)
{
    H_TRY(GrB_Matrix_clear(T));
    H_TRY(GrB_mxm(T, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, A, B, GrB_NULL));
    H_TRY(GrB_eWiseAdd(C, GrB_NULL, GrB_NULL, GrB_LOR, C, T, GrB_NULL));
    return GrB_SUCCESS;
}

#undef H_TRY

typedef struct
{
    GrB_Index state_count;            // Q
    GrB_Index terminal_count;
    GrB_Index nonterminal_count;
    GrB_Index start_nonterminal;
    GrB_Matrix *terminal_matrices;    // [t]: Q x Q transitions labeled t, or NULL if none
    GrB_Matrix *nonterminal_matrices; // [i]: Q x Q transitions labeled i, or NULL if none
    GrB_Index *start_states;          // [i]: start state of the box of nonterminal i
    GrB_Vector *final_states;         // [i]: length-Q indicator of the final states of box i
} RSM;

// Fast-path contract (Q-001, resolved by the R2 review): every non-NULL input
// relation is a correctly shaped BOOL matrix (terminal/nonterminal Q x Q, graph
// V x V, final vectors length Q), the stored entries are true only (absence
// means false), and the RSM uses one globally consistent state numbering.
// This kernel does not reject stored false entries or non-BOOL matrices and
// does not support general value-sensitive relations: such callers are out of
// contract.  The test-only inputs_true_only() helper in tests/test_ds00.c is
// not library enforcement; a construction-time normalization helper is a
// separate, unreviewed proposal.  Output semantics are unchanged: *reachable is
// a length-V BOOL vector whose true entries are exactly the destinations
// accepted from the requested sources.
int LAGraph_CFPQ_RSM(GrB_Vector *reachable, const RSM *rsm, const GrB_Matrix *graph_term,
                     const GrB_Index *sources, size_t num_sources, GrB_Index V, char *msg)
{
    LG_CLEAR_MSG;

    // working matrices / vectors
    GrB_Matrix M = GrB_NULL;
    GrB_Matrix P = GrB_NULL;
    GrB_Matrix acc = GrB_NULL;
    GrB_Matrix M_call = GrB_NULL;
    GrB_Matrix M_new = GrB_NULL;
    GrB_Matrix scratch = GrB_NULL;
    GrB_Matrix s_temp = GrB_NULL; // QV x V product scratch for s_mul_acc
    GrB_Matrix new_edges = GrB_NULL;
    GrB_Matrix diag_entry = GrB_NULL;
    GrB_Vector entry_vec = GrB_NULL;
    GrB_Vector source_mask = GrB_NULL;
    GrB_Vector rowvec = GrB_NULL; // length-Q row activity of M

    GrB_Matrix *graph_nt = NULL;
    GrB_Matrix *W = NULL;         // QV x V, W[i] = (rsm_nonterm[i])^T * P
    GrB_Matrix *rsm_start = NULL; // Q x 1, rsm_start[S][i] = true, iff start_states[S] = i
    GrB_Index num_term = 0;
    GrB_Index num_nonterm = 0;

    // G=2 bookkeeping, declared before any assertion so cleanup is safe
    bool *term_active = NULL;
    GrB_Index **term_src = NULL;
    size_t *term_src_count = NULL;
    bool *active = NULL;
    GrB_Index *active_idx = NULL;
    bool *active_vals = NULL;

    LG_ASSERT(reachable != NULL, GrB_NULL_POINTER);
    *reachable = GrB_NULL;
    LG_ASSERT(rsm != NULL, GrB_NULL_POINTER);

    // shortcuts
    GrB_Index Q = rsm->state_count;
    num_term = rsm->terminal_count;
    num_nonterm = rsm->nonterminal_count;
    GrB_Index start_nonterm = rsm->start_nonterminal;
    GrB_Matrix *rsm_term = rsm->terminal_matrices;
    GrB_Matrix *rsm_nonterm = rsm->nonterminal_matrices;
    GrB_Index *start_states = rsm->start_states;
    GrB_Vector *final_states = rsm->final_states;

    // A count of zero makes the corresponding array optional.
    LG_ASSERT(rsm_term != NULL || num_term == 0, GrB_NULL_POINTER);
    LG_ASSERT(rsm_nonterm != NULL, GrB_NULL_POINTER);
    LG_ASSERT(start_states != NULL, GrB_NULL_POINTER);
    LG_ASSERT(final_states != NULL, GrB_NULL_POINTER);
    LG_ASSERT(graph_term != NULL || num_term == 0, GrB_NULL_POINTER);
    LG_ASSERT(sources != NULL || num_sources == 0, GrB_NULL_POINTER);
    LG_ASSERT(start_nonterm < num_nonterm, GrB_INVALID_VALUE);
    LG_ASSERT(num_sources <= V, GrB_INVALID_VALUE);

    //--------------------------------------------------------------------------
    // dimension, index, and overflow validation (before any allocation)
    //--------------------------------------------------------------------------

    // V*V is the column count of M/scratch and the length of the flat
    // new_edges buffer; Q*V is the row count of acc/W/s_temp; Q*V*V is the
    // number of entries those matrices can address.  All three are computed
    // from the caller's Q and V, so they are checked here rather than trusted.
    LG_ASSERT_MSG(!s_mul_exceeds(V, V, CFPQ_MAX_DIM), GrB_INVALID_VALUE,
                  "V*V exceeds the GraphBLAS dimension limit");
    LG_ASSERT_MSG(!s_mul_exceeds(Q, V, CFPQ_MAX_DIM), GrB_INVALID_VALUE,
                  "Q*V exceeds the GraphBLAS dimension limit");
    LG_ASSERT_MSG(!s_mul_exceeds(V, V, (GrB_Index)INT64_MAX), GrB_INVALID_VALUE,
                  "V*V does not fit in int64_t");
    LG_ASSERT_MSG(Q == 0 || V == 0 || Q <= ((GrB_Index)INT64_MAX) / V / V, GrB_INVALID_VALUE,
                  "Q*V*V does not fit in int64_t");

    // sources are flattened as s*V+s, so they must be validated before that
    // product is formed; an out-of-range source would otherwise wrap.
    for (size_t i = 0; i < num_sources; ++i)
    {
        LG_ASSERT_MSG(sources[i] < V, GrB_INVALID_INDEX, "source vertex out of range");
    }

    // start states are written into a Q x 1 matrix; validate every entry the
    // kernel actually reads (all called boxes plus the start box).
    LG_ASSERT_MSG(start_states[start_nonterm] < Q, GrB_INVALID_INDEX,
                  "start state of the start box out of range");
    for (size_t i = 0; i < num_nonterm; ++i)
    {
        if (rsm_nonterm[i] != GrB_NULL)
        {
            LG_ASSERT_MSG(start_states[i] < Q, GrB_INVALID_INDEX,
                          "start state of a called box out of range");
        }
    }

    //--------------------------------------------------------------------------
    // input matrix and final-vector shape validation (metadata only)
    //--------------------------------------------------------------------------

    // These checks must precede every tuple extraction and native index read.
    // In particular the terminal row indices are extracted into term_src[] and
    // used to index active[Q]; a (Q+1) x (Q+1) terminal matrix with an entry in
    // row Q would otherwise be an out-of-bounds read before mxm can report a
    // dimension error.  NULL operands are allowed (they mean "no entries"), but
    // every non-NULL operand is validated, including empty or inactive ones, so
    // skipped work cannot hide an invalid shape.
    for (size_t i = 0; i < num_term; ++i)
    {
        if (rsm_term[i] != GrB_NULL)
        {
            GrB_Index nr = 0, nc = 0;
            GRB_TRY(GrB_Matrix_nrows(&nr, rsm_term[i]));
            GRB_TRY(GrB_Matrix_ncols(&nc, rsm_term[i]));
            LG_ASSERT_MSG(nr == Q && nc == Q, GrB_DIMENSION_MISMATCH,
                          "terminal transition matrix is not Q x Q");
        }
        if (graph_term[i] != GrB_NULL)
        {
            GrB_Index nr = 0, nc = 0;
            GRB_TRY(GrB_Matrix_nrows(&nr, graph_term[i]));
            GRB_TRY(GrB_Matrix_ncols(&nc, graph_term[i]));
            LG_ASSERT_MSG(nr == V && nc == V, GrB_DIMENSION_MISMATCH,
                          "graph matrix is not V x V");
        }
    }
    for (size_t i = 0; i < num_nonterm; ++i)
    {
        if (rsm_nonterm[i] != GrB_NULL)
        {
            GrB_Index nr = 0, nc = 0;
            GRB_TRY(GrB_Matrix_nrows(&nr, rsm_nonterm[i]));
            GRB_TRY(GrB_Matrix_ncols(&nc, rsm_nonterm[i]));
            LG_ASSERT_MSG(nr == Q && nc == Q, GrB_DIMENSION_MISMATCH,
                          "nonterminal transition matrix is not Q x Q");
        }
        if (final_states[i] != GrB_NULL)
        {
            GrB_Index n = 0;
            GRB_TRY(GrB_Vector_size(&n, final_states[i]));
            LG_ASSERT_MSG(n == Q, GrB_DIMENSION_MISMATCH, "final vector length is not Q");
        }
    }

    //--------------------------------------------------------------------------
    // allocate the per-box arrays
    //--------------------------------------------------------------------------

    // LAGraph_Calloc matters for cleanup: if any of these allocations fails,
    // LG_FREE_ALL walks the array with the full count and must never see an
    // uninitialized slot.
    LG_TRY(LAGraph_Calloc((void **)&graph_nt, num_nonterm, sizeof(GrB_Matrix), msg));
    LG_TRY(LAGraph_Calloc((void **)&W, num_nonterm, sizeof(GrB_Matrix), msg));
    LG_TRY(LAGraph_Calloc((void **)&rsm_start, num_nonterm, sizeof(GrB_Matrix), msg));

    for (size_t i = 0; i < num_nonterm; ++i)
    {
        GRB_TRY(GrB_Matrix_new(&graph_nt[i], GrB_BOOL, V, V));
        if (rsm_nonterm[i] == GrB_NULL)
            continue;

        GRB_TRY(GrB_Matrix_new(&W[i], GrB_BOOL, Q * V, V));
        GRB_TRY(GrB_Matrix_new(&rsm_start[i], GrB_BOOL, Q, 1));
        GRB_TRY(GrB_Matrix_setElement_BOOL(rsm_start[i], true, start_states[i], 0));
    }

    // allocate working matrices
    GRB_TRY(GrB_Matrix_new(&M, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&acc, GrB_BOOL, Q*V, V));
    GRB_TRY(GrB_Matrix_new(&M_call, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&scratch, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&s_temp, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&new_edges, GrB_BOOL, 1, V * V));
    // there is no need to create matrix for diag_entry
    GRB_TRY(GrB_Vector_new(&entry_vec, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(&source_mask, GrB_BOOL, V));

    // G=2: static per-terminal emptiness and source-state patterns, computed
    // once per call.  Terminals with no entries in either operand are never
    // shifted; the remaining chains are predicted from active rows of M.
    if (num_term > 0)
    {
        LG_TRY(LAGraph_Calloc((void **) &term_active, num_term, sizeof(bool), msg));
        LG_TRY(LAGraph_Calloc((void **) &term_src, num_term, sizeof(GrB_Index *), msg));
        LG_TRY(LAGraph_Calloc((void **) &term_src_count, num_term, sizeof(size_t), msg));

        for (size_t i = 0; i < num_term; ++i)
        {
            if (rsm_term[i] == GrB_NULL || graph_term[i] == GrB_NULL)
                continue;

            GrB_Index rn = 0, gn = 0;
            GRB_TRY(GrB_Matrix_nvals(&rn, rsm_term[i]));
            GRB_TRY(GrB_Matrix_nvals(&gn, graph_term[i]));
            if (rn == 0 || gn == 0)
                continue;

            term_active[i] = true;
            LG_TRY(LAGraph_Malloc((void **) &term_src[i], rn, sizeof(GrB_Index), msg));
            GrB_Index rn2 = rn;
            GRB_TRY(GrB_Matrix_extractTuples_BOOL(term_src[i], NULL, NULL, &rn2, rsm_term[i]));
            term_src_count[i] = (size_t) rn2;
        }
    }

    // G=2: row-active prediction scratch, plus the dense Q-entry lookup.
    if (Q > 0)
    {
        GRB_TRY(GrB_Vector_new(&rowvec, GrB_BOOL, Q));
        LG_TRY(LAGraph_Malloc((void **) &active, Q, sizeof(bool), msg));
        LG_TRY(LAGraph_Malloc((void **) &active_idx, Q, sizeof(GrB_Index), msg));
        LG_TRY(LAGraph_Malloc((void **) &active_vals, Q, sizeof(bool), msg));
        for (GrB_Index i = 0; i < Q; ++i)
            active[i] = false;
    }

    // each source s starts a call to the start nonterminal at (s, s)
    GrB_Index start_state = start_states[start_nonterm];
    for (size_t i = 0; i < num_sources; ++i)
    {
        GrB_Index s = sources[i];
        GRB_TRY(GrB_Matrix_setElement_BOOL(M, true, start_state, s * V + s));
        GRB_TRY(GrB_Vector_setElement_BOOL(source_mask, true, s));
    }
    GRB_TRY(GrB_Matrix_dup(&P, M));

    GrB_Index m_nvals = 0;
    GRB_TRY(GrB_Matrix_nvals(&m_nvals, M));
    while (m_nvals > 0)
    {
        // G=2: mark rows of M that currently contain any true entry.
        if (Q > 0)
        {
            GRB_TRY(GrB_Vector_clear(rowvec));
            GRB_TRY(GrB_reduce(rowvec, GrB_NULL, GrB_NULL, GrB_LOR_MONOID_BOOL, M, GrB_NULL));
            GrB_Index nactive = 0;
            GRB_TRY(GrB_Vector_nvals(&nactive, rowvec));
            GrB_Index nactive2 = nactive;
            GRB_TRY(GrB_Vector_extractTuples_BOOL(active_idx, active_vals, &nactive2, rowvec));
            memset(active, 0, (size_t) Q * sizeof(bool));
            for (GrB_Index k = 0; k < nactive2; ++k)
                active[active_idx[k]] = true;
        }

        // Phase 1: terminal transitions.  T = A_i^T * M is nonempty only if
        // some source state of A_i is an active row of M, so whole chains can
        // be skipped before the shift.
        for (size_t i = 0; i < num_term; ++i)
        {
            if (!term_active[i])
                continue;

            bool predicted = false;
            for (size_t k = 0; k < term_src_count[i]; ++k)
            {
                if (active[term_src[i][k]])
                {
                    predicted = true;
                    break;
                }
            }
            if (!predicted)
                continue;

            LG_TRY(s_shift(scratch, rsm_term[i], M, Q, V));
            GrB_Index tn = 0;
            GRB_TRY(GrB_Matrix_nvals(&tn, scratch));
            if (tn > 0)
                LG_TRY(s_mul_acc(acc, scratch, graph_term[i], s_temp));
            LG_TRY(s_unshift(scratch, Q, V));
        }

        // Phases 2+3: nonterminal transitions and calls
        GRB_TRY(GrB_Matrix_clear(M_call));
        for (size_t i = 0; i < num_nonterm; ++i)
        {
            if (rsm_nonterm[i] == GrB_NULL)
                continue;

            // scratch = reshape(R_A^T * M), QV x V
            LG_TRY(s_shift(scratch, rsm_nonterm[i], M, Q, V));

            GrB_Index x_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&x_nvals, scratch));
            if (x_nvals > 0)
            {
                // call: every vertex v where X is nonzero enters nonterminal i
                // at (v, v), in its start state
                GRB_TRY(GrB_reduce(entry_vec, GrB_NULL, GrB_NULL, GrB_LOR_MONOID_BOOL, scratch,
                                   GrB_DESC_T0));
                GRB_TRY(GrB_Matrix_diag(&diag_entry, entry_vec, 0)); // V x V
                GRB_TRY(GxB_Matrix_reshape(diag_entry, false, 1, V * V, GrB_NULL));
                GRB_TRY(GrB_mxm(M_call, GrB_NULL, GrB_LOR, GrB_LOR_LAND_SEMIRING_BOOL,
                                rsm_start[i], diag_entry, GrB_NULL));
                GRB_TRY(GrB_free(&diag_entry));

                // skip over nonterminal i using the summary edges
                GrB_Index gnt_nvals = 0;
                GRB_TRY(GrB_Matrix_nvals(&gnt_nvals, graph_nt[i]));
                if (gnt_nvals > 0)
                    LG_TRY(s_mul_acc(acc, scratch, graph_nt[i], s_temp));

                // RETURN bookkeeping: W[i] == reshape(R_i' * P)
                GRB_TRY(GrB_eWiseAdd(W[i], GrB_NULL, GrB_NULL, GrB_LOR, W[i], scratch, GrB_NULL));
            }
            LG_TRY(s_unshift(scratch, Q, V));
        }

        // Phase 4: return transitions.  A box with a non-NULL final vector
        // contributes summary edges even if it has no incoming call
        // transitions (rsm_nonterm[i] == NULL); this is what makes the start
        // box's summary -- and epsilon acceptance at its entry state -- visible
        // to the final extraction.  The return product W[i]*dG is a separate
        // step because W[i] is absent exactly when no call can ever wait for
        // this box's return.
        for (size_t i = 0; i < num_nonterm; ++i)
        {
            if (final_states[i] == GrB_NULL)
                continue;

            // 1 x Q * Q x VV -> 1 x VV
            GRB_TRY(GrB_mxm(new_edges, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL,
                            (GrB_Matrix)final_states[i], M, GrB_DESC_T0));

            GrB_Index ne_vals = 0;
            GRB_TRY(GrB_Matrix_nvals(&ne_vals, new_edges));
            if (ne_vals == 0)
                continue;

            GRB_TRY(GxB_Matrix_reshape(new_edges, false, V, V, GrB_NULL));

            // dG = new_edges \ graph_nt[i].  The structural mask is the
            // pre-existing behavior: stored false entries are outside the
            // true-only fast-path contract, and a value-sensitive mask would
            // need a separate visited-subtraction design (see DS-00 report).
            //
            // An empty graph_nt[i] needs no subtraction.  Besides skipping
            // useless work, this avoids a SuiteSparse:GraphBLAS v10.5.2 edge
            // case: GrB_apply with GrB_DESC_RSC on a 1x1 output whose mask has
            // no entries returns GrB_DIMENSION_MISMATCH, which would make the
            // valid V=1 first-iteration case fail.  With a non-empty mask the
            // apply is used and is exact.
            GrB_Index gnt_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&gnt_nvals, graph_nt[i]));
            if (gnt_nvals > 0)
            {
                GRB_TRY(GrB_apply(new_edges, graph_nt[i], GrB_NULL, GrB_IDENTITY_BOOL, new_edges,
                                  GrB_DESC_RSC));
                GRB_TRY(GrB_Matrix_nvals(&ne_vals, new_edges));
                if (ne_vals == 0)
                {
                    GRB_TRY(GxB_Matrix_reshape(new_edges, false, 1, V*V, GrB_NULL));
                    continue;
                }
            }

            GRB_TRY(GrB_eWiseAdd(graph_nt[i], GrB_NULL, GrB_NULL, GrB_LOR, graph_nt[i], new_edges,
                                 GrB_NULL));

            // acc |= W * dG, where W = R_A^T * P; skip when W is absent (box
            // never called) or has no waiting configuration yet.
            if (W[i] != GrB_NULL)
            {
                GrB_Index w_nvals = 0;
                GRB_TRY(GrB_Matrix_nvals(&w_nvals, W[i]));
                if (w_nvals > 0)
                    LG_TRY(s_mul_acc(acc, W[i], new_edges, s_temp));
            }

            GRB_TRY(GxB_Matrix_reshape(new_edges, false, 1, V * V, GrB_NULL));
        }

        // Phase 5: M_new = (reshape(acc) | M_call) \ P.  A structural visited
        // subtraction is required for termination: a false structural entry in
        // P must not be reintroduced, and values are true-only by contract.
        GRB_TRY(GrB_free(&M_new));
        GRB_TRY(GxB_Matrix_reshapeDup(&M_new, acc, false, Q, V * V, GrB_NULL));
        GRB_TRY(GrB_Matrix_clear(acc));
        GRB_TRY(GrB_eWiseAdd(M_new, P, GrB_NULL, GrB_LOR, M_new, M_call, GrB_DESC_RSC));
        GRB_TRY(GrB_eWiseAdd(P, GrB_NULL, GrB_NULL, GrB_LOR, P, M_new, GrB_NULL));

        GrB_Matrix swap = M;
        M = M_new;
        M_new = swap;

        GRB_TRY(GrB_Matrix_nvals(&m_nvals, M));
    }

    // reachable(w) = OR_s source_mask(s) & graph_nt[start](s, w)
    GRB_TRY(GrB_Vector_new(reachable, GrB_BOOL, V));
    GRB_TRY(GrB_mxv(*reachable, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL,
                    graph_nt[start_nonterm], source_mask, GrB_DESC_T0));

    LG_FREE_WORK;
    return GrB_SUCCESS;
}
