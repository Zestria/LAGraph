#define FREE_MATRIX_ARRAY(array, len)                                                              \
    {                                                                                              \
        if ((array) != NULL)                                                                       \
        {                                                                                          \
            for (size_t _k = 0; _k < (len); ++_k)                                                  \
                LAGraph_Free((void **)&(array)[_k], NULL);                                         \
            LAGraph_Free((void **)&(array), NULL);                                                 \
        }                                                                                          \
    }

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
        GrB_free(&new_edges);                                                                      \
        GrB_free(&entry_vec);                                                                      \
        GrB_free(&source_mask);                                                                    \
        LAGraph_Free((void **)&call_i, NULL);                                                      \
        LAGraph_Free((void **)&call_j, NULL);                                                      \
        LAGraph_Free((void **)&call_x, NULL);                                                      \
        LAGraph_Free((void **)&extract_idx, NULL);                                                 \
        FREE_GRB_MATRIX_ARRAY(graph_nt, num_nonterm);                                              \
        s_waitboxes_free(&waitboxes, num_nonterm);                                                \
        GrB_free(&gbuild);                                                                         \
        s_tuples_free(&atup);                                                     \
    }

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
    H_TRY(GrB_mxm(T, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL, A, M, GrB_DESC_T0));
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

// C |= A * B, using one reused QV x V product temporary.  The first nonempty
// product swaps into C instead of being unioned; later products use the union.
static GrB_Info s_mul_acc(GrB_Matrix *C, GrB_Matrix *T, GrB_Matrix A, GrB_Matrix B,
                          bool *acc_empty)
{
    H_TRY(GrB_mxm(*T, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL, A, B, GrB_NULL));

    GrB_Index tn = 0;
    H_TRY(GrB_Matrix_nvals(&tn, *T));
    if (tn == 0)
        return GrB_SUCCESS;

    if (*acc_empty)
    {
        GrB_Matrix swap = *C;
        *C = *T;
        *T = swap;
        *acc_empty = false;
        return GrB_SUCCESS;
    }

    H_TRY(GrB_eWiseAdd(*C, GrB_NULL, GrB_NULL, GrB_LOR, *C, *T, GrB_NULL));
    return GrB_SUCCESS;
}

#define S_NO_STATE ((GrB_Index)-1)
// reusable tuple arrays (both GrB_Index and Boolean value arrays)
typedef struct
{
    GrB_Index *i;
    GrB_Index *j;
    bool *x;
    GrB_Index cap;
} S_TupleBuf;

static void s_tuples_free(S_TupleBuf *b)
{
    LAGraph_Free((void **)&b->i, NULL);
    LAGraph_Free((void **)&b->j, NULL);
    LAGraph_Free((void **)&b->x, NULL);
    b->cap = 0;
}

static GrB_Info s_tuples_reserve(S_TupleBuf *b, GrB_Index need, char *msg)
{
    if (need <= b->cap)
        return GrB_SUCCESS;
    if (need > SIZE_MAX / sizeof(GrB_Index))
        return GrB_OUT_OF_MEMORY;

    GrB_Index cap = (b->cap == 0) ? 64 : b->cap;
    while (cap < need)
    {
        if (cap > (GrB_Index)(SIZE_MAX / (8 * sizeof(GrB_Index))))
        {
            cap = need;
            break;
        }
        cap *= 2;
    }

    int st;
    st = LAGraph_Realloc((void **)&b->i, (size_t)cap, (size_t)b->cap, sizeof(GrB_Index), msg);
    if (st < 0)
        return (GrB_Info)st;
    st = LAGraph_Realloc((void **)&b->j, (size_t)cap, (size_t)b->cap, sizeof(GrB_Index), msg);
    if (st < 0)
        return (GrB_Info)st;
    st = LAGraph_Realloc((void **)&b->x, (size_t)cap, (size_t)b->cap, sizeof(bool), msg);
    if (st < 0)
        return (GrB_Info)st;

    b->cap = cap;
    return GrB_SUCCESS;
}

