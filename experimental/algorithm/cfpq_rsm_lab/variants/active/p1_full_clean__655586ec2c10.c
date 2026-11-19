// Multi-source context-free path querying with a recursive state machine.
// The frontier stores (state, entry vertex, current vertex). A sweep follows
// labeled edges, starts calls, and resumes callers until no new triples appear.
// Based on the p1_full variant (0f85bb5f1dc6).

#include "LAGraphX.h"
#include "LG_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(GxB_SUITESPARSE_GRAPHBLAS)
#define CFPQ_MAX_DIM ((GrB_Index)GxB_INDEX_MAX)
#else
#define CFPQ_MAX_DIM ((GrB_Index)(1ULL << 60))
#endif

// Infer element sizes from the destination, including for arrays of handles.
#define ALLOC(array, n) LAGraph_Malloc((void **)&(array), (n), sizeof *(array), msg)
#define CALLOC(array, n) LAGraph_Calloc((void **)&(array), (n), sizeof *(array), msg)
#define FREE(array) LAGraph_Free((void **)&(array), NULL)

static bool product_fits(GrB_Index a, GrB_Index b)
{
    return b == 0 || a <= CFPQ_MAX_DIM / b;
}

static int check_square(GrB_Matrix matrix, GrB_Index size, const char *description, char *msg)
{
    if (matrix == NULL)
        return GrB_SUCCESS;

    GrB_Index rows, cols;
    GRB_TRY(GrB_Matrix_nrows(&rows, matrix));
    GRB_TRY(GrB_Matrix_ncols(&cols, matrix));
    LG_ASSERT_MSG(rows == size && cols == size, GrB_DIMENSION_MISMATCH, description);
    return GrB_SUCCESS;
}

static int check_inputs(const RSM *rsm, const GrB_Matrix *graph, const GrB_Index *sources,
                        size_t num_sources, GrB_Index V, char *msg)
{
    LG_ASSERT(rsm != NULL, GrB_NULL_POINTER);
    LG_ASSERT(rsm->terminal_count == 0 || (rsm->terminal_matrices && graph), GrB_NULL_POINTER);
    LG_ASSERT(rsm->nonterminal_matrices && rsm->start_states && rsm->final_states,
              GrB_NULL_POINTER);
    LG_ASSERT(num_sources == 0 || sources != NULL, GrB_NULL_POINTER);
    LG_ASSERT(rsm->start_nonterminal < rsm->nonterminal_count, GrB_INVALID_VALUE);
    LG_ASSERT(num_sources <= V, GrB_INVALID_VALUE);

    GrB_Index Q = rsm->state_count;
    LG_ASSERT_MSG(product_fits(V, V) && product_fits(Q, V), GrB_INVALID_VALUE,
                  "flattened matrix dimension exceeds the GraphBLAS limit");
    for (size_t i = 0; i < num_sources; ++i)
        LG_ASSERT_MSG(sources[i] < V, GrB_INVALID_INDEX, "source vertex out of range");

    for (GrB_Index t = 0; t < rsm->terminal_count; ++t)
    {
        LG_TRY(check_square(rsm->terminal_matrices[t], Q, "terminal matrix must be Q x Q", msg));
        LG_TRY(check_square(graph[t], V, "graph matrix must be V x V", msg));
    }
    for (GrB_Index i = 0; i < rsm->nonterminal_count; ++i)
    {
        if (i == rsm->start_nonterminal || rsm->nonterminal_matrices[i] != NULL)
            LG_ASSERT_MSG(rsm->start_states[i] < Q, GrB_INVALID_INDEX, "start state out of range");
        LG_TRY(
            check_square(rsm->nonterminal_matrices[i], Q, "nonterminal matrix must be Q x Q", msg));
        if (rsm->final_states[i] != NULL)
        {
            GrB_Index size;
            GRB_TRY(GrB_Vector_size(&size, rsm->final_states[i]));
            LG_ASSERT_MSG(size == Q, GrB_DIMENSION_MISMATCH, "final vector length must be Q");
        }
    }
    return GrB_SUCCESS;
}

typedef struct
{
    GrB_Index *row, *col;
    bool *value;
    GrB_Index count, capacity;
} Tuples;

static void free_tuples(Tuples *t)
{
    FREE(t->row);
    FREE(t->col);
    FREE(t->value);
}

