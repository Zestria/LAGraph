// Literal block-matrix version of P1 reviewed (parent SHA-256 1c5ad06ef855...).
// Read s_group_build_blocks(), s_group_forward(), and s_group_dual() first.
// Only terminal-family storage/transfers differ: family discovery, ordinary
// fallback, nonterminal calls/returns, and the fixed-point loop retain P1 logic.
// Construction is inside this one-shot call, as in P1 reviewed. Logical block
// axes have K*V positions; matrix storage and parallelism are left to GraphBLAS.

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
        LAGraph_Free((void **)&term_active, NULL);                                                 \
        s_groupset_free(&groupset, num_term);                                                      \
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

// Literal terminal families; setup tuple buffers describe RSM transitions only.

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

// A family with K transitions uses the full K*V block axis. GraphBLAS chooses
// the sparse/bitmap/full storage; we do not enumerate graph edges, hash observed
// endpoints, merge columns, or remap individual frontier/product entries.
typedef struct
{
    bool dual;                // false: shared source; true: shared destination
    GrB_Index anchor;         // that source or destination RSM state
    size_t ntrans;            // K, the number of transitions in this family
    GrB_Index *label;          // [K] graph relation used by each block
    GrB_Index *from;           // [K] source RSM states
    GrB_Index *to;             // [K] destination RSM states
    GrB_Index *member_tr;      // [K] setup-only transition IDs for fallback
    GrB_Index ncols;           // K*V, with every vertex position retained
    GrB_Matrix inc;            // forward H: V x KV; dual J: KV x V
    GrB_Matrix tmp;            // forward Y: V x KV; dual Y: V x V
    GrB_Matrix D;              // dual only: [F_r0 | ... | F_r(K-1)], V x KV
    GrB_Matrix *frontier_tiles;// dual only: K reusable V x V frontier slices
} S_Group;

typedef struct
{
    S_Group *groups;
    size_t ngroups;
    GrB_Matrix *ord_term;      // residual transitions not assigned to a family
    GrB_Index **ord_src;
    size_t *ord_src_count;
} S_GroupSet;

static void s_group_free(S_Group *g)
{
    if (g->frontier_tiles != NULL)
    {
        for (size_t m = 0; m < g->ntrans; ++m)
            GrB_free(&g->frontier_tiles[m]);
        LAGraph_Free((void **)&g->frontier_tiles, NULL);
    }
    GrB_free(&g->D);
    GrB_free(&g->inc);
    GrB_free(&g->tmp);
    LAGraph_Free((void **)&g->label, NULL);
    LAGraph_Free((void **)&g->from, NULL);
    LAGraph_Free((void **)&g->to, NULL);
    LAGraph_Free((void **)&g->member_tr, NULL);
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
    gs->ngroups = 0;
}

// Construct exactly the block matrices in the formulas. The temporary tile
// array borrows graph matrices: concatenation copies entries, not ownership.
static GrB_Info s_group_build_blocks(S_Group *g, GrB_Index V,
                                     const GrB_Matrix *graph_term, char *msg)
{
    GrB_Info info = GrB_SUCCESS;
    GrB_Matrix *tiles = NULL;
#define B_TRY(expr) do { info = (expr); if (info != GrB_SUCCESS) goto cleanup; } while (0)
    B_TRY(LAGraph_Malloc((void **)&tiles, g->ntrans, sizeof(*tiles), msg));
    for (size_t m = 0; m < g->ntrans; ++m)
        tiles[m] = graph_term[g->label[m]];

    if (!g->dual)
    {
        // H = [G_0 | G_1 | ... | G_(K-1)], dimensions V x (K*V).
        B_TRY(GrB_Matrix_new(&g->inc, GrB_BOOL, V, g->ncols));
        B_TRY(GxB_Matrix_concat(g->inc, tiles, 1, g->ntrans, GrB_NULL));
        B_TRY(GrB_Matrix_new(&g->tmp, GrB_BOOL, V, g->ncols));
    }
    else
    {
        // J = [G_0; G_1; ...; G_(K-1)], dimensions (K*V) x V.
        B_TRY(GrB_Matrix_new(&g->inc, GrB_BOOL, g->ncols, V));
        B_TRY(GxB_Matrix_concat(g->inc, tiles, g->ntrans, 1, GrB_NULL));
        B_TRY(GrB_Matrix_new(&g->D, GrB_BOOL, V, g->ncols));
        B_TRY(GrB_Matrix_new(&g->tmp, GrB_BOOL, V, V));
        B_TRY(LAGraph_Calloc((void **)&g->frontier_tiles, g->ntrans,
                             sizeof(*g->frontier_tiles), msg));
        for (size_t m = 0; m < g->ntrans; ++m)
            B_TRY(GrB_Matrix_new(&g->frontier_tiles[m], GrB_BOOL, V, V));
    }

cleanup:
    LAGraph_Free((void **)&tiles, NULL);
#undef B_TRY
    return info;
}

// F_q[u,v] = M[q, u*V+v]. Extract the complete row and reshape it by row.
// With no mask/accumulator, extract replaces F, including empty positions.
static GrB_Info s_frontier_slice(GrB_Matrix F, GrB_Matrix M,
                                 GrB_Index q, GrB_Index V)
{
    H_TRY(GxB_Matrix_reshape(F, false, 1, V * V, GrB_NULL));
    H_TRY(GrB_Matrix_extract(F, GrB_NULL, GrB_NULL, M, &q, 1,
                             GrB_ALL, V * V, GrB_NULL));
    H_TRY(GxB_Matrix_reshape(F, false, V, V, GrB_NULL));
    return GrB_SUCCESS;
}