// allocation helpers for the setup helper, which has its own cleanup label
#define S_ALLOC(ptr, nitems, size)                                                                 \
    {                                                                                              \
        int _sst = LAGraph_Malloc((void **)&(ptr), (size_t)(nitems), (size_t)(size), NULL);        \
        if (_sst < 0)                                                                              \
        {                                                                                          \
            info = (GrB_Info)_sst;                                                                 \
            goto cleanup;                                                                          \
        }                                                                                          \
    }
#define S_CALLOC(ptr, nitems, size)                                                                \
    {                                                                                              \
        int _sst = LAGraph_Calloc((void **)&(ptr), (size_t)(nitems), (size_t)(size), NULL);        \
        if (_sst < 0)                                                                              \
        {                                                                                          \
            info = (GrB_Info)_sst;                                                                 \
            goto cleanup;                                                                          \
        }                                                                                          \
    }
#define S_FREE(ptr) LAGraph_Free((void **)&(ptr), NULL)

#define S_TRY(expr)                                                                                \
    {                                                                                              \
        GrB_Info _stry = (expr);                                                                   \
        if (_stry != GrB_SUCCESS)                                                                  \
        {                                                                                          \
            info = _stry;                                                                          \
            goto cleanup;                                                                          \
        }                                                                                          \
    }

// C |= delta with the same first-contribution handle swap as s_mul_acc.
static GrB_Info s_acc_merge(GrB_Matrix *C, GrB_Matrix *delta, bool *acc_empty)
{
    GrB_Index n = 0;
    H_TRY(GrB_Matrix_nvals(&n, *delta));
    if (n == 0)
        return GrB_SUCCESS;

    if (*acc_empty)
    {
        GrB_Matrix swap = *C;
        *C = *delta;
        *delta = swap;
        *acc_empty = false;
    }
    else
    {
        H_TRY(GrB_eWiseAdd(*C, GrB_NULL, GrB_NULL, GrB_LOR, *C, *delta, GrB_NULL));
    }

    H_TRY(GrB_Matrix_clear(*delta));
    return GrB_SUCCESS;
}

typedef struct
{
    GrB_Index d;        // number of distinct return destinations
    GrB_Index *dest;    // [d] compact destination state D_A[l]
    GrB_Matrix route;   // Q x d, route[q,l] = R_A[q, dest[l]]
    GrB_Matrix x;       // d x V*V on entry, d*V x V while in use
    GrB_Matrix w;       // d*V x V waiting prefixes
    GrB_Matrix p;       // d*V x V local product scratch
} S_WaitBox;

static void s_waitboxes_free(S_WaitBox **wbp, GrB_Index n)
{
    S_WaitBox *wb = *wbp;
    if (wb == NULL)
        return;
    for (GrB_Index i = 0; i < n; ++i)
    {
        LAGraph_Free((void **)&wb[i].dest, NULL);
        GrB_free(&wb[i].route);
        GrB_free(&wb[i].x);
        GrB_free(&wb[i].w);
        GrB_free(&wb[i].p);
    }
    LAGraph_Free((void **)wbp, NULL);
}

