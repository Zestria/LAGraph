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

#define FREE_ROUTE_ARRAY(array, len)                                                               \
    {                                                                                              \
        if ((array) != NULL)                                                                       \
        {                                                                                          \
            for (size_t _k = 0; _k < (len); ++_k)                                                  \
                s_route_free(&(array)[_k]);                                                        \
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
        LAGraph_Free((void **)&term_active, NULL);                                                 \
        FREE_ROUTE_ARRAY(route_term, num_term);                                                    \
        FREE_ROUTE_ARRAY(route_nt, num_nonterm);                                                   \
        LAGraph_Free((void **)&final_q_count, NULL);                                               \
        FREE_MATRIX_ARRAY(final_q, num_nonterm);                                                   \
        LAGraph_Free((void **)&fr_rows, NULL);                                                     \
        LAGraph_Free((void **)&fr_cols, NULL);                                                     \
        LAGraph_Free((void **)&fr_u, NULL);                                                        \
        LAGraph_Free((void **)&fr_v, NULL);                                                        \
        LAGraph_Free((void **)&bucket_start, NULL);                                                \
        LAGraph_Free((void **)&bucket_pos, NULL);                                                  \
        LAGraph_Free((void **)&bucket_order, NULL);                                                \
        LAGraph_Free((void **)&route_i, NULL);                                                     \
        LAGraph_Free((void **)&route_j, NULL);                                                     \
        LAGraph_Free((void **)&route_x, NULL);                                                     \
        LAGraph_Free((void **)&call_i, NULL);                                                      \
        LAGraph_Free((void **)&call_j, NULL);                                                      \
        LAGraph_Free((void **)&call_x, NULL);                                                      \
        LAGraph_Free((void **)&dg_i, NULL);                                                        \
        LAGraph_Free((void **)&dg_j, NULL);                                                        \
        LAGraph_Free((void **)&dg_x, NULL);                                                        \
        LAGraph_Free((void **)&extract_idx, NULL);                                                 \
        GrB_free(&new_edges);                                                                      \
        GrB_free(&entry_vec);                                                                      \
        GrB_free(&source_mask);                                                                    \
        FREE_GRB_MATRIX_ARRAY(graph_nt, num_nonterm);                                              \
        FREE_GRB_MATRIX_ARRAY(W, num_nonterm);                                                     \
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

// C |= A * B, using one reused QV x V product temporary.
static GrB_Info s_mul_acc(GrB_Matrix C, GrB_Matrix A, GrB_Matrix B, GrB_Matrix T)
{
    H_TRY(GrB_Matrix_clear(T));
    H_TRY(GrB_mxm(T, GrB_NULL, GrB_NULL, GrB_LOR_LAND_SEMIRING_BOOL, A, B, GrB_NULL));
    H_TRY(GrB_eWiseAdd(C, GrB_NULL, GrB_NULL, GrB_LOR, C, T, GrB_NULL));
    return GrB_SUCCESS;
}

// Per-label RSM routing edges q -> r.  Repeated destination states are kept.
typedef struct
{
    GrB_Index *src;
    GrB_Index *dst;
    GrB_Index count;
} s_route;

static int s_route_load(s_route *route, GrB_Matrix A, char *msg)
{
    memset(route, 0, sizeof(*route));
    GrB_Index n = 0;
    int rc = GrB_Matrix_nvals(&n, A);
    if (rc != GrB_SUCCESS)
        return rc;
    if (n == 0)
        return GrB_SUCCESS;
    rc = LAGraph_Malloc((void **) &route->src, (size_t) n, sizeof(GrB_Index), msg);
    if (rc != GrB_SUCCESS)
        return rc;
    rc = LAGraph_Malloc((void **) &route->dst, (size_t) n, sizeof(GrB_Index), msg);
    if (rc != GrB_SUCCESS)
    {
        LAGraph_Free((void **) &route->src, msg);
        return rc;
    }
    GrB_Index n2 = n;
    rc = GrB_Matrix_extractTuples_BOOL(route->src, route->dst, NULL, &n2, A);
    if (rc != GrB_SUCCESS)
        return rc;
    route->count = n2;
    return GrB_SUCCESS;
}

static void s_route_free(s_route *route)
{
    LAGraph_Free((void **) &route->src, NULL);
    LAGraph_Free((void **) &route->dst, NULL);
    route->count = 0;
}

// Grow the reusable (I,J,X) tuple buffers to hold at least need entries.
static GrB_Info s_tuple_reserve(GrB_Index **I, GrB_Index **J, bool **X, GrB_Index *cap,
                                GrB_Index need, char *msg)
{
    if (need <= *cap)
        return GrB_SUCCESS;
    GrB_Index newcap = (*cap == 0) ? (GrB_Index) 64 : *cap;
    while (newcap < need)
        newcap *= 2;
    int rc = LAGraph_Realloc((void **) I, (size_t) newcap, (size_t) *cap,
                             sizeof(GrB_Index), msg);
    if (rc != GrB_SUCCESS)
        return (GrB_Info) rc;
    rc = LAGraph_Realloc((void **) J, (size_t) newcap, (size_t) *cap,
                         sizeof(GrB_Index), msg);
    if (rc != GrB_SUCCESS)
        return (GrB_Info) rc;
    rc = LAGraph_Realloc((void **) X, (size_t) newcap, (size_t) *cap, sizeof(bool), msg);
    if (rc != GrB_SUCCESS)
        return (GrB_Info) rc;
    *cap = newcap;
    return GrB_SUCCESS;
}

// scratch = routed RSM shift for one label, QV x V, built from the frontier
// buckets.  Every (q,r) edge contributes (r*V+u, v) for each (q*V+u, v)
// frontier tuple; the build uses LOR for duplicate configurations.
static GrB_Info s_route_build(GrB_Matrix scratch, const s_route *route,
                              const GrB_Index *bucket_start, const GrB_Index *bucket_order,
                              const GrB_Index *fr_u, const GrB_Index *fr_v,
                              GrB_Index **ri, GrB_Index **rj, bool **rx,
                              GrB_Index *rcount, GrB_Index *rcap, GrB_Index V, char *msg)
{
    *rcount = 0;
    H_TRY(GrB_Matrix_clear(scratch));
    for (GrB_Index e = 0; e < route->count; ++e)
    {
        GrB_Index q = route->src[e];
        GrB_Index r = route->dst[e];
        GrB_Index begin = bucket_start[q];
        GrB_Index end = bucket_start[q + 1];
        if (begin == end)
            continue;
        GrB_Index need = *rcount + (end - begin);
        H_TRY(s_tuple_reserve(ri, rj, rx, rcap, need, msg));
        for (GrB_Index t = begin; t < end; ++t)
        {
            GrB_Index k = bucket_order[t];
            (*ri)[*rcount] = r * V + fr_u[k];
            (*rj)[*rcount] = fr_v[k];
            (*rx)[*rcount] = true;
            (*rcount)++;
        }
    }
    if (*rcount > 0)
        H_TRY(GrB_Matrix_build_BOOL(scratch, *ri, *rj, *rx, *rcount, GrB_LOR));
    return GrB_SUCCESS;
}

// new_edges = V x V summary candidates for one box, built from the frontier
// buckets selected by that box's final states.
static GrB_Info s_summary_build(GrB_Matrix new_edges, const GrB_Index *final_q,
                                GrB_Index final_count, const GrB_Index *bucket_start,
                                const GrB_Index *bucket_order, const GrB_Index *fr_u,
                                const GrB_Index *fr_v, GrB_Index **di, GrB_Index **dj,
                                bool **dx, GrB_Index *dcount, GrB_Index *dcap, char *msg)
{
    *dcount = 0;
    H_TRY(GrB_Matrix_clear(new_edges));
    for (GrB_Index f = 0; f < final_count; ++f)
    {
        GrB_Index q = final_q[f];
        GrB_Index begin = bucket_start[q];
        GrB_Index end = bucket_start[q + 1];
        if (begin == end)
            continue;
        GrB_Index need = *dcount + (end - begin);
        H_TRY(s_tuple_reserve(di, dj, dx, dcap, need, msg));
        for (GrB_Index t = begin; t < end; ++t)
        {
            GrB_Index k = bucket_order[t];
            (*di)[*dcount] = fr_u[k];
            (*dj)[*dcount] = fr_v[k];
            (*dx)[*dcount] = true;
            (*dcount)++;
        }
    }
    if (*dcount > 0)
        H_TRY(GrB_Matrix_build_BOOL(new_edges, *di, *dj, *dx, *dcount, GrB_LOR));
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

int LAGraph_CFPQ_RSM(GrB_Vector *reachable, const RSM *rsm, const GrB_Matrix *graph_term,
                     const GrB_Index *sources, size_t num_sources, GrB_Index V, char *msg)
{
    LG_CLEAR_MSG;

    // working matrices / vectors (all configuration matrices are block QV x V)
    GrB_Matrix M = GrB_NULL;
    GrB_Matrix P = GrB_NULL;
    GrB_Matrix acc = GrB_NULL;
    GrB_Matrix M_call = GrB_NULL;
    GrB_Matrix M_new = GrB_NULL;
    GrB_Matrix scratch = GrB_NULL;
    GrB_Matrix s_temp = GrB_NULL; // QV x V product scratch for s_mul_acc
    GrB_Matrix new_edges = GrB_NULL;
    GrB_Vector entry_vec = GrB_NULL;
    GrB_Vector source_mask = GrB_NULL;

    GrB_Matrix *graph_nt = NULL;
    GrB_Matrix *W = NULL;         // QV x V, W[i] = (rsm_nonterm[i])^T * P
    GrB_Index num_term = 0;
    GrB_Index num_nonterm = 0;

    bool *term_active = NULL;

    // per-label RSM adjacency lists and per-box final-state q lists
    s_route *route_term = NULL;
    s_route *route_nt = NULL;
    GrB_Index **final_q = NULL;
    GrB_Index *final_q_count = NULL;

    // reusable frontier extraction, bucketing and tuple buffers
    GrB_Index *fr_rows = NULL;
    GrB_Index *fr_cols = NULL;
    GrB_Index *fr_u = NULL;
    GrB_Index *fr_v = NULL;
    GrB_Index *bucket_start = NULL;
    GrB_Index *bucket_pos = NULL;
    GrB_Index *bucket_order = NULL;
    GrB_Index front_cap = 0;

    GrB_Index *route_i = NULL;
    GrB_Index *route_j = NULL;
    bool *route_x = NULL;
    GrB_Index route_count = 0;
    GrB_Index route_cap = 0;

    GrB_Index *call_i = NULL;
    GrB_Index *call_j = NULL;
    bool *call_x = NULL;
    GrB_Index call_count = 0;
    GrB_Index call_cap = 0;

    GrB_Index *dg_i = NULL;
    GrB_Index *dg_j = NULL;
    bool *dg_x = NULL;
    GrB_Index dg_count = 0;
    GrB_Index dg_cap = 0;

    GrB_Index *extract_idx = NULL;
    GrB_Index extract_cap = 0;

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
    LG_TRY(LAGraph_Calloc((void **)&W, num_nonterm, sizeof(GrB_Matrix), msg));

    for (size_t i = 0; i < num_nonterm; ++i)
    {
        GRB_TRY(GrB_Matrix_new(&graph_nt[i], GrB_BOOL, V, V));
        if (rsm_nonterm[i] == GrB_NULL)
            continue;

        GRB_TRY(GrB_Matrix_new(&W[i], GrB_BOOL, Q * V, V));
    }

    // per-label routing lists and per-box final-state lists
    if (num_term > 0)
        LG_TRY(LAGraph_Calloc((void **)&route_term, num_term, sizeof(s_route), msg));
    LG_TRY(LAGraph_Calloc((void **)&route_nt, num_nonterm, sizeof(s_route), msg));
    LG_TRY(LAGraph_Calloc((void **)&final_q, num_nonterm, sizeof(GrB_Index *), msg));
    LG_TRY(LAGraph_Calloc((void **)&final_q_count, num_nonterm, sizeof(GrB_Index), msg));

    for (size_t i = 0; i < num_nonterm; ++i)
    {
        if (rsm_nonterm[i] != GrB_NULL)
            LG_TRY(s_route_load(&route_nt[i], rsm_nonterm[i], msg));

        if (final_states[i] == GrB_NULL)
            continue;
        GrB_Index fn = 0;
        GRB_TRY(GrB_Vector_nvals(&fn, final_states[i]));
        if (fn == 0)
            continue;
        LG_TRY(LAGraph_Malloc((void **)&final_q[i], (size_t) fn, sizeof(GrB_Index), msg));
        GrB_Index fn2 = fn;
        GRB_TRY(GrB_Vector_extractTuples_BOOL(final_q[i], NULL, &fn2, final_states[i]));
        final_q_count[i] = fn2;
    }

    // allocate working matrices; everything except the graph summaries and
    // new_edges is block QV x V
    GRB_TRY(GrB_Matrix_new(&M, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&acc, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&M_call, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&M_new, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&scratch, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&s_temp, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&new_edges, GrB_BOOL, V, V));
    GRB_TRY(GrB_Vector_new(&entry_vec, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(&source_mask, GrB_BOOL, V));

    if (Q > 0)
    {
        LG_TRY(LAGraph_Calloc((void **)&bucket_start, (size_t) Q + 1,
                              sizeof(GrB_Index), msg));
        LG_TRY(LAGraph_Calloc((void **)&bucket_pos, (size_t) Q, sizeof(GrB_Index), msg));
    }

    if (num_term > 0)
    {
        LG_TRY(LAGraph_Calloc((void **) &term_active, num_term, sizeof(bool), msg));

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
            LG_TRY(s_route_load(&route_term[i], rsm_term[i], msg));
        }
    }

    // each source s starts a call at block row start_state*V+s, column s
    GrB_Index start_state = start_states[start_nonterm];
    for (size_t i = 0; i < num_sources; ++i)
    {
        GrB_Index s = sources[i];
        GRB_TRY(GrB_Matrix_setElement_BOOL(M, true, start_state * V + s, s));
        GRB_TRY(GrB_Vector_setElement_BOOL(source_mask, true, s));
    }
    GRB_TRY(GrB_Matrix_dup(&P, M));

    GrB_Index m_nvals = 0;
    GRB_TRY(GrB_Matrix_nvals(&m_nvals, M));
    while (m_nvals > 0)
    {
        // Extract the block frontier once, decode (q*V+u, v) and bucket the
        // tuples by state q for all routing in this iteration.
        if (m_nvals > front_cap)
        {
            LG_TRY(LAGraph_Realloc((void **) &fr_rows, (size_t) m_nvals,
                                   (size_t) front_cap, sizeof(GrB_Index), msg));
            LG_TRY(LAGraph_Realloc((void **) &fr_cols, (size_t) m_nvals,
                                   (size_t) front_cap, sizeof(GrB_Index), msg));
            LG_TRY(LAGraph_Realloc((void **) &fr_u, (size_t) m_nvals,
                                   (size_t) front_cap, sizeof(GrB_Index), msg));
            LG_TRY(LAGraph_Realloc((void **) &fr_v, (size_t) m_nvals,
                                   (size_t) front_cap, sizeof(GrB_Index), msg));
            LG_TRY(LAGraph_Realloc((void **) &bucket_order, (size_t) m_nvals,
                                   (size_t) front_cap, sizeof(GrB_Index), msg));
            front_cap = m_nvals;
        }
        GrB_Index front_n = m_nvals;
        GRB_TRY(GrB_Matrix_extractTuples_BOOL(fr_rows, fr_cols, NULL, &front_n, M));

        memset(bucket_start, 0, (size_t) (Q + 1) * sizeof(GrB_Index));
        for (GrB_Index k = 0; k < front_n; ++k)
        {
            GrB_Index q = fr_rows[k] / V;
            fr_u[k] = fr_rows[k] % V;
            fr_v[k] = fr_cols[k];
            bucket_start[q + 1]++;
        }
        for (GrB_Index q = 0; q < Q; ++q)
            bucket_start[q + 1] += bucket_start[q];
        memcpy(bucket_pos, bucket_start, (size_t) Q * sizeof(GrB_Index));
        for (GrB_Index k = 0; k < front_n; ++k)
        {
            GrB_Index q = fr_rows[k] / V;
            bucket_order[bucket_pos[q]++] = k;
        }

        // Phase 1: terminal transitions, routed through the RSM edges
        for (size_t i = 0; i < num_term; ++i)
        {
            if (!term_active[i])
                continue;

            LG_TRY(s_route_build(scratch, &route_term[i], bucket_start, bucket_order,
                                 fr_u, fr_v, &route_i, &route_j, &route_x,
                                 &route_count, &route_cap, V, msg));
            GrB_Index tn = 0;
            GRB_TRY(GrB_Matrix_nvals(&tn, scratch));
            if (tn > 0)
                LG_TRY(s_mul_acc(acc, scratch, graph_term[i], s_temp));
        }

        // Phases 2+3: nonterminal transitions and calls
        GRB_TRY(GrB_Matrix_clear(M_call));
        call_count = 0;
        for (size_t i = 0; i < num_nonterm; ++i)
        {
            if (rsm_nonterm[i] == GrB_NULL)
                continue;

            LG_TRY(s_route_build(scratch, &route_nt[i], bucket_start, bucket_order,
                                 fr_u, fr_v, &route_i, &route_j, &route_x,
                                 &route_count, &route_cap, V, msg));

            GrB_Index x_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&x_nvals, scratch));
            if (x_nvals > 0)
            {
                // call: every entry vertex v enters nonterminal i at block
                // row start_states[i]*V+v, column v
                GRB_TRY(GrB_reduce(entry_vec, GrB_NULL, GrB_NULL, GrB_LOR_MONOID_BOOL, scratch,
                                   GrB_DESC_T0));
                GrB_Index entry_nvals = 0;
                GRB_TRY(GrB_Vector_nvals(&entry_nvals, entry_vec));
                if (entry_nvals > 0)
                {
                    if (entry_nvals > extract_cap)
                    {
                        LG_TRY(LAGraph_Realloc((void **) &extract_idx, (size_t) entry_nvals,
                                               (size_t) extract_cap, sizeof(GrB_Index), msg));
                        extract_cap = entry_nvals;
                    }
                    GrB_Index entry_nvals2 = entry_nvals;
                    GRB_TRY(GrB_Vector_extractTuples_BOOL(extract_idx, NULL, &entry_nvals2,
                                                          entry_vec));

                    if (call_count + entry_nvals2 > call_cap)
                    {
                        GrB_Index new_cap = (call_count + entry_nvals2) * 2;
                        LG_TRY(LAGraph_Realloc((void **) &call_i, (size_t) new_cap,
                                               (size_t) call_cap, sizeof(GrB_Index), msg));
                        LG_TRY(LAGraph_Realloc((void **) &call_j, (size_t) new_cap,
                                               (size_t) call_cap, sizeof(GrB_Index), msg));
                        LG_TRY(LAGraph_Realloc((void **) &call_x, (size_t) new_cap,
                                               (size_t) call_cap, sizeof(bool), msg));
                        call_cap = new_cap;
                    }

                    for (GrB_Index k = 0; k < entry_nvals2; ++k)
                    {
                        GrB_Index v = extract_idx[k];
                        call_i[call_count] = start_states[i] * V + v;
                        call_j[call_count] = v;
                        call_x[call_count] = true;
                        call_count++;
                    }
                }

                // skip over nonterminal i using the summary edges
                GrB_Index gnt_nvals = 0;
                GRB_TRY(GrB_Matrix_nvals(&gnt_nvals, graph_nt[i]));
                if (gnt_nvals > 0)
                    LG_TRY(s_mul_acc(acc, scratch, graph_nt[i], s_temp));

                // RETURN bookkeeping: W[i] == reshape(R_i' * P)
                GRB_TRY(GrB_eWiseAdd(W[i], GrB_NULL, GrB_NULL, GrB_LOR, W[i], scratch, GrB_NULL));
            }
        }

        if (call_count > 0)
        {
            GRB_TRY(GrB_Matrix_build_BOOL(M_call, call_i, call_j, call_x, call_count,
                                          GrB_LOR));
        }

        // Phase 4: return transitions, built from the same frontier buckets
        for (size_t i = 0; i < num_nonterm; ++i)
        {
            if (final_states[i] == GrB_NULL)
                continue;

            LG_TRY(s_summary_build(new_edges, final_q[i], final_q_count[i],
                                   bucket_start, bucket_order, fr_u, fr_v,
                                   &dg_i, &dg_j, &dg_x, &dg_count, &dg_cap, msg));
            GrB_Index ne_vals = 0;
            GRB_TRY(GrB_Matrix_nvals(&ne_vals, new_edges));
            if (ne_vals == 0)
                continue;

            // dG = new_edges \ graph_nt[i]
            GrB_Index gnt_nvals = 0;
            GRB_TRY(GrB_Matrix_nvals(&gnt_nvals, graph_nt[i]));
            if (gnt_nvals > 0)
            {
                GRB_TRY(GrB_apply(new_edges, graph_nt[i], GrB_NULL, GrB_IDENTITY_BOOL, new_edges,
                                  GrB_DESC_RSC));
                GRB_TRY(GrB_Matrix_nvals(&ne_vals, new_edges));
                if (ne_vals == 0)
                    continue;
            }

            GRB_TRY(GrB_eWiseAdd(graph_nt[i], GrB_NULL, GrB_NULL, GrB_LOR, graph_nt[i], new_edges,
                                 GrB_NULL));

            // acc |= W * dG, where W = R_A^T * P
            if (W[i] != GrB_NULL)
            {
                GrB_Index w_nvals = 0;
                GRB_TRY(GrB_Matrix_nvals(&w_nvals, W[i]));
                if (w_nvals > 0)
                    LG_TRY(s_mul_acc(acc, W[i], new_edges, s_temp));
            }
        }

        // Phase 5: overwrite the block frontier, subtract P, union into P
        GRB_TRY(GrB_eWiseAdd(M_new, P, GrB_NULL, GrB_LOR, acc, M_call, GrB_DESC_RSC));
        GRB_TRY(GrB_eWiseAdd(P, GrB_NULL, GrB_NULL, GrB_LOR, P, M_new, GrB_NULL));
        GRB_TRY(GrB_Matrix_clear(acc));

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
