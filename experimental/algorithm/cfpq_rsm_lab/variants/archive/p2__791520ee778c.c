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
        s_compact_free(&compact);                                                                 \
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

typedef struct
{
    GrB_Index orig_Q;         // original state count
    GrB_Index state_count;    // compact Q'
    GrB_Index orig_term;      // original terminal count
    GrB_Index term_count;     // compact terminal count
    GrB_Index nonterm_count;  // unchanged
    GrB_Index *new_of_old;    // [orig_Q], S_NO_STATE when removed
    GrB_Index *old_of_new;    // [state_count]
    GrB_Index *term_of_orig;  // [orig_term], S_NO_STATE when dropped
    GrB_Index *orig_of_term;  // [term_count]
    GrB_Matrix *term;         // [term_count] owned Q' x Q'
    GrB_Matrix *graph;        // [term_count] borrowed caller graph matrices
    GrB_Matrix *nonterm;      // [nonterm_count] owned Q' x Q' or NULL
    GrB_Index *start_states;  // [nonterm_count]
    GrB_Vector *finals;       // [nonterm_count] owned length Q' or NULL
} S_Compact;

static void s_compact_free(S_Compact *cp)
{
    if (cp->term != NULL)
    {
        for (GrB_Index j = 0; j < cp->term_count; ++j)
            GrB_free(&cp->term[j]);
        LAGraph_Free((void **)&cp->term, NULL);
    }
    LAGraph_Free((void **)&cp->graph, NULL); // matrices themselves are borrowed
    if (cp->nonterm != NULL)
    {
        for (GrB_Index i = 0; i < cp->nonterm_count; ++i)
            GrB_free(&cp->nonterm[i]);
        LAGraph_Free((void **)&cp->nonterm, NULL);
    }
    if (cp->finals != NULL)
    {
        for (GrB_Index i = 0; i < cp->nonterm_count; ++i)
            GrB_free(&cp->finals[i]);
        LAGraph_Free((void **)&cp->finals, NULL);
    }
    LAGraph_Free((void **)&cp->new_of_old, NULL);
    LAGraph_Free((void **)&cp->old_of_new, NULL);
    LAGraph_Free((void **)&cp->term_of_orig, NULL);
    LAGraph_Free((void **)&cp->orig_of_term, NULL);
    LAGraph_Free((void **)&cp->start_states, NULL);
    memset(cp, 0, sizeof(*cp));
}