// Reserve space for an entire batch. On error the owner releases all arrays,
// including any that have already grown.
static int reserve_more(Tuples *t, GrB_Index extra, char *msg)
{
    const size_t limit = SIZE_MAX / sizeof *t->row;
    if (extra > limit - t->count)
        return GrB_OUT_OF_MEMORY;
    GrB_Index needed = t->count + extra;
    if (needed <= t->capacity)
        return GrB_SUCCESS;

    GrB_Index capacity = t->capacity == 0 ? 64 : t->capacity;
    while (capacity < needed)
        capacity = capacity <= limit / 2 ? 2 * capacity : needed;

    LG_TRY(LAGraph_Realloc((void **)&t->row, capacity, t->capacity, sizeof *t->row, msg));
    LG_TRY(LAGraph_Realloc((void **)&t->col, capacity, t->capacity, sizeof *t->col, msg));
    LG_TRY(LAGraph_Realloc((void **)&t->value, capacity, t->capacity, sizeof *t->value, msg));
    t->capacity = capacity;
    return GrB_SUCCESS;
}

static void append_tuple(Tuples *t, GrB_Index row, GrB_Index col)
{
    t->row[t->count] = row;
    t->col[t->count] = col;
    t->value[t->count] = true;
    ++t->count;
}

static int read_tuples(Tuples *t, GrB_Matrix matrix, char *msg)
{
    GrB_Index count;
    GRB_TRY(GrB_Matrix_nvals(&count, matrix));
    t->count = 0;
    LG_TRY(reserve_more(t, count, msg));
    GRB_TRY(GrB_Matrix_extractTuples_BOOL(t->row, t->col, t->value, &count, matrix));
    t->count = count;
    return GrB_SUCCESS;
}

static int build_from_tuples(GrB_Matrix matrix, const Tuples *t, char *msg)
{
    GRB_TRY(GrB_Matrix_build_BOOL(matrix, t->row, t->col, t->value, t->count, GrB_LOR));
    return GrB_SUCCESS;
}

// Each dual family tracks the columns member*V+v for which its graph label
// has an outgoing edge at v. All valid column indices fit below UINT64_MAX.
typedef struct
{
    GrB_Index *keys;
    size_t count, capacity;
} Support;

static size_t support_slot(const Support *s, GrB_Index key)
{
    uint64_t hash = key;
    hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
    hash ^= hash >> 31;

    size_t mask = s->capacity - 1;
    size_t slot = hash & mask;
    while (s->keys[slot] != UINT64_MAX && s->keys[slot] != key)
        slot = (slot + 1) & mask;
    return slot;
}

static int add_support(Support *s, GrB_Index key, char *msg)
{
    if (s->count >= s->capacity - s->capacity / 4)
    {
        if (s->capacity > SIZE_MAX / 2)
            return GrB_OUT_OF_MEMORY;
        Support grown = {.count = s->count, .capacity = s->capacity ? 2 * s->capacity : 64};
        LG_TRY(ALLOC(grown.keys, grown.capacity));
        memset(grown.keys, 0xff, grown.capacity * sizeof *grown.keys);
        for (size_t i = 0; i < s->capacity; ++i)
        {
            if (s->keys[i] != UINT64_MAX)
                grown.keys[support_slot(&grown, s->keys[i])] = s->keys[i];
        }
        FREE(s->keys);
        *s = grown;
    }
    size_t slot = support_slot(s, key);
    if (s->keys[slot] == UINT64_MAX)
    {
        s->keys[slot] = key;
        ++s->count;
    }
    return GrB_SUCCESS;
}

// Terminal families share a source state (forward) or a destination state
// (dual). Each transition belongs to one family or runs separately by label.
typedef struct
{
    bool dual;
    GrB_Index anchor;
    GrB_Matrix incidence, operand; // operand holds forward output or dual input
    Support support;
    Tuples input; // dual frontier entries gathered this sweep
} Family;

typedef struct
{
    Family *family;
    GrB_Index local;
} Member;

typedef struct
{
    GrB_Matrix transitions;
    GrB_Matrix graph; // borrowed adjacency matrix for this label
    GrB_Index *sources;
    GrB_Index count;
} OrdinaryLabel;

typedef struct
{
    Family *families;
    GrB_Index count;
    GrB_Index *dual_offsets; // members with source q occupy [offset[q], offset[q+1])
    Member *dual_members;
    OrdinaryLabel *ordinary;
    GrB_Index ordinary_count;
} TerminalPlan;

static void free_plan(TerminalPlan *plan)
{
    for (GrB_Index i = 0; i < plan->count; ++i)
    {
        Family *f = &plan->families[i];
        GrB_free(&f->incidence);
        GrB_free(&f->operand);
        FREE(f->support.keys);
        free_tuples(&f->input);
    }
    for (GrB_Index i = 0; i < plan->ordinary_count; ++i)
    {
        GrB_free(&plan->ordinary[i].transitions);
        FREE(plan->ordinary[i].sources);
    }
    FREE(plan->families);
    FREE(plan->dual_offsets);
    FREE(plan->dual_members);
    FREE(plan->ordinary);
}

typedef struct
{
    GrB_Index label, from, to;
    Member member;
    bool claimed; // oversized families stay together on the ordinary path
} Transition;