// acc uses row q*V+u and column v. Add one V x V result to state q.
// OR accumulation also handles several family members returning to one state.
static GrB_Info s_accumulate_block(GrB_Matrix acc, GrB_Matrix block,
                                   GrB_Index q, GrB_Index V, bool *acc_empty)
{
    GrB_Index nvals = 0;
    H_TRY(GrB_Matrix_nvals(&nvals, block));
    if (nvals == 0)
        return GrB_SUCCESS;

    GrB_Index rows[2] = {q * V, (q + 1) * V - 1};
    H_TRY(GrB_Matrix_assign(acc, GrB_NULL, GrB_LOR, block,
                            rows, GxB_RANGE, GrB_ALL, V, GrB_NULL));
    *acc_empty = false;
    return GrB_SUCCESS;
}

// First type: a shared source s and transitions s --t_m--> d_m.
// H is fixed. One multiplication computes Y = F_s H. Block m of Y
// contains F_s G_m, and is assigned to the corresponding destination d_m.
static GrB_Info s_group_forward(S_Group *g, GrB_Index V, GrB_Matrix M,
                                GrB_Matrix block, GrB_Matrix acc, bool *acc_empty)
{
    H_TRY(s_frontier_slice(block, M, g->anchor, V));
    H_TRY(GrB_mxm(g->tmp, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL,
                   block, g->inc, GrB_NULL)); // Y = F_s H

    for (size_t m = 0; m < g->ntrans; ++m)
    {
        GrB_Index cols[2] = {m * V, (m + 1) * V - 1};
        H_TRY(GrB_Matrix_extract(block, GrB_NULL, GrB_NULL, g->tmp,
                                 GrB_ALL, V, cols, GxB_RANGE, GrB_NULL));
        H_TRY(s_accumulate_block(acc, block, g->to[m], V, acc_empty));
    }
    return GrB_SUCCESS;
}

// Second type: transitions r_m --t_m--> d with one shared destination d.
// D = [F_r0 | ... | F_r(K-1)], J = [G_0; ...; G_(K-1)].
// Y = D J = OR_m(F_rm G_m), already a single V x V destination block.
static GrB_Info s_group_dual(S_Group *g, GrB_Index V, GrB_Matrix M,
                             GrB_Matrix acc, bool *acc_empty)
{
    for (size_t m = 0; m < g->ntrans; ++m)
        H_TRY(s_frontier_slice(g->frontier_tiles[m], M, g->from[m], V));

    H_TRY(GxB_Matrix_concat(g->D, g->frontier_tiles, 1, g->ntrans, GrB_NULL));
    H_TRY(GrB_mxm(g->tmp, GrB_NULL, GrB_NULL, GxB_ANY_PAIR_BOOL,
                   g->D, g->inc, GrB_NULL)); // Y = D J
    H_TRY(s_accumulate_block(acc, g->tmp, g->anchor, V, acc_empty));
    return GrB_SUCCESS;
}

// Keep P1's activity-based family dispatch. Only the representation and
// terminal-transfer operations change; all nonterminal/call/return phases stay
// identical to P1 reviewed. No OpenMP or GraphBLAS thread settings are changed.
static GrB_Info s_apply_groups(S_GroupSet *gs, GrB_Index V, GrB_Matrix M,
                               const bool *active, GrB_Matrix block,
                               GrB_Matrix acc, bool *acc_empty, char *msg)
{
    for (size_t gi = 0; gi < gs->ngroups; ++gi)
    {
        S_Group *g = &gs->groups[gi];
        if (g->ntrans == 0)
            continue;
        if (!g->dual)
        {
            if (active[g->anchor])
                H_TRY(s_group_forward(g, V, M, block, acc, acc_empty));
        }
        else
        {
            bool live = false;
            for (size_t m = 0; m < g->ntrans; ++m)
                live = live || active[g->from[m]];
            if (live)
                H_TRY(s_group_dual(g, V, M, acc, acc_empty));
        }
    }
    return GrB_SUCCESS;
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

// Discover families, build their full block matrices, and build the residual
// ordinary terminal matrices.  On failure, partially built state stays in `gs`
// for the caller's cleanup; helper-local scratch is always released here.
static GrB_Info s_build_groups(S_GroupSet *gs, GrB_Index Q, GrB_Index V, GrB_Index num_term,
                               GrB_Matrix *rsm_term, const GrB_Matrix *graph_term, bool *term_active, char *msg)
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
    memset(&ttup, 0, sizeof(ttup));

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

    // Exactly P1's selected families, using full K*V axes instead of compact
    // endpoint IDs. Fall back to ordinary terminals if a block is too large.
    for (size_t gi = 0; gi < gs->ngroups; ++gi)
    {
        S_Group *g = &gs->groups[gi];
        if (s_mul_exceeds((GrB_Index)g->ntrans, V, CFPQ_MAX_DIM) ||
            s_mul_exceeds(V, (GrB_Index)g->ntrans * V, CFPQ_MAX_DIM))
        {
            for (size_t m = 0; m < g->ntrans; ++m)
                assigned[g->member_tr[m]] = false;
            s_group_free(g);
            continue;
        }
        g->ncols = (GrB_Index)g->ntrans * V;
        info = s_group_build_blocks(g, V, graph_term, msg);
        if (info != GrB_SUCCESS)
            goto cleanup;
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

    // grouped terminal-transfer state and scratch
    S_GroupSet groupset;
    memset(&groupset, 0, sizeof(groupset));
    GrB_Matrix fscratch = GrB_NULL; // V x V row extraction / forward operand

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

        LG_TRY(s_build_groups(&groupset, Q, V, num_term, rsm_term, graph_term, term_active, msg));
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
            LG_TRY(s_apply_groups(&groupset, V, M, active, fscratch, acc, &acc_empty, msg));
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