static GrB_Info s_compact_build(S_Compact *cp, GrB_Index Q, GrB_Index V, GrB_Index orig_term,
                                GrB_Index orig_nonterm, GrB_Matrix *rsm_term,
                                const GrB_Matrix *graph_term, GrB_Matrix *rsm_nonterm,
                                const GrB_Index *start_states, GrB_Vector *final_states,
                                GrB_Index start_nonterm, bool *start_removed, char *msg)
{
    (void)V;
    GrB_Info info = GrB_SUCCESS;
    bool *term_ok = NULL;
    GrB_Index *efrom = NULL;
    GrB_Index *eto = NULL;
    GrB_Index *foff = NULL;
    GrB_Index *roff = NULL;
    GrB_Index *fadj = NULL;
    GrB_Index *radj = NULL;
    GrB_Index *cur = NULL;
    bool *fseen = NULL;
    bool *rseen = NULL;
    bool *keep = NULL;
    GrB_Index *queue = NULL;
    GrB_Index *new_of_old = NULL;
    GrB_Index *old_of_new = NULL;
    S_TupleBuf ttup;
    S_TupleBuf ftup;
    memset(&ttup, 0, sizeof(ttup));
    memset(&ftup, 0, sizeof(ftup));

    *start_removed = false;

    S_CALLOC(term_ok, orig_term, sizeof(bool));

    // count control edges: active terminal transitions plus every nonterminal
    // transition (conservative call treatment)
    GrB_Index E = 0;
    for (GrB_Index t = 0; t < orig_term; ++t)
    {
        if (rsm_term[t] == GrB_NULL || graph_term[t] == GrB_NULL)
            continue;
        GrB_Index rn = 0, gn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_term[t]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        info = GrB_Matrix_nvals(&gn, graph_term[t]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        if (rn == 0 || gn == 0)
            continue;
        term_ok[t] = true;
        if (rn > CFPQ_MAX_DIM - E)
        {
            info = GrB_OUT_OF_MEMORY;
            goto cleanup;
        }
        E += rn;
    }
    for (GrB_Index i = 0; i < orig_nonterm; ++i)
    {
        if (rsm_nonterm[i] == GrB_NULL)
            continue;
        GrB_Index rn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_nonterm[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        if (rn > CFPQ_MAX_DIM - E)
        {
            info = GrB_OUT_OF_MEMORY;
            goto cleanup;
        }
        E += rn;
    }

    S_ALLOC(efrom, E, sizeof(GrB_Index));
    S_ALLOC(eto, E, sizeof(GrB_Index));

    GrB_Index e = 0;
    for (GrB_Index t = 0; t < orig_term; ++t)
    {
        if (!term_ok[t])
            continue;
        GrB_Index rn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_term[t]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        info = s_tuples_reserve(&ttup, rn, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index n = rn;
        info = GrB_Matrix_extractTuples_BOOL(ttup.i, ttup.j, ttup.x, &n, rsm_term[t]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        for (GrB_Index k = 0; k < n; ++k)
        {
            efrom[e] = ttup.i[k];
            eto[e] = ttup.j[k];
            e++;
        }
    }
    for (GrB_Index i = 0; i < orig_nonterm; ++i)
    {
        if (rsm_nonterm[i] == GrB_NULL)
            continue;
        GrB_Index rn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_nonterm[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        if (rn == 0)
            continue;
        info = s_tuples_reserve(&ttup, rn, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index n = rn;
        info = GrB_Matrix_extractTuples_BOOL(ttup.i, ttup.j, ttup.x, &n, rsm_nonterm[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        for (GrB_Index k = 0; k < n; ++k)
        {
            efrom[e] = ttup.i[k];
            eto[e] = ttup.j[k];
            e++;
        }
    }

    // forward / reverse CSR of the control graph
    S_CALLOC(foff, (size_t)Q + 1, sizeof(GrB_Index));
    S_CALLOC(roff, (size_t)Q + 1, sizeof(GrB_Index));
    for (GrB_Index k = 0; k < e; ++k)
    {
        foff[efrom[k] + 1]++;
        roff[eto[k] + 1]++;
    }
    for (GrB_Index q = 0; q < Q; ++q)
    {
        foff[q + 1] += foff[q];
        roff[q + 1] += roff[q];
    }
    S_ALLOC(fadj, e, sizeof(GrB_Index));
    S_ALLOC(radj, e, sizeof(GrB_Index));
    S_ALLOC(cur, (size_t)Q + 1, sizeof(GrB_Index));
    memcpy(cur, foff, ((size_t)Q + 1) * sizeof(GrB_Index));
    for (GrB_Index k = 0; k < e; ++k)
        fadj[cur[efrom[k]]++] = eto[k];
    memcpy(cur, roff, ((size_t)Q + 1) * sizeof(GrB_Index));
    for (GrB_Index k = 0; k < e; ++k)
        radj[cur[eto[k]]++] = efrom[k];

    S_CALLOC(fseen, Q, sizeof(bool));
    S_CALLOC(rseen, Q, sizeof(bool));
    S_CALLOC(keep, Q, sizeof(bool));
    S_ALLOC(queue, Q, sizeof(GrB_Index));

    // forward seeds: the start box and every conservatively callable box start
    GrB_Index head = 0, tail = 0;
    {
        GrB_Index ss = start_states[start_nonterm];
        if (ss < Q && !fseen[ss])
        {
            fseen[ss] = true;
            queue[tail++] = ss;
        }
    }
    for (GrB_Index i = 0; i < orig_nonterm; ++i)
    {
        if (rsm_nonterm[i] == GrB_NULL)
            continue;
        GrB_Index ss = start_states[i];
        if (ss < Q && !fseen[ss])
        {
            fseen[ss] = true;
            queue[tail++] = ss;
        }
    }
    while (head < tail)
    {
        GrB_Index q = queue[head++];
        for (GrB_Index p = foff[q]; p < foff[q + 1]; ++p)
        {
            GrB_Index v = fadj[p];
            if (!fseen[v])
            {
                fseen[v] = true;
                queue[tail++] = v;
            }
        }
    }

    // reverse seeds: every stored entry of every final vector
    head = 0;
    tail = 0;
    for (GrB_Index i = 0; i < orig_nonterm; ++i)
    {
        if (final_states[i] == GrB_NULL)
            continue;
        GrB_Index fn = 0;
        info = GrB_Vector_nvals(&fn, final_states[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        if (fn == 0)
            continue;
        info = s_tuples_reserve(&ftup, fn, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index n = fn;
        info = GrB_Vector_extractTuples_BOOL(ftup.i, ftup.x, &n, final_states[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        for (GrB_Index k = 0; k < n; ++k)
        {
            GrB_Index v = ftup.i[k];
            if (v < Q && !rseen[v])
            {
                rseen[v] = true;
                queue[tail++] = v;
            }
        }
    }
    while (head < tail)
    {
        GrB_Index q = queue[head++];
        for (GrB_Index p = roff[q]; p < roff[q + 1]; ++p)
        {
            GrB_Index v = radj[p];
            if (!rseen[v])
            {
                rseen[v] = true;
                queue[tail++] = v;
            }
        }
    }

    GrB_Index kept = 0;
    for (GrB_Index q = 0; q < Q; ++q)
    {
        keep[q] = fseen[q] && rseen[q];
        if (keep[q])
            kept++;
    }
    if (kept == 0 || start_states[start_nonterm] >= Q ||
        !keep[start_states[start_nonterm]])
    {
        // no accepting path starts at the query's start box: the exact result
        // is the empty length-V vector
        *start_removed = true;
        goto cleanup;
    }

    S_ALLOC(new_of_old, Q, sizeof(GrB_Index));
    S_ALLOC(old_of_new, kept, sizeof(GrB_Index));
    for (GrB_Index q = 0; q < Q; ++q)
        new_of_old[q] = S_NO_STATE;
    GrB_Index nx = 0;
    for (GrB_Index q = 0; q < Q; ++q)
    {
        if (!keep[q])
            continue;
        new_of_old[q] = nx;
        old_of_new[nx] = q;
        nx++;
    }

    cp->orig_Q = Q;
    cp->state_count = kept;
    cp->orig_term = orig_term;
    cp->nonterm_count = orig_nonterm;
    cp->new_of_old = new_of_old;
    cp->old_of_new = old_of_new;
    new_of_old = NULL;
    old_of_new = NULL;

    S_CALLOC(cp->term_of_orig, orig_term, sizeof(GrB_Index));
    for (GrB_Index t = 0; t < orig_term; ++t)
        cp->term_of_orig[t] = S_NO_STATE;

    // assign deterministic internal terminal ids (increasing original id)
    GrB_Index nt = 0;
    for (GrB_Index t = 0; t < orig_term; ++t)
    {
        if (!term_ok[t])
            continue;
        GrB_Index rn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_term[t]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        info = s_tuples_reserve(&ttup, rn, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index n = rn;
        info = GrB_Matrix_extractTuples_BOOL(ttup.i, ttup.j, ttup.x, &n, rsm_term[t]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index c = 0;
        for (GrB_Index k = 0; k < n; ++k)
            if (cp->new_of_old[ttup.i[k]] != S_NO_STATE &&
                cp->new_of_old[ttup.j[k]] != S_NO_STATE)
                c++;
        if (c > 0)
            cp->term_of_orig[t] = nt++;
    }
    cp->term_count = nt;

    S_CALLOC(cp->term, nt, sizeof(GrB_Matrix));
    S_CALLOC(cp->graph, nt, sizeof(GrB_Matrix));
    S_ALLOC(cp->orig_of_term, nt, sizeof(GrB_Index));

    for (GrB_Index t = 0; t < orig_term; ++t)
    {
        GrB_Index j = cp->term_of_orig[t];
        if (j == S_NO_STATE)
            continue;
        GrB_Index rn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_term[t]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        info = s_tuples_reserve(&ttup, rn, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index n = rn;
        info = GrB_Matrix_extractTuples_BOOL(ttup.i, ttup.j, ttup.x, &n, rsm_term[t]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index c = 0;
        for (GrB_Index k = 0; k < n; ++k)
        {
            GrB_Index a = cp->new_of_old[ttup.i[k]];
            GrB_Index b = cp->new_of_old[ttup.j[k]];
            if (a == S_NO_STATE || b == S_NO_STATE)
                continue;
            ttup.i[c] = a;
            ttup.j[c] = b;
            ttup.x[c] = true;
            c++;
        }
        info = GrB_Matrix_new(&cp->term[j], GrB_BOOL, kept, kept);
        if (info != GrB_SUCCESS)
            goto cleanup;
        info = GrB_Matrix_build_BOOL(cp->term[j], ttup.i, ttup.j, ttup.x, c, GrB_LOR);
        if (info != GrB_SUCCESS)
            goto cleanup;
        cp->orig_of_term[j] = t;
        cp->graph[j] = graph_term[t];
    }

    S_CALLOC(cp->nonterm, orig_nonterm, sizeof(GrB_Matrix));
    S_ALLOC(cp->start_states, orig_nonterm, sizeof(GrB_Index));
    S_CALLOC(cp->finals, orig_nonterm, sizeof(GrB_Vector));

    for (GrB_Index i = 0; i < orig_nonterm; ++i)
    {
        GrB_Index ss = start_states[i];
        cp->start_states[i] = (ss < Q) ? cp->new_of_old[ss] : S_NO_STATE;

        if (rsm_nonterm[i] == GrB_NULL)
            continue;
        if (cp->start_states[i] == S_NO_STATE)
        {
            // empty-language box: its call sites cannot contribute and its
            // start state is not representable in the compact domain
            continue;
        }
        GrB_Index rn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_nonterm[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        if (rn == 0)
            continue;
        info = s_tuples_reserve(&ttup, rn, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index n = rn;
        info = GrB_Matrix_extractTuples_BOOL(ttup.i, ttup.j, ttup.x, &n, rsm_nonterm[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index c = 0;
        for (GrB_Index k = 0; k < n; ++k)
        {
            GrB_Index a = cp->new_of_old[ttup.i[k]];
            GrB_Index b = cp->new_of_old[ttup.j[k]];
            if (a == S_NO_STATE || b == S_NO_STATE)
                continue;
            ttup.i[c] = a;
            ttup.j[c] = b;
            ttup.x[c] = true;
            c++;
        }
        if (c == 0)
            continue; // no surviving call sites: equivalent to no calls
        info = GrB_Matrix_new(&cp->nonterm[i], GrB_BOOL, kept, kept);
        if (info != GrB_SUCCESS)
            goto cleanup;
        info = GrB_Matrix_build_BOOL(cp->nonterm[i], ttup.i, ttup.j, ttup.x, c, GrB_LOR);
        if (info != GrB_SUCCESS)
            goto cleanup;
    }

    for (GrB_Index i = 0; i < orig_nonterm; ++i)
    {
        if (final_states[i] == GrB_NULL)
            continue;
        GrB_Index fn = 0;
        info = GrB_Vector_nvals(&fn, final_states[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        if (fn == 0)
            continue;
        info = s_tuples_reserve(&ftup, fn, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index n = fn;
        info = GrB_Vector_extractTuples_BOOL(ftup.i, ftup.x, &n, final_states[i]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        GrB_Index c = 0;
        for (GrB_Index k = 0; k < n; ++k)
            if (cp->new_of_old[ftup.i[k]] != S_NO_STATE)
                c++;
        if (c == 0)
            continue;
        info = GrB_Vector_new(&cp->finals[i], GrB_BOOL, kept);
        if (info != GrB_SUCCESS)
            goto cleanup;
        for (GrB_Index k = 0; k < n; ++k)
        {
            GrB_Index a = cp->new_of_old[ftup.i[k]];
            if (a == S_NO_STATE)
                continue;
            info = GrB_Vector_setElement_BOOL(cp->finals[i], true, a);
            if (info != GrB_SUCCESS)
                goto cleanup;
        }
    }

cleanup:
    S_FREE(term_ok);
    S_FREE(efrom);
    S_FREE(eto);
    S_FREE(foff);
    S_FREE(roff);
    S_FREE(fadj);
    S_FREE(radj);
    S_FREE(cur);
    S_FREE(fseen);
    S_FREE(rseen);
    S_FREE(keep);
    S_FREE(queue);
    S_FREE(new_of_old);
    S_FREE(old_of_new);
    s_tuples_free(&ttup);
    s_tuples_free(&ftup);
    return info;
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

    S_Compact compact;
    memset(&compact, 0, sizeof(compact));

    // reusable call-tuple buffers for the direct flat-layout M_call build
    GrB_Index *call_i = NULL;
    GrB_Index *call_j = NULL;
    bool *call_x = NULL;
    GrB_Index call_count = 0;
    GrB_Index call_cap = 0;
    GrB_Index *extract_idx = NULL;
    GrB_Index extract_cap = 0;

    GrB_Matrix *graph_nt = NULL;
    GrB_Matrix *W = NULL;         // QV x V, W[i] = (rsm_nonterm[i])^T * P
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
    // conservative trimming and internal compaction (patch 2)
    //--------------------------------------------------------------------------

    bool start_removed = false;
    LG_TRY(s_compact_build(&compact, Q, V, num_term, num_nonterm, rsm_term, graph_term,
                           rsm_nonterm, start_states, final_states, start_nonterm,
                           &start_removed, msg));

    if (start_removed)
    {
        // Every original input check above has already run.  No state lies on
        // an accepting start-to-final control path, so the exact result is the
        // empty length-V vector.
        GRB_TRY(GrB_Vector_new(reachable, GrB_BOOL, V));
        LG_FREE_WORK;
        return GrB_SUCCESS;
    }

    // Switch downstream dimensions and arrays to the compact representation.
    // V, nonterminal ids, and start_nonterminal are unchanged.
    Q = compact.state_count;
    num_term = compact.term_count;
    rsm_term = compact.term;
    graph_term = compact.graph;
    rsm_nonterm = compact.nonterm;
    start_states = compact.start_states;
    final_states = compact.finals;

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

        // Phases 2+3: nonterminal transitions and calls
        GRB_TRY(GrB_Matrix_clear(M_call));
        call_count = 0;
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
                // at (v, v), in its start state.  Collect the tuples and build
                // M_call once after the loop.
                GRB_TRY(GrB_reduce(entry_vec, GrB_NULL, GrB_NULL, GrB_LOR_MONOID_BOOL, scratch,
                                   GrB_DESC_T0));
                GrB_Index entry_nvals = 0;
                GRB_TRY(GrB_Vector_nvals(&entry_nvals, entry_vec));
                // A nonempty scratch has at least one active column.
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
                    LG_TRY(s_mul_acc(&acc, &s_temp, scratch, graph_nt[i], &acc_empty));
                
                // RETURN bookkeeping: W[i] == reshape(R_i' * P)
                GRB_TRY(GrB_eWiseAdd(W[i], GrB_NULL, GrB_NULL, GrB_LOR, W[i], scratch, GrB_NULL));
            }
            LG_TRY(s_unshift(scratch, Q, V));
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

            // acc |= W * dG, where W = R_A^T * P
            if (W[i] != GrB_NULL)
            {
                GrB_Index w_nvals = 0;
                GRB_TRY(GrB_Matrix_nvals(&w_nvals, W[i]));
                if (w_nvals > 0)
                    LG_TRY(s_mul_acc(&acc, &s_temp, W[i], new_edges, &acc_empty));
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