typedef struct
{
    GrB_Index *offsets, *ids;
    GrB_Index branching_states;
} StateIndex;

typedef struct
{
    bool dual;
    GrB_Index anchor, degree;
} Candidate;

// Family discovery scratch expires before the first sweep.
typedef struct
{
    Transition *transitions; // grouped by label
    GrB_Index *label_offsets;
    StateIndex by_source, by_destination;
    GrB_Index *cursor;
    Candidate *candidates;
    GrB_Index dual_count;
    Tuples entries, incidence;
} Setup;

static void free_setup(Setup *s)
{
    FREE(s->transitions);
    FREE(s->label_offsets);
    FREE(s->by_source.offsets);
    FREE(s->by_source.ids);
    FREE(s->by_destination.offsets);
    FREE(s->by_destination.ids);
    FREE(s->cursor);
    FREE(s->candidates);
    free_tuples(&s->entries);
    free_tuples(&s->incidence);
}

static int collect_transitions(Setup *s, const RSM *rsm, const GrB_Matrix *graph, char *msg)
{
    GrB_Index labels = rsm->terminal_count;
    LG_TRY(CALLOC(s->label_offsets, labels + 1));
    GrB_Index total = 0;
    for (GrB_Index t = 0; t < labels; ++t)
    {
        if (rsm->terminal_matrices[t] && graph[t])
        {
            GrB_Index edges;
            GRB_TRY(GrB_Matrix_nvals(&edges, graph[t]));
            if (edges > 0)
            {
                GrB_Index count;
                GRB_TRY(GrB_Matrix_nvals(&count, rsm->terminal_matrices[t]));
                if (count > SIZE_MAX / sizeof(Transition) - total)
                    return GrB_OUT_OF_MEMORY;
                total += count;
            }
        }
        s->label_offsets[t + 1] = total;
    }
    if (total == 0)
        return GrB_SUCCESS;

    LG_TRY(CALLOC(s->transitions, total));
    for (GrB_Index t = 0; t < labels; ++t)
    {
        GrB_Index first = s->label_offsets[t];
        if (first == s->label_offsets[t + 1])
            continue;
        LG_TRY(read_tuples(&s->entries, rsm->terminal_matrices[t], msg));
        for (GrB_Index k = 0; k < s->entries.count; ++k)
        {
            Transition *tr = &s->transitions[first + k];
            tr->label = t;
            tr->from = s->entries.row[k];
            tr->to = s->entries.col[k];
        }
    }
    return GrB_SUCCESS;
}

// Convert counts stored at offset[q+1] to ranges and initialize write positions.
static void finish_offsets(GrB_Index *offsets, GrB_Index *cursor, GrB_Index Q)
{
    for (GrB_Index q = 0; q < Q; ++q)
    {
        offsets[q + 1] += offsets[q];
        cursor[q] = offsets[q];
    }
}

static int index_transitions(StateIndex *index, const Transition *tr, GrB_Index count, GrB_Index Q,
                             bool dual, GrB_Index *cursor, char *msg)
{
    LG_TRY(CALLOC(index->offsets, Q + 1));
    LG_TRY(ALLOC(index->ids, count));
    for (GrB_Index k = 0; k < count; ++k)
    {
        GrB_Index state = dual ? tr[k].to : tr[k].from;
        if (++index->offsets[state + 1] == 2)
            ++index->branching_states;
    }
    finish_offsets(index->offsets, cursor, Q);
    for (GrB_Index k = 0; k < count; ++k)
    {
        GrB_Index state = dual ? tr[k].to : tr[k].from;
        index->ids[cursor[state]++] = k;
    }
    return GrB_SUCCESS;
}

static int compare_candidates(const void *left, const void *right)
{
    const Candidate *a = left, *b = right;
    if (a->degree != b->degree)
        return a->degree > b->degree ? -1 : 1;
    if (a->dual != b->dual)
        return a->dual ? 1 : -1;
    return (a->anchor > b->anchor) - (a->anchor < b->anchor);
}

