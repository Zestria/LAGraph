// P1 full coordinates, derived from P1 reviewed (SHA-256 1c5ad06ef855...).
// Coordinate compression and its encoding/decoding tables are removed.
// Forward columns are destination_state*V+vertex, preserving endpoint sharing;
// dual columns are member*V+vertex. A membership-only hash retains P1's
// support-filtered, one-scan dual gather. All transfers remain batched over
// actual nonzeros. Nonterminal/call/return/fixed-point phases are unchanged.

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
        GrB_free(&fscratch);                                                                       \
        GrB_free(&dprod);                                                                          \
        GrB_free(&gbuild);                                                                         \
        LAGraph_Free((void **)&term_active, NULL);                                                 \
        s_groupset_free(&groupset, num_term);                                                      \
        s_tuples_free(&gtup);                                                                      \
        s_tuples_free(&atup);                                                                      \
        s_tuples_free(&mtup);                                                                      \
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

//------------------------------------------------------------------------------
// grouped terminal transfers
//------------------------------------------------------------------------------
//
// A terminal family is a set of indexed terminal transitions that share one
// endpoint: either one source state (`q --t_i--> p_i`, a forward family) or one
// destination state (`q_i --t_i--> r`, a dual family).  The family is evaluated
// with one product on full state/member-by-vertex coordinates. The same batched
// gather/scatter and graph-support filtering as P1 reviewed are retained;
// no matrix axis is renumbered to observed endpoints.  Families are discovered from transition topology and
// graph support only, so no terminal names, label numbering, or box ownership
// metadata is assumed.  Every transition belongs to at most one family; the
// remaining transitions use the ordinary per-label path.

#define S_HASH_EMPTY UINT64_MAX

typedef struct
{
    uint64_t *key_a;  // global member id, or S_HASH_EMPTY when the slot is free
    uint64_t *key_b;  // observed vertex
    size_t cap;       // power of two
    size_t count;
} S_SupportSet;

// reusable tuple arrays (both GrB_Index and Boolean value arrays)
typedef struct
{
    GrB_Index *i;
    GrB_Index *j;
    bool *x;
    GrB_Index cap;
} S_TupleBuf;

typedef struct
{
    bool dual;             // false: shared source; true: shared destination
    GrB_Index anchor;      // shared source or destination state
    GrB_Index member_base; // first global member id of this family
    size_t ntrans;
    GrB_Index *label;      // [ntrans] terminal label of each transition
    GrB_Index *from;       // [ntrans]
    GrB_Index *to;         // [ntrans]
    GrB_Index *member_tr;  // [ntrans] index into the flat transition list
    GrB_Index ncols;       // full axis: Q*V forward; ntrans*V dual
    GrB_Matrix inc;        // forward H: V x ncols; dual J: ncols x V
    GrB_Matrix tmp;        // forward Y: V x ncols; dual F_in: V x ncols
    bool live;             // runtime flag: family has active source rows
    GrB_Index *fi_u;       // dual gather tuples: entry row u
    GrB_Index *fi_c;       // dual gather tuples: full column (member*V+vertex)
    bool *fi_x;            // dual gather values (always true)
    GrB_Index fi_n;
    GrB_Index fi_cap;
} S_Group;

typedef struct
{
    S_Group *groups;
    size_t ngroups;
    S_SupportSet support;      // membership of (global member id, graph source vertex)
    GrB_Index *dual_off; // [Q+1] CSR of dual-family members by source state
    GrB_Index *dual_group;
    GrB_Index *dual_local;
    GrB_Index ndual;
    GrB_Matrix *ord_term;      // [num_term] residual ordinary terminal matrices
    GrB_Index **ord_src;       // [num_term] active-source prediction lists
    size_t *ord_src_count;     // [num_term]
} S_GroupSet;

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