static GrB_Info s_waitboxes_init(S_WaitBox *wb, GrB_Index n, GrB_Index Q, GrB_Index V,
                                 GrB_Matrix *rsm_nonterm, char *msg)
{
    GrB_Info info = GrB_SUCCESS;
    GrB_Index *seen = NULL;
    GrB_Index *dest_tmp = NULL;
    GrB_Index *lcol = NULL;
    GrB_Index maxn = 0;
    S_TupleBuf ttup;
    memset(&ttup, 0, sizeof(ttup));

    for (GrB_Index i = 0; i < n; ++i)
    {
        if (rsm_nonterm[i] == GrB_NULL)
            continue;
        GrB_Index rn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_nonterm[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        if (rn > maxn)
            maxn = rn;
    }
    S_CALLOC(seen, Q, sizeof(GrB_Index));
    for (GrB_Index q = 0; q < Q; ++q)
        seen[q] = S_NO_STATE;
    S_ALLOC(dest_tmp, maxn, sizeof(GrB_Index));
    S_ALLOC(lcol, maxn, sizeof(GrB_Index));
    S_TRY(s_tuples_reserve(&ttup, maxn, msg));

    for (GrB_Index i = 0; i < n; ++i)
    {
        if (rsm_nonterm[i] == GrB_NULL)
            continue;
        GrB_Index rn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_nonterm[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        if (rn == 0)
            continue;
        GrB_Index nn = rn;
        info = GrB_Matrix_extractTuples_BOOL(ttup.i, ttup.j, ttup.x, &nn, rsm_nonterm[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;

        GrB_Index d = 0;
        for (GrB_Index k = 0; k < nn; ++k)
        {
            GrB_Index to = ttup.j[k];
            if (seen[to] == S_NO_STATE)
            {
                seen[to] = d;
                dest_tmp[d] = to;
                d++;
            }
            lcol[k] = seen[to];
        }
        for (GrB_Index l = 0; l < d; ++l)
            seen[dest_tmp[l]] = S_NO_STATE;

        if (d == 0)
            continue;
        if (s_mul_exceeds(d, V, CFPQ_MAX_DIM))
        {
            info = GrB_INVALID_VALUE;
            goto cleanup;
        }

        S_ALLOC(wb[i].dest, d, sizeof(GrB_Index));
        memcpy(wb[i].dest, dest_tmp, (size_t)d * sizeof(GrB_Index));
        wb[i].d = d;

        info = GrB_Matrix_new(&wb[i].route, GrB_BOOL, Q, d);
        if (info != GrB_SUCCESS)
            goto cleanup;
        for (GrB_Index k = 0; k < nn; ++k)
        {
            ttup.j[k] = lcol[k];
            ttup.x[k] = true;
        }
        info = GrB_Matrix_build_BOOL(wb[i].route, ttup.i, ttup.j, ttup.x, nn, GrB_LOR);
        if (info != GrB_SUCCESS)
            goto cleanup;

        info = GrB_Matrix_new(&wb[i].x, GrB_BOOL, d, V * V);
        if (info != GrB_SUCCESS)
            goto cleanup;
        info = GrB_Matrix_new(&wb[i].w, GrB_BOOL, d * V, V);
        if (info != GrB_SUCCESS)
            goto cleanup;
        info = GrB_Matrix_new(&wb[i].p, GrB_BOOL, d * V, V);
        if (info != GrB_SUCCESS)
            goto cleanup;
    }

cleanup:
    S_FREE(seen);
    S_FREE(dest_tmp);
    S_FREE(lcol);
    s_tuples_free(&ttup);
    return info;
}

// X = reshape(B^T * M), d*V x V
static GrB_Info s_shift_compact(GrB_Matrix X, GrB_Matrix B, GrB_Matrix M, GrB_Index d,
                                GrB_Index V)
{
    H_TRY(GrB_mxm(X, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL, B, M, GrB_DESC_T0));
    H_TRY(GxB_Matrix_reshape(X, false, d * V, V, GrB_NULL));
    return GrB_SUCCESS;
}

// restore X to an empty d x V*V matrix for the next sweep
static GrB_Info s_unshift_compact(GrB_Matrix X, GrB_Index d, GrB_Index V)
{
    H_TRY(GrB_Matrix_clear(X));
    H_TRY(GxB_Matrix_reshape(X, false, d, V * V, GrB_NULL));
    return GrB_SUCCESS;
}

// Scatter local rows l*V+u of a d*V x V product to global rows dest[l]*V+u.
static GrB_Info s_scatter_local(GrB_Matrix local, GrB_Index d, const GrB_Index *dest,
                                GrB_Index V, GrB_Matrix *gbuild, S_TupleBuf *tup,
                                GrB_Matrix *acc, bool *acc_empty, char *msg)
{
    GrB_Index n = 0;
    H_TRY(GrB_Matrix_nvals(&n, local));
    if (n == 0)
        return GrB_SUCCESS;
    if (d == 1)
    {
        // A single continuation is a contiguous row block. GraphBLAS can
        // scatter it directly without exporting and rebuilding every tuple.
        GrB_Index rows[2] = {dest[0] * V, (dest[0] + 1) * V - 1};
        H_TRY(GrB_Matrix_clear(*gbuild));
        H_TRY(GrB_Matrix_assign(*gbuild, GrB_NULL, GrB_NULL, local,
                                rows, GxB_RANGE, GrB_ALL, V, GrB_NULL));
        return s_acc_merge(acc, gbuild, acc_empty);
    }
    H_TRY(s_tuples_reserve(tup, n, msg));
    H_TRY(GrB_Matrix_extractTuples_BOOL(tup->i, tup->j, tup->x, &n, local));
    for (GrB_Index k = 0; k < n; ++k)
    {
        GrB_Index lr = tup->i[k];
        GrB_Index l = lr / V;
        GrB_Index u = lr % V;
        if (l >= d)
            return GrB_INVALID_INDEX;
        tup->i[k] = dest[l] * V + u;
        tup->x[k] = true;
    }
    H_TRY(GrB_Matrix_clear(*gbuild));
    H_TRY(GrB_Matrix_build_BOOL(*gbuild, tup->i, tup->j, tup->x, n, GrB_LOR));
    return s_acc_merge(acc, gbuild, acc_empty);
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

int LAGraph_CFPQ_RSM(GrB_Vector *reachable, const RSM *rsm, const GrB_Matrix *graph_term,
                     const GrB_Index *sources, size_t num_sources, GrB_Index V, char *msg)
{
    LG_CLEAR_MSG;

    // working matrices / vectors
    GrB_Matrix M = GrB_NULL;
    GrB_Matrix P = GrB_NULL;
    GrB_Matrix acc = GrB_NULL;
    bool acc_empty = true;
    GrB_Matrix M_call = GrB_NULL;
    GrB_Matrix M_new = GrB_NULL;
    GrB_Matrix scratch = GrB_NULL;
    GrB_Matrix s_temp = GrB_NULL; // QV x V product scratch for s_mul_acc
    GrB_Matrix new_edges = GrB_NULL;
    GrB_Vector entry_vec = GrB_NULL;
    GrB_Vector source_mask = GrB_NULL;
    GrB_Vector rowvec = GrB_NULL; // length-Q row activity of M

    // reusable call-tuple buffers for the direct flat-layout M_call build
    GrB_Index *call_i = NULL;
    GrB_Index *call_j = NULL;
    bool *call_x = NULL;
    GrB_Index call_count = 0;
    GrB_Index call_cap = 0;
    GrB_Index *extract_idx = NULL;
    GrB_Index extract_cap = 0;

    GrB_Matrix *graph_nt = NULL;
    S_WaitBox *waitboxes = NULL;
    GrB_Matrix gbuild = GrB_NULL;
    S_TupleBuf atup = {0};
    GrB_Index num_term = 0;
    GrB_Index num_nonterm = 0;

    bool *term_active = NULL;
    GrB_Index **term_src = NULL;
    size_t *term_src_count = NULL;
    bool *active = NULL;
    GrB_Index *active_idx = NULL;

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

    LG_ASSERT_MSG(!s_mul_exceeds(V, V, CFPQ_MAX_DIM), GrB_INVALID_VALUE,
                  "V*V exceeds the GraphBLAS dimension limit");
    LG_ASSERT_MSG(!s_mul_exceeds(Q, V, CFPQ_MAX_DIM), GrB_INVALID_VALUE,
                  "Q*V exceeds the GraphBLAS dimension limit");

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
    // The valid start state also guarantees Q > 0 for all subsequent work.
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

    LG_TRY(LAGraph_Calloc((void **)&graph_nt, num_nonterm, sizeof(GrB_Matrix), msg));
    LG_TRY(LAGraph_Calloc((void **)&waitboxes, num_nonterm, sizeof(S_WaitBox), msg));

    for (size_t i = 0; i < num_nonterm; ++i)
    {
        GRB_TRY(GrB_Matrix_new(&graph_nt[i], GrB_BOOL, V, V));

    }

    LG_TRY(s_waitboxes_init(waitboxes, num_nonterm, Q, V, rsm_nonterm, msg));
    GRB_TRY(GrB_Matrix_new(&gbuild, GrB_BOOL, Q * V, V));

    // allocate working matrices
    GRB_TRY(GrB_Matrix_new(&M, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&acc, GrB_BOOL, Q*V, V));
    GRB_TRY(GrB_Matrix_new(&M_call, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&scratch, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&s_temp, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&new_edges, GrB_BOOL, 1, V * V));
    GRB_TRY(GrB_Vector_new(&entry_vec, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(&source_mask, GrB_BOOL, V));

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
            GRB_TRY(GrB_Matrix_extractTuples_BOOL(term_src[i], NULL, NULL, &rn, rsm_term[i]));
            term_src_count[i] = (size_t) rn;
        }
    }

    GRB_TRY(GrB_Vector_new(&rowvec, GrB_BOOL, Q));
    LG_TRY(LAGraph_Malloc((void **) &active, Q, sizeof(bool), msg));
    LG_TRY(LAGraph_Malloc((void **) &active_idx, Q, sizeof(GrB_Index), msg));

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
        // mark rows of M that currently contain any true entry.
        GRB_TRY(GrB_reduce(rowvec, GrB_NULL, GrB_NULL, GrB_LOR_MONOID_BOOL, M, GrB_NULL));
        GrB_Index nactive = Q;
        GRB_TRY(GrB_Vector_extractTuples_BOOL(active_idx, NULL, &nactive, rowvec));
        memset(active, 0, (size_t) Q * sizeof(bool));
        for (GrB_Index k = 0; k < nactive; ++k)
            active[active_idx[k]] = true;

        // Phase 1: terminal transitions. T = A_i^T * M is nonempty only if
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
            LG_TRY(s_mul_acc(&acc, &s_temp, scratch, graph_term[i], &acc_empty));
            LG_TRY(s_unshift(scratch, Q, V));
        }

        // Phases 2+3: nonterminal transitions and calls.  Each callee uses only
        // its distinct return destinations; local rows are scattered through
        // waitboxes[i].dest.
        GRB_TRY(GrB_Matrix_clear(M_call));
        call_count = 0;
        for (size_t i = 0; i < num_nonterm; ++i)
        {
            S_WaitBox *wb = &waitboxes[i];
            if (rsm_nonterm[i] == GrB_NULL || wb->d == 0)
                continue;

            // x = reshape(B_A^T * M), d*V x V
            LG_TRY(s_shift_compact(wb->x, wb->route, M, wb->d, V));

            GrB_Index x_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&x_nvals, wb->x));
            if (x_nvals > 0)
            {
                // call: every vertex v where X is nonzero enters nonterminal i
                // at (v, v), in its start state.  Collect the tuples and build
                // M_call once after the loop.
                GRB_TRY(GrB_reduce(entry_vec, GrB_NULL, GrB_NULL, GrB_LOR_MONOID_BOOL, wb->x,
                                   GrB_DESC_T0));
                GrB_Index entry_nvals = 0;
                GRB_TRY(GrB_Vector_nvals(&entry_nvals, entry_vec));
                // A nonempty x has at least one active column.
                if (entry_nvals > extract_cap)
                {
                    LG_TRY(LAGraph_Realloc((void **) &extract_idx, (size_t) entry_nvals,
                                           (size_t) extract_cap, sizeof(GrB_Index), msg));
                    extract_cap = entry_nvals;
                }
                GRB_TRY(GrB_Vector_extractTuples_BOOL(extract_idx, NULL, &entry_nvals,
                                                      entry_vec));

                if (call_count + entry_nvals > call_cap)
                {
                    GrB_Index new_cap = (call_count + entry_nvals) * 2;
                    LG_TRY(LAGraph_Realloc((void **) &call_i, (size_t) new_cap,
                                           (size_t) call_cap, sizeof(GrB_Index), msg));
                    LG_TRY(LAGraph_Realloc((void **) &call_j, (size_t) new_cap,
                                           (size_t) call_cap, sizeof(GrB_Index), msg));
                    LG_TRY(LAGraph_Realloc((void **) &call_x, (size_t) new_cap,
                                           (size_t) call_cap, sizeof(bool), msg));
                    call_cap = new_cap;
                }

                for (GrB_Index k = 0; k < entry_nvals; ++k)
                {
                    GrB_Index v = extract_idx[k];
                    call_i[call_count] = start_states[i];
                    call_j[call_count] = v * V + v;
                    call_x[call_count] = true;
                    call_count++;
                }

                // skip over nonterminal i using the summary edges
                GrB_Index gnt_nvals = 0;
                GRB_TRY(GrB_Matrix_nvals(&gnt_nvals, graph_nt[i]));
                if (gnt_nvals > 0)
                {
                    GRB_TRY(GrB_mxm(wb->p, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL, wb->x,
                                    graph_nt[i], GrB_NULL));
                    LG_TRY(s_scatter_local(wb->p, wb->d, wb->dest, V, &gbuild, &atup,
                                           &acc, &acc_empty, msg));
                }

                // RETURN bookkeeping: W_A |= X_A, destination-local
                GRB_TRY(GrB_eWiseAdd(wb->w, GrB_NULL, GrB_NULL, GrB_LOR, wb->w, wb->x,
                                     GrB_NULL));
            }
            LG_TRY(s_unshift_compact(wb->x, wb->d, V));
        }

        // M_call holds the union of every box's start-state / vertex tuples
        if (call_count > 0)
        {
            GRB_TRY(GrB_Matrix_build_BOOL(M_call, call_i, call_j, call_x, call_count,
                                          GrB_LOR));
        }

        // Phase 4: return transitions
        for (size_t i = 0; i < num_nonterm; ++i)
        {
            if (final_states[i] == GrB_NULL)
                continue;

            // 1 x Q * Q x VV -> 1 x VV
            GRB_TRY(GrB_mxm(new_edges, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL,
                            (GrB_Matrix)final_states[i], M, GrB_DESC_T0));

            GrB_Index ne_vals = 0;
            GRB_TRY(GrB_Matrix_nvals(&ne_vals, new_edges));
            if (ne_vals == 0)
                continue;

            GRB_TRY(GxB_Matrix_reshape(new_edges, false, V, V, GrB_NULL));

            // dG = new_edges \ graph_nt[i]
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

            // acc |= W_A * dG, then scatter local destination rows
            S_WaitBox *wb = &waitboxes[i];
            if (wb->w != GrB_NULL)
            {
                GrB_Index w_nvals = 0;
                GRB_TRY(GrB_Matrix_nvals(&w_nvals, wb->w));
                if (w_nvals > 0)
                {
                    GRB_TRY(GrB_mxm(wb->p, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL, wb->w,
                                    new_edges, GrB_NULL));
                    LG_TRY(s_scatter_local(wb->p, wb->d, wb->dest, V, &gbuild, &atup,
                                           &acc, &acc_empty, msg));
                }
            }

            GRB_TRY(GxB_Matrix_reshape(new_edges, false, 1, V * V, GrB_NULL));
        }

        // Phase 5: M_new = (reshape(acc) | M_call) \ P
        GRB_TRY(GrB_free(&M_new));
        GRB_TRY(GxB_Matrix_reshapeDup(&M_new, acc, false, Q, V * V, GrB_NULL));
        GRB_TRY(GrB_Matrix_clear(acc));
        acc_empty = true;
        GRB_TRY(GrB_eWiseAdd(M_new, P, GrB_NULL, GrB_LOR, M_new, M_call, GrB_DESC_RSC));
        GRB_TRY(GrB_eWiseAdd(P, GrB_NULL, GrB_NULL, GrB_LOR, P, M_new, GrB_NULL));

        GrB_Matrix swap = M;
        M = M_new;
        M_new = swap;

        GRB_TRY(GrB_Matrix_nvals(&m_nvals, M));
    }

    // reachable(w) = OR_s source_mask(s) & graph_nt[start](s, w)
    GRB_TRY(GrB_Vector_new(reachable, GrB_BOOL, V));
    GRB_TRY(GrB_mxv(*reachable, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL,
                    graph_nt[start_nonterm], source_mask, GrB_DESC_T0));

    LG_FREE_WORK;
    return GrB_SUCCESS;
}