static int build_family(Family *f, Setup *s, const GrB_Index *ids, GrB_Index count,
                        const GrB_Matrix *graph, GrB_Index V, GrB_Index width, char *msg)
{
    Tuples *inc = &s->incidence, *edges = &s->entries;
    inc->count = 0;
    for (GrB_Index m = 0; m < count; ++m)
    {
        Transition *tr = &s->transitions[ids[m]];
        tr->member = (Member){f, m};
        LG_TRY(read_tuples(edges, graph[tr->label], msg));
        LG_TRY(reserve_more(inc, edges->count, msg));
        for (GrB_Index k = 0; k < edges->count; ++k)
        {
            GrB_Index v = edges->row[k], w = edges->col[k];
            if (f->dual)
            {
                GrB_Index column = m * V + v;
                LG_TRY(add_support(&f->support, column, msg));
                append_tuple(inc, column, w);
            }
            else
                append_tuple(inc, v, tr->to * V + w);
        }
    }
    // Forward labels with the same continuation share a column. Dual labels
    // use distinct member blocks even when they share both endpoint states.
    GRB_TRY(GrB_Matrix_new(&f->incidence, GrB_BOOL, f->dual ? width : V, f->dual ? V : width));
    LG_TRY(build_from_tuples(f->incidence, inc, msg));
    GRB_TRY(GrB_Matrix_new(&f->operand, GrB_BOOL, V, width));
    return GrB_SUCCESS;
}

static int select_families(TerminalPlan *plan, Setup *s, GrB_Index total, GrB_Index Q, GrB_Index V,
                           const GrB_Matrix *graph, char *msg)
{
    if (total < 2)
        return GrB_SUCCESS;
    LG_TRY(ALLOC(s->cursor, Q));
    LG_TRY(index_transitions(&s->by_source, s->transitions, total, Q, false, s->cursor, msg));
    LG_TRY(index_transitions(&s->by_destination, s->transitions, total, Q, true, s->cursor, msg));

    GrB_Index capacity = s->by_source.branching_states + s->by_destination.branching_states;
    if (capacity == 0)
        return GrB_SUCCESS;
    LG_TRY(ALLOC(s->candidates, capacity));
    GrB_Index n = 0;
    for (GrB_Index q = 0; q < Q; ++q)
    {
        GrB_Index outgoing = s->by_source.offsets[q + 1] - s->by_source.offsets[q];
        GrB_Index incoming = s->by_destination.offsets[q + 1] - s->by_destination.offsets[q];
        if (outgoing >= 2)
            s->candidates[n++] = (Candidate){false, q, outgoing};
        if (incoming >= 2)
            s->candidates[n++] = (Candidate){true, q, incoming};
    }
    // Greedily claim the largest family first. Ties favor a shared source,
    // then the lowest state index.
    qsort(s->candidates, n, sizeof *s->candidates, compare_candidates);
    LG_TRY(CALLOC(plan->families, LAGRAPH_MIN(n, total / 2)));

    for (GrB_Index i = 0; i < n; ++i)
    {
        Candidate c = s->candidates[i];
        StateIndex *index = c.dual ? &s->by_destination : &s->by_source;
        GrB_Index *ids = index->ids + index->offsets[c.anchor];
        // Each endpoint range is visited once, so compact it in place.
        GrB_Index count = 0;
        for (GrB_Index k = 0; k < c.degree; ++k)
            if (!s->transitions[ids[k]].claimed)
                ids[count++] = ids[k];
        if (count < 2)
            continue;
        for (GrB_Index k = 0; k < count; ++k)
            s->transitions[ids[k]].claimed = true;

        GrB_Index blocks = c.dual ? count : Q;
        if (!product_fits(blocks, V) || !product_fits(blocks * V, V))
            continue;

        Family *f = &plan->families[plan->count++];
        f->anchor = c.anchor;
        f->dual = c.dual;
        if (f->dual)
            s->dual_count += count;
        LG_TRY(build_family(f, s, ids, count, graph, V, blocks * V, msg));
    }
    return GrB_SUCCESS;
}

static int index_dual_members(TerminalPlan *plan, Setup *s, GrB_Index total, GrB_Index Q, char *msg)
{
    if (s->dual_count == 0)
        return GrB_SUCCESS;

    LG_TRY(CALLOC(plan->dual_offsets, Q + 1));
    LG_TRY(ALLOC(plan->dual_members, s->dual_count));
    for (GrB_Index k = 0; k < total; ++k)
    {
        Transition *tr = &s->transitions[k];
        if (tr->member.family && tr->member.family->dual)
            ++plan->dual_offsets[tr->from + 1];
    }
    finish_offsets(plan->dual_offsets, s->cursor, Q);
    for (GrB_Index k = 0; k < total; ++k)
    {
        Transition *tr = &s->transitions[k];
        if (tr->member.family && tr->member.family->dual)
            plan->dual_members[s->cursor[tr->from]++] = tr->member;
    }
    return GrB_SUCCESS;
}