static uint64_t s_support_mix(uint64_t a, uint64_t b)
{
    uint64_t x = (a * 0x9E3779B97F4A7C15ULL) ^ (b + 0x9E3779B97F4A7C15ULL);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

static void s_support_free(S_SupportSet *h)
{
    LAGraph_Free((void **)&h->key_a, NULL);
    LAGraph_Free((void **)&h->key_b, NULL);
    h->cap = 0;
    h->count = 0;
}

static GrB_Info s_support_alloc(S_SupportSet *h, size_t cap, char *msg)
{
    if (cap < 16)
        cap = 16;
    if (cap > SIZE_MAX / sizeof(uint64_t))
        return GrB_OUT_OF_MEMORY;

    uint64_t *ka = NULL;
    uint64_t *kb = NULL;
    int st;
    st = LAGraph_Malloc((void **)&ka, cap, sizeof(uint64_t), msg);
    if (st < 0)
        return (GrB_Info)st;
    st = LAGraph_Malloc((void **)&kb, cap, sizeof(uint64_t), msg);
    if (st < 0)
    {
        LAGraph_Free((void **)&ka, NULL);
        return (GrB_Info)st;
    }
    memset(ka, 0xFF, cap * sizeof(uint64_t)); // S_HASH_EMPTY
    h->key_a = ka;
    h->key_b = kb;
    h->cap = cap;
    h->count = 0;
    return GrB_SUCCESS;
}

static GrB_Info s_support_grow(S_SupportSet *h, char *msg)
{
    if (h->cap > (SIZE_MAX / 2))
        return GrB_OUT_OF_MEMORY;

    S_SupportSet bigger;
    memset(&bigger, 0, sizeof(bigger));
    GrB_Info info = s_support_alloc(&bigger, h->cap * 2, msg);
    if (info != GrB_SUCCESS)
        return info;

    for (size_t k = 0; k < h->cap; ++k)
    {
        if (h->key_a[k] == S_HASH_EMPTY)
            continue;
        uint64_t mask = (uint64_t)(bigger.cap - 1);
        uint64_t p = s_support_mix(h->key_a[k], h->key_b[k]) & mask;
        while (bigger.key_a[p] != S_HASH_EMPTY)
            p = (p + 1) & mask;
        bigger.key_a[p] = h->key_a[k];
        bigger.key_b[p] = h->key_b[k];
        bigger.count++;
    }

    s_support_free(h);
    *h = bigger;
    return GrB_SUCCESS;
}

// Retain graph support membership without assigning or storing coordinates.
static GrB_Info s_support_add(S_SupportSet *h, uint64_t a, uint64_t b, char *msg)
{
    if (h->cap == 0)
        H_TRY(s_support_alloc(h, 64, msg));
    if (h->count + 1 >= h->cap - h->cap / 4)
        H_TRY(s_support_grow(h, msg));

    uint64_t mask = (uint64_t)(h->cap - 1);
    uint64_t p = s_support_mix(a, b) & mask;
    while (h->key_a[p] != S_HASH_EMPTY)
    {
        if (h->key_a[p] == a && h->key_b[p] == b)
            return GrB_SUCCESS;
        p = (p + 1) & mask;
    }
    h->key_a[p] = a;
    h->key_b[p] = b;
    h->count++;
    return GrB_SUCCESS;
}

static bool s_support_contains(const S_SupportSet *h, uint64_t a, uint64_t b)
{
    if (h->cap == 0)
        return false;
    uint64_t mask = (uint64_t)(h->cap - 1);
    uint64_t p = s_support_mix(a, b) & mask;
    while (h->key_a[p] != S_HASH_EMPTY)
    {
        if (h->key_a[p] == a && h->key_b[p] == b)
        {
            return true;
        }
        p = (p + 1) & mask;
    }
    return false;
}

static GrB_Info s_group_fi_append(S_Group *g, GrB_Index u, GrB_Index c, char *msg)
{
    if (g->fi_n == g->fi_cap)
    {
        GrB_Index cap = (g->fi_cap == 0) ? 64 : g->fi_cap;
        while (cap <= g->fi_n)
        {
            if (cap > (GrB_Index)(SIZE_MAX / (8 * sizeof(GrB_Index))))
            {
                cap = g->fi_n + 1;
                break;
            }
            cap *= 2;
        }
        int st;
        st = LAGraph_Realloc((void **)&g->fi_u, (size_t)cap, (size_t)g->fi_cap,
                             sizeof(GrB_Index), msg);
        if (st < 0)
            return (GrB_Info)st;
        st = LAGraph_Realloc((void **)&g->fi_c, (size_t)cap, (size_t)g->fi_cap,
                             sizeof(GrB_Index), msg);
        if (st < 0)
            return (GrB_Info)st;
        st = LAGraph_Realloc((void **)&g->fi_x, (size_t)cap, (size_t)g->fi_cap, sizeof(bool),
                             msg);
        if (st < 0)
            return (GrB_Info)st;
        g->fi_cap = cap;
    }
    g->fi_u[g->fi_n] = u;
    g->fi_c[g->fi_n] = c;
    g->fi_x[g->fi_n] = true;
    g->fi_n++;
    return GrB_SUCCESS;
}

static void s_group_free(S_Group *g)
{
    LAGraph_Free((void **)&g->label, NULL);
    LAGraph_Free((void **)&g->from, NULL);
    LAGraph_Free((void **)&g->to, NULL);
    LAGraph_Free((void **)&g->member_tr, NULL);
    LAGraph_Free((void **)&g->fi_u, NULL);
    LAGraph_Free((void **)&g->fi_c, NULL);
    LAGraph_Free((void **)&g->fi_x, NULL);
    GrB_free(&g->inc);
    GrB_free(&g->tmp);
    memset(g, 0, sizeof(*g));
}

static void s_groupset_free(S_GroupSet *gs, GrB_Index num_term)
{
    if (gs->groups != NULL)
    {
        for (size_t k = 0; k < gs->ngroups; ++k)
            s_group_free(&gs->groups[k]);
        LAGraph_Free((void **)&gs->groups, NULL);
    }
    gs->ngroups = 0;
    s_support_free(&gs->support);
    LAGraph_Free((void **)&gs->dual_off, NULL);
    LAGraph_Free((void **)&gs->dual_group, NULL);
    LAGraph_Free((void **)&gs->dual_local, NULL);
    gs->ndual = 0;

    if (gs->ord_term != NULL)
    {
        for (GrB_Index t = 0; t < num_term; ++t)
            GrB_free(&gs->ord_term[t]);
        LAGraph_Free((void **)&gs->ord_term, NULL);
    }
    if (gs->ord_src != NULL)
    {
        for (GrB_Index t = 0; t < num_term; ++t)
            LAGraph_Free((void **)&gs->ord_src[t], NULL);
        LAGraph_Free((void **)&gs->ord_src, NULL);
    }
    LAGraph_Free((void **)&gs->ord_src_count, NULL);
}

typedef struct
{
    bool dual;
    GrB_Index anchor;
    size_t count;
} S_Cand;

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

static int s_cand_cmp(const void *pa, const void *pb)
{
    const S_Cand *a = (const S_Cand *)pa;
    const S_Cand *b = (const S_Cand *)pb;
    if (a->count != b->count)
        return (a->count > b->count) ? -1 : 1;
    if (a->dual != b->dual)
        return a->dual ? 1 : -1;
    if (a->anchor != b->anchor)
        return (a->anchor < b->anchor) ? -1 : 1;
    return 0;
}

// Build a fixed incidence matrix on full, directly decodable coordinates.
// Forward: H[v, destination_state*V+w] = OR of matching graph edges v->w.
// Dual:    J[member*V+v, w] = G_label(member)[v,w].
// The forward coordinate deliberately uses the destination state, retaining
// P1's endpoint sharing when several labels lead to the same continuation.
// The dual support set records membership only; it never assigns column IDs.
static GrB_Info s_group_build_full(S_Group *g, GrB_Index V,
                                   const GrB_Matrix *graph_term,
                                   S_SupportSet *support, S_TupleBuf *htup,
                                   S_TupleBuf *gtup, char *msg)
{
    GrB_Index h_n = 0;
    for (size_t m = 0; m < g->ntrans; ++m)
    {
        GrB_Index gn = 0;
        H_TRY(GrB_Matrix_nvals(&gn, graph_term[g->label[m]]));
        if (gn > SIZE_MAX / sizeof(GrB_Index) - h_n)
            return GrB_OUT_OF_MEMORY;
        H_TRY(s_tuples_reserve(gtup, gn, msg));
        H_TRY(s_tuples_reserve(htup, h_n + gn, msg));
        GrB_Index n = gn;
        H_TRY(GrB_Matrix_extractTuples_BOOL(gtup->i, gtup->j, gtup->x, &n,
                                            graph_term[g->label[m]]));

        for (GrB_Index k = 0; k < n; ++k)
        {
            GrB_Index v = gtup->i[k];
            GrB_Index w = gtup->j[k];
            if (g->dual)
            {
                // Keep support filtering independent of coordinate encoding.
                H_TRY(s_support_add(support, g->member_base + (GrB_Index)m, v, msg));
                htup->i[h_n] = (GrB_Index)m * V + v;
                htup->j[h_n] = w;
            }
            else
            {
                htup->i[h_n] = v;
                htup->j[h_n] = g->to[m] * V + w;
            }
            htup->x[h_n++] = true;
        }
    }

    GrB_Index nrows = g->dual ? g->ncols : V;
    GrB_Index ncols = g->dual ? V : g->ncols;
    H_TRY(GrB_Matrix_new(&g->inc, GrB_BOOL, nrows, ncols));
    // OR duplicate coordinates, exactly as in P1's compact construction.
    H_TRY(GrB_Matrix_build_BOOL(g->inc, htup->i, htup->j, htup->x, h_n, GrB_LOR));
    H_TRY(GrB_Matrix_new(&g->tmp, GrB_BOOL, V, g->ncols));
    return GrB_SUCCESS;
}

// Discover families, build their full incidence matrices and support membership, and build the residual
// ordinary terminal matrices.  On failure, partially built state stays in `gs`
// for the caller's cleanup; helper-local scratch is always released here.
static GrB_Info s_build_groups(S_GroupSet *gs, GrB_Index Q, GrB_Index V, GrB_Index num_term,
                               GrB_Matrix *rsm_term, const GrB_Matrix *graph_term, bool *term_active,
                               S_TupleBuf *gtup, char *msg)
{
    GrB_Info info = GrB_SUCCESS;
    GrB_Index *tr_label = NULL;
    GrB_Index *tr_from = NULL;
    GrB_Index *tr_to = NULL;
    GrB_Index *tr_off = NULL;
    GrB_Index *src_off = NULL;
    GrB_Index *src_list = NULL;
    GrB_Index *dst_off = NULL;
    GrB_Index *dst_list = NULL;
    GrB_Index *cursor = NULL;
    S_Cand *cands = NULL;
    bool *assigned = NULL;
    S_TupleBuf ttup;
    S_TupleBuf htup;
    memset(&ttup, 0, sizeof(ttup));
    memset(&htup, 0, sizeof(htup));

    // flat list of supported terminal transitions, grouped by label
    GrB_Index total = 0;
    for (GrB_Index t = 0; t < num_term; ++t)
    {
        if (!term_active[t])
            continue;
        GrB_Index rn = 0;
        info = GrB_Matrix_nvals(&rn, rsm_term[t]);
        if (info != GrB_SUCCESS)
            goto cleanup;
        if (rn > (GrB_Index)(SIZE_MAX / sizeof(GrB_Index)) - total)
        {
            info = GrB_OUT_OF_MEMORY;
            goto cleanup;
        }
        total += rn;
    }

    if (total == 0)
        goto cleanup; // no supported transitions: nothing to group or run

    if (total > (GrB_Index)(SIZE_MAX / sizeof(GrB_Index)) ||
        num_term > (GrB_Index)(SIZE_MAX / sizeof(GrB_Index) - 1))
    {
        info = GrB_OUT_OF_MEMORY;
        goto cleanup;
    }

    S_ALLOC(tr_label, total, sizeof(GrB_Index));
    S_ALLOC(tr_from, total, sizeof(GrB_Index));
    S_ALLOC(tr_to, total, sizeof(GrB_Index));
    S_ALLOC(tr_off, (size_t)num_term + 1, sizeof(GrB_Index));
    S_CALLOC(src_off, (size_t)Q + 1, sizeof(GrB_Index));
    S_CALLOC(dst_off, (size_t)Q + 1, sizeof(GrB_Index));
    S_ALLOC(src_list, total, sizeof(GrB_Index));
    S_ALLOC(dst_list, total, sizeof(GrB_Index));
    S_ALLOC(cursor, (size_t)Q + 1, sizeof(GrB_Index));
    S_CALLOC(assigned, total, sizeof(bool));

    GrB_Index at = 0;
    for (GrB_Index t = 0; t < num_term; ++t)
    {
        tr_off[t] = at;
        if (!term_active[t])
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
            tr_label[at] = t;
            tr_from[at] = ttup.i[k];
            tr_to[at] = ttup.j[k];
            at++;
        }
    }
    tr_off[num_term] = at;

    // CSR lists by source and by destination
    for (GrB_Index k = 0; k < at; ++k)
    {
        src_off[tr_from[k] + 1]++;
        dst_off[tr_to[k] + 1]++;
    }
    for (GrB_Index q = 0; q < Q; ++q)
    {
        src_off[q + 1] += src_off[q];
        dst_off[q + 1] += dst_off[q];
    }
    memcpy(cursor, src_off, ((size_t)Q + 1) * sizeof(GrB_Index));
    for (GrB_Index k = 0; k < at; ++k)
        src_list[cursor[tr_from[k]]++] = k;
    memcpy(cursor, dst_off, ((size_t)Q + 1) * sizeof(GrB_Index));
    for (GrB_Index k = 0; k < at; ++k)
        dst_list[cursor[tr_to[k]]++] = k;

    size_t ncand = 0;
    for (GrB_Index q = 0; q < Q; ++q)
    {
        if (src_off[q + 1] - src_off[q] >= 2)
            ncand++;
        if (dst_off[q + 1] - dst_off[q] >= 2)
            ncand++;
    }
    if (ncand == 0)
        goto build_ordinary;

    S_ALLOC(cands, ncand, sizeof(S_Cand));

    size_t max_groups = (size_t)(at / 2) + 1;
    S_CALLOC(gs->groups, max_groups, sizeof(S_Group));
    gs->ngroups = 0;
    size_t ci = 0;
    for (GrB_Index q = 0; q < Q; ++q)
    {
        if (src_off[q + 1] - src_off[q] >= 2)
        {
            cands[ci].dual = false;
            cands[ci].anchor = q;
            cands[ci].count = (size_t)(src_off[q + 1] - src_off[q]);
            ci++;
        }
        if (dst_off[q + 1] - dst_off[q] >= 2)
        {
            cands[ci].dual = true;
            cands[ci].anchor = q;
            cands[ci].count = (size_t)(dst_off[q + 1] - dst_off[q]);
            ci++;
        }
    }
    qsort(cands, ncand, sizeof(S_Cand), s_cand_cmp);

    for (size_t c = 0; c < ncand; ++c)
    {
        GrB_Index lo = cands[c].dual ? dst_off[cands[c].anchor] : src_off[cands[c].anchor];
        GrB_Index hi = cands[c].dual ? dst_off[cands[c].anchor + 1] : src_off[cands[c].anchor + 1];
        const GrB_Index *list = cands[c].dual ? dst_list : src_list;
        size_t keep = 0;
        for (GrB_Index p = lo; p < hi; ++p)
            if (!assigned[list[p]])
                keep++;
        if (keep < 2)
            continue;

        S_Group *g = &gs->groups[gs->ngroups];
        memset(g, 0, sizeof(*g));
        g->dual = cands[c].dual;
        g->anchor = cands[c].anchor;
        g->ntrans = keep;
        // count this slot before sub-allocations so failure cleanup covers it
        gs->ngroups++;
        S_ALLOC(g->label, keep, sizeof(GrB_Index));
        S_ALLOC(g->from, keep, sizeof(GrB_Index));
        S_ALLOC(g->to, keep, sizeof(GrB_Index));
        S_ALLOC(g->member_tr, keep, sizeof(GrB_Index));
        size_t m = 0;
        for (GrB_Index p = lo; p < hi; ++p)
        {
            GrB_Index k = list[p];
            if (assigned[k])
                continue;
            assigned[k] = true;
            g->label[m] = tr_label[k];
            g->from[m] = tr_from[k];
            g->to[m] = tr_to[k];
            g->member_tr[m] = k;
            m++;
        }
    }

    // P1's families and support filtering, with no compact coordinate IDs.
    GrB_Index next_base = 0;
    for (size_t gi = 0; gi < gs->ngroups; ++gi)
    {
        S_Group *g = &gs->groups[gi];
        g->member_base = next_base;
        next_base += (GrB_Index)g->ntrans;

        // Forward uses destination-state blocks so identical continuations
        // still share coordinates. Dual uses one full vertex block per member.
        GrB_Index blocks = g->dual ? (GrB_Index)g->ntrans : Q;
        if (s_mul_exceeds(blocks, V, CFPQ_MAX_DIM) ||
            s_mul_exceeds(V, blocks * V, CFPQ_MAX_DIM))
        {
            for (size_t m = 0; m < g->ntrans; ++m)
                assigned[g->member_tr[m]] = false;
            s_group_free(g);
            continue;
        }
        g->ncols = blocks * V;
        info = s_group_build_full(g, V, graph_term, &gs->support, &htup, gtup, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
    }

    // dual-family members grouped by source state, for one shared M scan
    {
        GrB_Index nd = 0;
        for (size_t gi = 0; gi < gs->ngroups; ++gi)
            if (gs->groups[gi].dual)
                nd += (GrB_Index)gs->groups[gi].ntrans;
        gs->ndual = nd;
        if (nd > 0)
        {
            if (nd > (GrB_Index)(SIZE_MAX / sizeof(GrB_Index)))
            {
                info = GrB_OUT_OF_MEMORY;
                goto cleanup;
            }
            S_CALLOC(gs->dual_off, (size_t)Q + 1, sizeof(GrB_Index));
            S_ALLOC(gs->dual_group, nd, sizeof(GrB_Index));
            S_ALLOC(gs->dual_local, nd, sizeof(GrB_Index));
            for (size_t gi = 0; gi < gs->ngroups; ++gi)
            {
                S_Group *g = &gs->groups[gi];
                if (!g->dual)
                    continue;
                for (size_t m = 0; m < g->ntrans; ++m)
                    gs->dual_off[g->from[m] + 1]++;
            }
            for (GrB_Index q = 0; q < Q; ++q)
                gs->dual_off[q + 1] += gs->dual_off[q];
            memcpy(cursor, gs->dual_off, ((size_t)Q + 1) * sizeof(GrB_Index));
            for (size_t gi = 0; gi < gs->ngroups; ++gi)
            {
                S_Group *g = &gs->groups[gi];
                if (!g->dual)
                    continue;
                for (size_t m = 0; m < g->ntrans; ++m)
                {
                    GrB_Index pos = cursor[g->from[m]]++;
                    gs->dual_group[pos] = (GrB_Index)gi;
                    gs->dual_local[pos] = (GrB_Index)m;
                }
            }
        }
    }

build_ordinary:
    // residual ordinary terminal matrices: every transition not in a family
    S_CALLOC(gs->ord_term, (size_t)num_term, sizeof(GrB_Matrix));
    S_CALLOC(gs->ord_src, (size_t)num_term, sizeof(GrB_Index *));
    S_CALLOC(gs->ord_src_count, (size_t)num_term, sizeof(size_t));

    for (GrB_Index t = 0; t < num_term; ++t)
    {
        if (!term_active[t])
            continue;
        GrB_Index cnt = 0;
        for (GrB_Index k = tr_off[t]; k < tr_off[t + 1]; ++k)
            if (!assigned[k])
                cnt++;
        if (cnt == 0)
            continue;

        S_ALLOC(gs->ord_src[t], cnt, sizeof(GrB_Index));
        info = s_tuples_reserve(&ttup, cnt, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
        gs->ord_src_count[t] = (size_t)cnt;

        GrB_Index c = 0;
        for (GrB_Index k = tr_off[t]; k < tr_off[t + 1]; ++k)
        {
            if (assigned[k])
                continue;
            ttup.i[c] = tr_from[k];
            ttup.j[c] = tr_to[k];
            ttup.x[c] = true;
            gs->ord_src[t][c] = tr_from[k];
            c++;
        }

        info = GrB_Matrix_new(&gs->ord_term[t], GrB_BOOL, Q, Q);
        if (info != GrB_SUCCESS)
            goto cleanup;
        info = GrB_Matrix_build_BOOL(gs->ord_term[t], ttup.i, ttup.j, ttup.x, c, GrB_LOR);
        if (info != GrB_SUCCESS)
            goto cleanup;
    }

cleanup:
    S_FREE(tr_label);
    S_FREE(tr_from);
    S_FREE(tr_to);
    S_FREE(tr_off);
    S_FREE(src_off);
    S_FREE(dst_off);
    S_FREE(src_list);
    S_FREE(dst_list);
    S_FREE(cursor);
    S_FREE(cands);
    S_FREE(assigned);
    s_tuples_free(&ttup);
    s_tuples_free(&htup);
    return info;
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

// Scatter one forward family: Y = F * H, with c = destination_state*V+w.
static GrB_Info s_group_forward(S_Group *g, GrB_Index V, GrB_Matrix M, GrB_Matrix fscratch,
                                GrB_Matrix *gbuild, S_TupleBuf *tup, GrB_Matrix *acc,
                                bool *acc_empty, char *msg)
{
    GrB_Index row = g->anchor;
    H_TRY(GxB_Matrix_reshape(fscratch, false, 1, V * V, GrB_NULL));
    H_TRY(GrB_Matrix_extract(fscratch, GrB_NULL, GrB_NULL, M, &row, 1, GrB_ALL, V * V,
                             GrB_NULL));
    H_TRY(GxB_Matrix_reshape(fscratch, false, V, V, GrB_NULL));
    H_TRY(GrB_mxm(g->tmp, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL, fscratch, g->inc, GrB_NULL));

    GrB_Index n = 0;
    H_TRY(GrB_Matrix_nvals(&n, g->tmp));
    if (n == 0)
        return GrB_SUCCESS;
    H_TRY(s_tuples_reserve(tup, n, msg));
    H_TRY(GrB_Matrix_extractTuples_BOOL(tup->i, tup->j, tup->x, &n, g->tmp));

    for (GrB_Index k = 0; k < n; ++k)
    {
        GrB_Index c = tup->j[k];
        GrB_Index destination = c / V;
        GrB_Index vertex = c % V;
        tup->i[k] = destination * V + tup->i[k];
        tup->j[k] = vertex;
        tup->x[k] = true;
    }

    H_TRY(GrB_Matrix_clear(*gbuild));
    H_TRY(GrB_Matrix_build_BOOL(*gbuild, tup->i, tup->j, tup->x, n, GrB_LOR));
    return s_acc_merge(acc, gbuild, acc_empty);
}

// Scatter one dual family: Y = F_in * J, where every row returns to `anchor`.
static GrB_Info s_group_dual(S_Group *g, GrB_Index V, GrB_Matrix dprod, GrB_Matrix *gbuild,
                             S_TupleBuf *tup, GrB_Matrix *acc, bool *acc_empty, char *msg)
{
    if (g->fi_n == 0)
        return GrB_SUCCESS;

    H_TRY(GrB_Matrix_clear(g->tmp));
    H_TRY(GrB_Matrix_build_BOOL(g->tmp, g->fi_u, g->fi_c, g->fi_x, g->fi_n, GrB_LOR));
    H_TRY(GrB_mxm(dprod, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL, g->tmp, g->inc, GrB_NULL));

    GrB_Index n = 0;
    H_TRY(GrB_Matrix_nvals(&n, dprod));
    if (n == 0)
        return GrB_SUCCESS;
    H_TRY(s_tuples_reserve(tup, n, msg));
    H_TRY(GrB_Matrix_extractTuples_BOOL(tup->i, tup->j, tup->x, &n, dprod));

    for (GrB_Index k = 0; k < n; ++k)
    {
        tup->i[k] = g->anchor * V + tup->i[k];
        tup->x[k] = true;
    }

    H_TRY(GrB_Matrix_clear(*gbuild));
    H_TRY(GrB_Matrix_build_BOOL(*gbuild, tup->i, tup->j, tup->x, n, GrB_LOR));
    return s_acc_merge(acc, gbuild, acc_empty);
}

// Apply all grouped terminal contributions for one sweep.
static GrB_Info s_apply_groups(S_GroupSet *gs, GrB_Index V, GrB_Matrix M, const bool *active,
                               GrB_Matrix fscratch, GrB_Matrix dprod, GrB_Matrix *gbuild,
                               S_TupleBuf *tup, S_TupleBuf *mtup, GrB_Matrix *acc,
                               bool *acc_empty, char *msg)
{
    bool any_dual = false;
    for (size_t gi = 0; gi < gs->ngroups; ++gi)
    {
        S_Group *g = &gs->groups[gi];
        if (g->ntrans == 0)
            continue;
        g->live = false;
        if (!g->dual)
        {
            g->live = active[g->anchor];
        }
        else
        {
            for (size_t m = 0; m < g->ntrans; ++m)
                if (active[g->from[m]])
                {
                    g->live = true;
                    break;
                }
        }
        if (g->dual && g->live)
            any_dual = true;
    }

    for (size_t gi = 0; gi < gs->ngroups; ++gi)
    {
        S_Group *g = &gs->groups[gi];
        if (g->ntrans == 0 || g->dual || !g->live)
            continue;
        H_TRY(s_group_forward(g, V, M, fscratch, gbuild, tup, acc, acc_empty, msg));
    }

    if (!any_dual)
        return GrB_SUCCESS;

    GrB_Index mn = 0;
    H_TRY(GrB_Matrix_nvals(&mn, M));
    if (mn == 0)
        return GrB_SUCCESS;
    H_TRY(s_tuples_reserve(mtup, mn, msg));
    H_TRY(GrB_Matrix_extractTuples_BOOL(mtup->i, mtup->j, mtup->x, &mn, M));

    for (size_t gi = 0; gi < gs->ngroups; ++gi)
        if (gs->groups[gi].dual)
            gs->groups[gi].fi_n = 0;

    for (GrB_Index k = 0; k < mn; ++k)
    {
        GrB_Index q = mtup->i[k];
        GrB_Index lo = gs->dual_off[q];
        GrB_Index hi = gs->dual_off[q + 1];
        if (lo == hi)
            continue;
        GrB_Index flat = mtup->j[k];
        GrB_Index u = flat / V;
        GrB_Index v = flat % V;
        for (GrB_Index p = lo; p < hi; ++p)
        {
            S_Group *g = &gs->groups[gs->dual_group[p]];
            if (!g->live)
                continue;
            GrB_Index member = gs->dual_local[p];
            uint64_t gm = (uint64_t)(g->member_base + member);
            if (!s_support_contains(&gs->support, gm, (uint64_t)v))
                continue;
            // Full coordinate; membership filtering above does no renumbering.
            H_TRY(s_group_fi_append(g, u, member * V + v, msg));
        }
    }

    for (size_t gi = 0; gi < gs->ngroups; ++gi)
    {
        S_Group *g = &gs->groups[gi];
        if (g->ntrans == 0 || !g->dual || !g->live)
            continue;
        H_TRY(s_group_dual(g, V, dprod, gbuild, tup, acc, acc_empty, msg));
    }

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

    // grouped terminal-transfer state and scratch
    S_GroupSet groupset;
    memset(&groupset, 0, sizeof(groupset));
    S_TupleBuf gtup;
    S_TupleBuf atup;
    S_TupleBuf mtup;
    memset(&gtup, 0, sizeof(gtup));
    memset(&atup, 0, sizeof(atup));
    memset(&mtup, 0, sizeof(mtup));
    GrB_Matrix fscratch = GrB_NULL; // V x V row extraction / forward operand
    GrB_Matrix dprod = GrB_NULL;    // V x V dual-family product
    GrB_Matrix gbuild = GrB_NULL;   // QV x V grouped scatter target

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
    // terminal preprocessing, family discovery, and immutable group maps
    //--------------------------------------------------------------------------

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
        }

        LG_TRY(s_build_groups(&groupset, Q, V, num_term, rsm_term, graph_term, term_active,
                              &gtup, msg));
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

    // allocate working matrices
    GRB_TRY(GrB_Matrix_new(&M, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&acc, GrB_BOOL, Q*V, V));
    GRB_TRY(GrB_Matrix_new(&M_call, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&scratch, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&s_temp, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&new_edges, GrB_BOOL, 1, V * V));
    GRB_TRY(GrB_Vector_new(&entry_vec, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(&source_mask, GrB_BOOL, V));

    // grouped-terminal scratch, only when at least one family survived
    bool have_group = false;
    for (size_t gi = 0; gi < groupset.ngroups; ++gi)
        if (groupset.groups[gi].ntrans > 0)
        {
            have_group = true;
            break;
        }
    if (have_group)
    {
        GRB_TRY(GrB_Matrix_new(&fscratch, GrB_BOOL, V, V));
        GRB_TRY(GrB_Matrix_new(&dprod, GrB_BOOL, V, V));
        GRB_TRY(GrB_Matrix_new(&gbuild, GrB_BOOL, Q * V, V));
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

        // Phase 1: grouped terminal families first, then the residual ordinary
        // terminal transitions.  T = A_i^T * M is nonempty only if some source
        // state of A_i is an active row of M, so whole chains can be skipped.
        if (have_group)
        {
            LG_TRY(s_apply_groups(&groupset, V, M, active, fscratch, dprod, &gbuild, &atup, &mtup,
                                  &acc, &acc_empty, msg));
        }

        for (size_t i = 0; i < num_term; ++i)
        {
            if (groupset.ord_term == NULL || groupset.ord_term[i] == GrB_NULL)
                continue;

            bool predicted = false;
            for (size_t k = 0; k < groupset.ord_src_count[i]; ++k)
            {
                if (active[groupset.ord_src[i][k]])
                {
                    predicted = true;
                    break;
                }
            }
            if (!predicted)
                continue;

            LG_TRY(s_shift(scratch, groupset.ord_term[i], M, Q, V));
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