static int build_ordinary_labels(TerminalPlan *plan, Setup *s, const RSM *rsm,
                                 const GrB_Matrix *graph, char *msg)
{
    Tuples *edges = &s->entries;
    for (GrB_Index t = 0; t < rsm->terminal_count; ++t)
    {
        edges->count = 0;
        GrB_Index first = s->label_offsets[t], end = s->label_offsets[t + 1];
        LG_TRY(reserve_more(edges, end - first, msg));
        for (GrB_Index k = first; k < end; ++k)
        {
            Transition *tr = &s->transitions[k];
            if (tr->member.family == NULL)
                append_tuple(edges, tr->from, tr->to);
        }
        if (edges->count == 0)
            continue;
        if (plan->ordinary == NULL)
            LG_TRY(CALLOC(plan->ordinary, rsm->terminal_count));
        OrdinaryLabel *label = &plan->ordinary[plan->ordinary_count++];
        label->graph = graph[t];
        label->count = edges->count;
        LG_TRY(ALLOC(label->sources, label->count));
        memcpy(label->sources, edges->row, label->count * sizeof *label->sources);
        GRB_TRY(GrB_Matrix_new(&label->transitions, GrB_BOOL, rsm->state_count, rsm->state_count));
        LG_TRY(build_from_tuples(label->transitions, edges, msg));
    }
    return GrB_SUCCESS;
}

static int build_plan(TerminalPlan *plan, Setup *s, const RSM *rsm, const GrB_Matrix *graph,
                      GrB_Index V, char *msg)
{
    if (rsm->terminal_count == 0)
        return GrB_SUCCESS;
    LG_TRY(collect_transitions(s, rsm, graph, msg));
    GrB_Index total = s->label_offsets[rsm->terminal_count];
    if (total == 0)
        return GrB_SUCCESS;

    LG_TRY(select_families(plan, s, total, rsm->state_count, V, graph, msg));
    LG_TRY(index_dual_members(plan, s, total, rsm->state_count, msg));
    return build_ordinary_labels(plan, s, rsm, graph, msg);
}

// During a sweep, contributions use rows state*V+entry and columns current.
// The first nonempty product becomes the accumulator by exchanging handles.
typedef struct
{
    GrB_Matrix entries, product;
    bool empty;
} Accumulator;

static int merge_product(Accumulator *acc, char *msg)
{
    GrB_Index count;
    GRB_TRY(GrB_Matrix_nvals(&count, acc->product));
    if (count == 0)
        return GrB_SUCCESS;

    if (acc->empty)
    {
        GrB_Matrix swap = acc->entries;
        acc->entries = acc->product;
        acc->product = swap;
        acc->empty = false;
    }
    else
        GRB_TRY(GrB_eWiseAdd(acc->entries, NULL, NULL, GrB_LOR, acc->entries, acc->product, NULL));
    return GrB_SUCCESS;
}

static int add_product(Accumulator *acc, GrB_Matrix left, GrB_Matrix right, char *msg)
{
    GRB_TRY(GrB_mxm(acc->product, NULL, NULL, GxB_ANY_PAIR_BOOL, left, right, NULL));
    return merge_product(acc, msg);
}

typedef struct
{
    GrB_Matrix summary; // completed (entry, exit) pairs for this box
    GrB_Matrix waiting; // (continuation state * V + caller entry, callee entry)
} Box;

typedef struct
{
    const RSM *rsm;
    GrB_Index V;

    TerminalPlan terminals;
    Box *boxes;
    GrB_Matrix frontier, seen, calls; // row = state, column = entry*V+current
    GrB_Matrix shifted, returns, square;
    Accumulator acc;
    GrB_Vector source_mask, active_rows, call_entries;
    GrB_Index *active_states;
    bool *active;
    Tuples scratch, call_tuples;
} Query;

static void free_query(Query *q)
{
    free_plan(&q->terminals);
    if (q->boxes != NULL)
    {
        for (GrB_Index i = 0; i < q->rsm->nonterminal_count; ++i)
        {
            GrB_free(&q->boxes[i].summary);
            GrB_free(&q->boxes[i].waiting);
        }
        FREE(q->boxes);
    }
    GrB_free(&q->frontier);
    GrB_free(&q->seen);
    GrB_free(&q->calls);
    GrB_free(&q->shifted);
    GrB_free(&q->returns);
    GrB_free(&q->square);
    GrB_free(&q->acc.entries);
    GrB_free(&q->acc.product);
    GrB_free(&q->source_mask);
    GrB_free(&q->active_rows);
    GrB_free(&q->call_entries);
    FREE(q->active_states);
    FREE(q->active);
    free_tuples(&q->scratch);
    free_tuples(&q->call_tuples);
}

static int init_query(Query *q, const GrB_Matrix *graph, const GrB_Index *sources,
                      size_t num_sources, char *msg)
{
    const RSM *rsm = q->rsm;
    GrB_Index Q = rsm->state_count, V = q->V;

    Setup setup = {0};
    int status = build_plan(&q->terminals, &setup, rsm, graph, V, msg);
    free_setup(&setup);
    LG_TRY(status);

    LG_TRY(CALLOC(q->boxes, rsm->nonterminal_count));
    for (GrB_Index i = 0; i < rsm->nonterminal_count; ++i)
    {
        GRB_TRY(GrB_Matrix_new(&q->boxes[i].summary, GrB_BOOL, V, V));
        if (rsm->nonterminal_matrices[i] != NULL)
            GRB_TRY(GrB_Matrix_new(&q->boxes[i].waiting, GrB_BOOL, Q * V, V));
    }

    GRB_TRY(GrB_Matrix_new(&q->frontier, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&q->calls, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&q->shifted, GrB_BOOL, Q, V * V));
    GRB_TRY(GrB_Matrix_new(&q->returns, GrB_BOOL, 1, V * V));
    GRB_TRY(GrB_Matrix_new(&q->acc.entries, GrB_BOOL, Q * V, V));
    GRB_TRY(GrB_Matrix_new(&q->acc.product, GrB_BOOL, Q * V, V));
    q->acc.empty = true;
    if (q->terminals.count > 0)
        GRB_TRY(GrB_Matrix_new(&q->square, GrB_BOOL, V, V));
    GRB_TRY(GrB_Vector_new(&q->source_mask, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(&q->call_entries, GrB_BOOL, V));
    GRB_TRY(GrB_Vector_new(&q->active_rows, GrB_BOOL, Q));
    LG_TRY(ALLOC(q->active, Q));
    LG_TRY(ALLOC(q->active_states, Q));

    GrB_Index start = rsm->start_states[rsm->start_nonterminal];
    for (size_t i = 0; i < num_sources; ++i)
    {
        GrB_Index s = sources[i];
        GRB_TRY(GrB_Matrix_setElement_BOOL(q->frontier, true, start, s * V + s));
        GRB_TRY(GrB_Vector_setElement_BOOL(q->source_mask, true, s));
    }
    GRB_TRY(GrB_Matrix_dup(&q->seen, q->frontier));
    return GrB_SUCCESS;
}

// Route a family product back to the shared (state*V+entry, current) layout.
static int scatter_family(Query *q, const Family *f, GrB_Matrix product, char *msg)
{
    Tuples *t = &q->scratch;
    LG_TRY(read_tuples(t, product, msg));
    if (t->count == 0)
        return GrB_SUCCESS;
    for (GrB_Index k = 0; k < t->count; ++k)
    {
        GrB_Index state = f->anchor;
        if (!f->dual)
        {
            state = t->col[k] / q->V;
            t->col[k] %= q->V;
        }
        t->row[k] += state * q->V;
    }
    GRB_TRY(GrB_Matrix_clear(q->acc.product));
    LG_TRY(build_from_tuples(q->acc.product, t, msg));
    return merge_product(&q->acc, msg);
}

static int apply_forward(Query *q, Family *f, char *msg)
{
    GrB_Index V = q->V;
    GRB_TRY(GxB_Matrix_reshape(q->square, false, 1, V * V, NULL));
    GRB_TRY(GrB_Matrix_extract(q->square, NULL, NULL, q->frontier, &f->anchor, 1, GrB_ALL, V * V,
                               NULL));
    GRB_TRY(GxB_Matrix_reshape(q->square, false, V, V, NULL));
    GRB_TRY(GrB_mxm(f->operand, NULL, NULL, GxB_ANY_PAIR_BOOL, q->square, f->incidence, NULL));
    return scatter_family(q, f, f->operand, msg);
}

static int apply_dual(Query *q, char *msg)
{
    TerminalPlan *plan = &q->terminals;
    Tuples *frontier = &q->scratch;
    LG_TRY(read_tuples(frontier, q->frontier, msg));

    // Scan the frontier once for all dual families. Route only to members
    // whose label has an outgoing graph edge at the current vertex.
    for (GrB_Index k = 0; k < frontier->count; ++k)
    {
        GrB_Index state = frontier->row[k];
        GrB_Index entry = frontier->col[k] / q->V;
        GrB_Index current = frontier->col[k] % q->V;
        for (GrB_Index p = plan->dual_offsets[state]; p < plan->dual_offsets[state + 1]; ++p)
        {
            Member m = plan->dual_members[p];
            GrB_Index column = m.local * q->V + current;
            Support *support = &m.family->support;
            if (support->keys[support_slot(support, column)] != column)
                continue;
            Tuples *input = &m.family->input;
            LG_TRY(reserve_more(input, 1, msg));
            append_tuple(input, entry, column);
        }
    }

    for (GrB_Index i = 0; i < plan->count; ++i)
    {
        Family *f = &plan->families[i];
        Tuples *input = &f->input;
        if (input->count == 0)
            continue;
        GRB_TRY(GrB_Matrix_clear(f->operand));
        LG_TRY(build_from_tuples(f->operand, input, msg));
        input->count = 0;
        GRB_TRY(GrB_mxm(q->square, NULL, NULL, GxB_ANY_PAIR_BOOL, f->operand, f->incidence, NULL));
        LG_TRY(scatter_family(q, f, q->square, msg));
    }
    return GrB_SUCCESS;
}

static int shift_frontier(Query *q, GrB_Matrix transitions, char *msg)
{
    GRB_TRY(
        GrB_mxm(q->shifted, NULL, NULL, GxB_ANY_PAIR_BOOL, transitions, q->frontier, GrB_DESC_T0));
    GRB_TRY(GxB_Matrix_reshape(q->shifted, false, q->rsm->state_count * q->V, q->V, NULL));
    return GrB_SUCCESS;
}

static int reset_shifted(Query *q, char *msg)
{
    GRB_TRY(GrB_Matrix_clear(q->shifted));
    GRB_TRY(GxB_Matrix_reshape(q->shifted, false, q->rsm->state_count, q->V * q->V, NULL));
    return GrB_SUCCESS;
}

static int follow_terminals(Query *q, char *msg)
{
    TerminalPlan *plan = &q->terminals;
    if (plan->count == 0 && plan->ordinary_count == 0)
        return GrB_SUCCESS;
    GrB_Index count = q->rsm->state_count;
    GRB_TRY(GrB_reduce(q->active_rows, NULL, NULL, GrB_LOR_MONOID_BOOL, q->frontier, NULL));
    GRB_TRY(GrB_Vector_extractTuples_BOOL(q->active_states, NULL, &count, q->active_rows));
    memset(q->active, 0, q->rsm->state_count * sizeof *q->active);
    bool any_dual = false;
    for (GrB_Index k = 0; k < count; ++k)
    {
        GrB_Index state = q->active_states[k];
        q->active[state] = true;
        if (plan->dual_offsets && plan->dual_offsets[state] != plan->dual_offsets[state + 1])
            any_dual = true;
    }
    for (GrB_Index i = 0; i < plan->count; ++i)
    {
        Family *f = &plan->families[i];
        if (!f->dual && q->active[f->anchor])
            LG_TRY(apply_forward(q, f, msg));
    }
    if (any_dual)
        LG_TRY(apply_dual(q, msg));

    for (GrB_Index i = 0; i < plan->ordinary_count; ++i)
    {
        OrdinaryLabel *label = &plan->ordinary[i];
        for (GrB_Index k = 0; k < label->count; ++k)
        {
            if (q->active[label->sources[k]])
            {
                LG_TRY(shift_frontier(q, label->transitions, msg));
                LG_TRY(add_product(&q->acc, q->shifted, label->graph, msg));
                LG_TRY(reset_shifted(q, msg));
                break;
            }
        }
    }
    return GrB_SUCCESS;
}

// Every nonempty column v in the shifted frontier starts the box at (v, v).
static int collect_calls(Query *q, GrB_Index start, char *msg)
{
    GRB_TRY(GrB_reduce(q->call_entries, NULL, NULL, GrB_LOR_MONOID_BOOL, q->shifted, GrB_DESC_T0));
    GrB_Index count;
    GRB_TRY(GrB_Vector_nvals(&count, q->call_entries));
    Tuples *calls = &q->call_tuples;
    LG_TRY(reserve_more(calls, count, msg));
    GrB_Index first = calls->count;
    GRB_TRY(GrB_Vector_extractTuples_BOOL(calls->col + first, NULL, &count, q->call_entries));
    for (GrB_Index k = first; k < first + count; ++k)
    {
        GrB_Index v = calls->col[k];
        calls->col[k] = v * q->V + v;
        calls->row[k] = start;
        calls->value[k] = true;
    }
    calls->count += count;
    return GrB_SUCCESS;
}

static int follow_calls(Query *q, char *msg)
{
    Tuples *calls = &q->call_tuples;
    calls->count = 0;
    for (GrB_Index i = 0; i < q->rsm->nonterminal_count; ++i)
    {
        GrB_Matrix transitions = q->rsm->nonterminal_matrices[i];
        if (transitions == NULL)
            continue;
        LG_TRY(shift_frontier(q, transitions, msg));
        GrB_Index count;
        GRB_TRY(GrB_Matrix_nvals(&count, q->shifted));
        if (count > 0)
        {
            Box *box = &q->boxes[i];
            LG_TRY(collect_calls(q, q->rsm->start_states[i], msg));
            // Follow known summaries now and save the callers for future returns.
            GrB_Index known;
            GRB_TRY(GrB_Matrix_nvals(&known, box->summary));
            if (known > 0)
                LG_TRY(add_product(&q->acc, q->shifted, box->summary, msg));
            GRB_TRY(
                GrB_eWiseAdd(box->waiting, NULL, NULL, GrB_LOR, box->waiting, q->shifted, NULL));
        }
        LG_TRY(reset_shifted(q, msg));
    }
    GRB_TRY(GrB_Matrix_clear(q->calls));
    if (calls->count > 0)
        LG_TRY(build_from_tuples(q->calls, calls, msg));
    return GrB_SUCCESS;
}

static int resume_callers(Query *q, Box *box, char *msg)
{
    // Remove pairs already in the box summary using its complemented structure.
    GrB_Index count;
    GRB_TRY(GrB_Matrix_nvals(&count, box->summary));
    if (count > 0)
    {
        GRB_TRY(
            GrB_apply(q->returns, box->summary, NULL, GrB_IDENTITY_BOOL, q->returns, GrB_DESC_RSC));
        GRB_TRY(GrB_Matrix_nvals(&count, q->returns));
        if (count == 0)
            return GrB_SUCCESS;
    }
    GRB_TRY(GrB_eWiseAdd(box->summary, NULL, NULL, GrB_LOR, box->summary, q->returns, NULL));
    if (box->waiting != NULL)
    {
        GRB_TRY(GrB_Matrix_nvals(&count, box->waiting));
        if (count > 0)
            LG_TRY(add_product(&q->acc, box->waiting, q->returns, msg));
    }
    return GrB_SUCCESS;
}

static int follow_returns(Query *q, char *msg)
{
    for (GrB_Index i = 0; i < q->rsm->nonterminal_count; ++i)
    {
        GrB_Vector finals = q->rsm->final_states[i];
        if (finals == NULL)
            continue;
        // SuiteSparse vectors can serve as single-column matrices.
        GRB_TRY(GrB_mxm(q->returns, NULL, NULL, GxB_ANY_PAIR_BOOL, (GrB_Matrix)finals, q->frontier,
                        GrB_DESC_T0));
        GrB_Index count;
        GRB_TRY(GrB_Matrix_nvals(&count, q->returns));
        if (count == 0)
            continue;
        GRB_TRY(GxB_Matrix_reshape(q->returns, false, q->V, q->V, NULL));
        LG_TRY(resume_callers(q, &q->boxes[i], msg));
        GRB_TRY(GxB_Matrix_reshape(q->returns, false, 1, q->V * q->V, NULL));
    }
    return GrB_SUCCESS;
}

static int advance_frontier(Query *q, char *msg)
{
    // New frontier = (accumulated moves union calls) minus previously seen triples.
    GRB_TRY(GrB_free(&q->frontier));
    GRB_TRY(GxB_Matrix_reshapeDup(&q->frontier, q->acc.entries, false, q->rsm->state_count,
                                  q->V * q->V, NULL));
    GRB_TRY(GrB_Matrix_clear(q->acc.entries));
    q->acc.empty = true;
    GRB_TRY(GrB_eWiseAdd(q->frontier, q->seen, NULL, GrB_LOR, q->frontier, q->calls, GrB_DESC_RSC));
    GRB_TRY(GrB_eWiseAdd(q->seen, NULL, NULL, GrB_LOR, q->seen, q->frontier, NULL));
    return GrB_SUCCESS;
}

static int run_query(Query *q, GrB_Vector *reachable, char *msg)
{
    for (;;)
    {
        GrB_Index count;
        GRB_TRY(GrB_Matrix_nvals(&count, q->frontier));
        if (count == 0)
            break;
        LG_TRY(follow_terminals(q, msg));
        LG_TRY(follow_calls(q, msg));
        LG_TRY(follow_returns(q, msg));
        LG_TRY(advance_frontier(q, msg));
    }

    Box *start = &q->boxes[q->rsm->start_nonterminal];
    GRB_TRY(GrB_Vector_new(reachable, GrB_BOOL, q->V));
    GRB_TRY(GrB_mxv(*reachable, NULL, NULL, GxB_ANY_PAIR_BOOL, start->summary, q->source_mask,
                    GrB_DESC_T0));
    return GrB_SUCCESS;
}

int LAGraph_CFPQ_RSM(GrB_Vector *reachable, const RSM *rsm, const GrB_Matrix *graph_term,
                     const GrB_Index *sources, size_t num_sources, GrB_Index V, char *msg)
{
    LG_CLEAR_MSG;
    LG_ASSERT(reachable != NULL, GrB_NULL_POINTER);
    *reachable = NULL;
    LG_TRY(check_inputs(rsm, graph_term, sources, num_sources, V, msg));

    Query q = {.rsm = rsm, .V = V};
    int status = init_query(&q, graph_term, sources, num_sources, msg);
    if (status == GrB_SUCCESS)
        status = run_query(&q, reachable, msg);
    free_query(&q);
    if (status != GrB_SUCCESS)
        GrB_free(reachable);
    return status;
}
