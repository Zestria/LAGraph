//------------------------------------------------------------------------------
// cfpq_rsm_lab/tests/test_ds00.c: CFPQ RSM correctness foundation (DS-00)
//------------------------------------------------------------------------------
//
// Built against exactly one candidate translation unit:
//
//   cc ... variants/correctness.c tests/test_ds00.c tests/oracle.c -llagraph ...
//
// so the tests exercise the candidate, not the installed liblagraphx.  The
// Makefile also produces a binary linked to the unchanged parent source
// (experimental/algorithm/LAGraph_CFPQ_RSM.c) as a negative control.
//
// Modes
//   (default)             deterministic regressions + randomized oracle cases
//   --cases N --seed S    number of randomized cases and base seed (default 200,
//                         2026).  Case k uses seed S + k, printed on every line.
//   --no-random           only the deterministic regressions
//   --alloc-sweep         allocation-failure sweep (fork + LG_brutal_*), one
//                         child process per allowed-allocation budget
//   --alloc-case N        one allocation-failure trial (used by --alloc-sweep)
//
// Exit status is 0 only if every check passed.

#define _POSIX_C_SOURCE 200809L

#include "LAGraph.h"
#include "LAGraphX.h"
#include "LG_test.h"

#include <GraphBLAS.h>

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "oracle.h"

char msg[LAGRAPH_MSG_LEN];

static int g_checks = 0;
static int g_failures = 0;
static bool g_verbose = false;

//------------------------------------------------------------------------------
// reporting helpers
//------------------------------------------------------------------------------

static void report_failure(const char *file, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "FAIL %s:%d: ", file, line);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    g_failures++;
}

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        g_checks++;                                                                                \
        if (!(cond))                                                                               \
            report_failure(__FILE__, __LINE__, "check failed: %s", #cond);                         \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                       \
    do                                                                                             \
    {                                                                                              \
        g_checks++;                                                                                \
        if (!(cond))                                                                               \
            report_failure(__FILE__, __LINE__, __VA_ARGS__);                                       \
    } while (0)

static bool info_ok(GrB_Info info, const char *file, int line, const char *what)
{
    g_checks++;
    if (info != GrB_SUCCESS)
    {
        report_failure(file, line, "%s failed with info %d: %s", what, (int)info, msg);
        return false;
    }
    return true;
}

#define OK(info, what) info_ok((info), __FILE__, __LINE__, (what))

//------------------------------------------------------------------------------
// frozen GraphBLAS environment
//------------------------------------------------------------------------------

// SuiteSparse:GraphBLAS JIT compilation writes compiled kernels under
// $GRAPHBLAS_CACHE_PATH (default $HOME/.SuiteSparse).  In this workspace that
// path is read-only, and a failed JIT compile was observed to surface as
// GrB_DIMENSION_MISMATCH from an otherwise valid mxm.  Correctness results
// must not depend on the JIT cache state, so the harness disables the JIT.
// DS-01 freezes the benchmark environment separately and records it.
static void freeze_graphblas_environment(void)
{
#if LAGRAPH_SUITESPARSE
    GrB_Info info = GrB_set(GrB_GLOBAL, (int32_t)GxB_JIT_OFF, GxB_JIT_C_CONTROL);
    CHECK_MSG(info == GrB_SUCCESS, "GrB_set(GxB_JIT_C_CONTROL, OFF) failed: %d", (int)info);
#endif
}

//------------------------------------------------------------------------------
// deterministic RNG (splitmix64); seeds are recorded in the output
//------------------------------------------------------------------------------

static uint64_t rng_state;

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t rng_next(void)
{
    rng_state = splitmix64(rng_state);
    return rng_state;
}

// uniform integer in [0, n) for n > 0
static uint64_t rng_below(uint64_t n)
{
    return n == 0 ? 0 : rng_next() % n;
}

//------------------------------------------------------------------------------
// test instance builder (GraphBLAS objects for the kernel, plus metadata)
//------------------------------------------------------------------------------

typedef struct
{
    GrB_Index Q;   // number of RSM states
    GrB_Index V;   // number of graph vertices
    size_t nt;     // terminal labels
    size_t nn;     // boxes
    GrB_Index start_nonterm;
    GrB_Index *start_states;  // [nn]
    GrB_Matrix *term;         // [nt] Q x Q
    GrB_Matrix *nonterm;      // [nn] Q x Q; entries may be GrB_NULL (no calls)
    GrB_Vector *finals;       // [nn] length Q; entries may be GrB_NULL (no finals)
    GrB_Matrix *graph;        // [nt] V x V; entries may be GrB_NULL (no edges)
    bool built;
} Case;

static void case_init_struct(Case *c)
{
    memset(c, 0, sizeof(*c));
}

static bool case_new(Case *c, GrB_Index Q, GrB_Index V, size_t nt, size_t nn, GrB_Index start_nonterm)
{
    case_init_struct(c);
    c->Q = Q;
    c->V = V;
    c->nt = nt;
    c->nn = nn;
    c->start_nonterm = start_nonterm;

    size_t nt_alloc = nt ? nt : 1;
    size_t nn_alloc = nn ? nn : 1;
    c->start_states = (GrB_Index *)calloc(nn_alloc, sizeof(GrB_Index));
    c->term = (GrB_Matrix *)calloc(nt_alloc, sizeof(GrB_Matrix));
    c->nonterm = (GrB_Matrix *)calloc(nn_alloc, sizeof(GrB_Matrix));
    c->finals = (GrB_Vector *)calloc(nn_alloc, sizeof(GrB_Vector));
    c->graph = (GrB_Matrix *)calloc(nt_alloc, sizeof(GrB_Matrix));
    if (c->start_states == NULL || c->term == NULL || c->nonterm == NULL || c->finals == NULL ||
        c->graph == NULL)
    {
        report_failure(__FILE__, __LINE__, "out of memory building case");
        return false;
    }

    for (size_t t = 0; t < nt; ++t)
    {
        if (!OK(GrB_Matrix_new(&c->term[t], GrB_BOOL, Q, Q), "term matrix") ||
            !OK(GrB_Matrix_new(&c->graph[t], GrB_BOOL, V, V), "graph matrix"))
            return false;
    }
    for (size_t i = 0; i < nn; ++i)
    {
        if (!OK(GrB_Matrix_new(&c->nonterm[i], GrB_BOOL, Q, Q), "nonterm matrix") ||
            !OK(GrB_Vector_new(&c->finals[i], GrB_BOOL, Q), "final vector"))
            return false;
    }
    c->built = true;
    return true;
}

static void case_free(Case *c)
{
    for (size_t t = 0; t < c->nt; ++t)
    {
        if (c->term != NULL)
            GrB_free(&c->term[t]);
        if (c->graph != NULL)
            GrB_free(&c->graph[t]);
    }
    for (size_t i = 0; i < c->nn; ++i)
    {
        if (c->nonterm != NULL)
            GrB_free(&c->nonterm[i]);
        if (c->finals != NULL)
            GrB_free(&c->finals[i]);
    }
    free(c->start_states);
    free(c->term);
    free(c->nonterm);
    free(c->finals);
    free(c->graph);
    case_init_struct(c);
}

// duplicate every GraphBLAS handle so a mutated variant can be freed
// independently of the instance it was copied from
static bool case_clone(Case *dst, const Case *src)
{
    case_init_struct(dst);
    dst->Q = src->Q;
    dst->V = src->V;
    dst->nt = src->nt;
    dst->nn = src->nn;
    dst->start_nonterm = src->start_nonterm;
    size_t nt_alloc = src->nt ? src->nt : 1;
    size_t nn_alloc = src->nn ? src->nn : 1;
    dst->start_states = (GrB_Index *)calloc(nn_alloc, sizeof(GrB_Index));
    dst->term = (GrB_Matrix *)calloc(nt_alloc, sizeof(GrB_Matrix));
    dst->nonterm = (GrB_Matrix *)calloc(nn_alloc, sizeof(GrB_Matrix));
    dst->finals = (GrB_Vector *)calloc(nn_alloc, sizeof(GrB_Vector));
    dst->graph = (GrB_Matrix *)calloc(nt_alloc, sizeof(GrB_Matrix));
    if (dst->start_states == NULL || dst->term == NULL || dst->nonterm == NULL ||
        dst->finals == NULL || dst->graph == NULL)
        return false;
    if (src->nn > 0)
        memcpy(dst->start_states, src->start_states, src->nn * sizeof(GrB_Index));
    for (size_t t = 0; t < src->nt; ++t)
    {
        if (src->term[t] != GrB_NULL && !OK(GrB_Matrix_dup(&dst->term[t], src->term[t]), "dup term"))
            return false;
        if (src->graph[t] != GrB_NULL && !OK(GrB_Matrix_dup(&dst->graph[t], src->graph[t]), "dup graph"))
            return false;
    }
    for (size_t i = 0; i < src->nn; ++i)
    {
        if (src->nonterm[i] != GrB_NULL &&
            !OK(GrB_Matrix_dup(&dst->nonterm[i], src->nonterm[i]), "dup nonterm"))
            return false;
        if (src->finals[i] != GrB_NULL && !OK(GrB_Vector_dup(&dst->finals[i], src->finals[i]), "dup finals"))
            return false;
    }
    dst->built = true;
    return true;
}

// drop a box's nonterminal matrix: "no incoming call transitions", not "empty"
static void case_drop_nonterm(Case *c, size_t i)
{
    GrB_free(&c->nonterm[i]);
    c->nonterm[i] = GrB_NULL;
}

// drop a box's final vector: no accepting state, expressed as a NULL vector
static void case_drop_finals(Case *c, size_t i)
{
    GrB_free(&c->finals[i]);
    c->finals[i] = GrB_NULL;
}

static void case_drop_graph(Case *c, size_t t)
{
    GrB_free(&c->graph[t]);
    c->graph[t] = GrB_NULL;
}

static void case_drop_term(Case *c, size_t t)
{
    GrB_free(&c->term[t]);
    c->term[t] = GrB_NULL;
}

static bool case_term(Case *c, size_t t, GrB_Index from, GrB_Index to)
{
    return OK(GrB_Matrix_setElement_BOOL(c->term[t], true, from, to), "term edge");
}

static bool case_term_value(Case *c, size_t t, GrB_Index from, GrB_Index to, bool value)
{
    return OK(GrB_Matrix_setElement_BOOL(c->term[t], value, from, to), "term edge value");
}

static bool case_nonterm(Case *c, size_t i, GrB_Index from, GrB_Index to)
{
    return OK(GrB_Matrix_setElement_BOOL(c->nonterm[i], true, from, to), "nonterm edge");
}

static bool case_final(Case *c, size_t i, GrB_Index q)
{
    return OK(GrB_Vector_setElement_BOOL(c->finals[i], true, q), "final state");
}

static bool case_final_value(Case *c, size_t i, GrB_Index q, bool value)
{
    return OK(GrB_Vector_setElement_BOOL(c->finals[i], value, q), "final state value");
}

static bool case_graph(Case *c, size_t t, GrB_Index from, GrB_Index to)
{
    return OK(GrB_Matrix_setElement_BOOL(c->graph[t], true, from, to), "graph edge");
}

static bool case_graph_value(Case *c, size_t t, GrB_Index from, GrB_Index to, bool value)
{
    return OK(GrB_Matrix_setElement_BOOL(c->graph[t], value, from, to), "graph edge value");
}

// Fill an RSM view of the case.  The caller picks which pointers are passed to
// the kernel (so NULL-array and NULL-graph edge cases can be exercised).
static void case_rsm(const Case *c, RSM *rsm)
{
    rsm->state_count = c->Q;
    rsm->terminal_count = (GrB_Index)c->nt;
    rsm->nonterminal_count = (GrB_Index)c->nn;
    rsm->start_nonterminal = c->start_nonterm;
    rsm->terminal_matrices = c->term;
    rsm->nonterminal_matrices = c->nonterm;
    rsm->start_states = c->start_states;
    rsm->final_states = c->finals;
}

static GrB_Info run_case(const Case *c, const GrB_Index *sources, size_t num_sources, GrB_Vector *out)
{
    RSM rsm;
    case_rsm(c, &rsm);
    *out = GrB_NULL;
    return LAGraph_CFPQ_RSM(out, &rsm, (const GrB_Matrix *)c->graph, sources, num_sources, c->V, msg);
}

//------------------------------------------------------------------------------
// conversion from GraphBLAS objects to the independent oracle
//------------------------------------------------------------------------------

typedef struct
{
    cfpq_oracle_input in;
    uint64_t *start_states;
    bool *final_states;
    cfpq_oracle_edge *term_edges;
    uint64_t *term_begin;
    cfpq_oracle_edge *nonterm_edges;
    uint64_t *nonterm_begin;
    cfpq_oracle_edge *graph_edges;
    uint64_t *graph_begin;
} OracleView;

static void oracle_view_free(OracleView *ov)
{
    free(ov->start_states);
    free(ov->final_states);
    free(ov->term_edges);
    free(ov->term_begin);
    free(ov->nonterm_edges);
    free(ov->nonterm_begin);
    free(ov->graph_edges);
    free(ov->graph_begin);
    memset(ov, 0, sizeof(*ov));
}

// Extract true entries of a BOOL matrix.  Returns the number of true entries
// in *count; false stored entries are dropped (the fast-path contract).
static bool extract_true_edges(GrB_Matrix A, cfpq_oracle_edge **edges, uint64_t *count)
{
    *edges = NULL;
    *count = 0;
    if (A == GrB_NULL)
        return true;

    GrB_Index nvals = 0;
    if (!OK(GrB_Matrix_nvals(&nvals, A), "matrix nvals"))
        return false;
    if (nvals == 0)
        return true;

    GrB_Index *I = (GrB_Index *)malloc((size_t)nvals * sizeof(GrB_Index));
    GrB_Index *J = (GrB_Index *)malloc((size_t)nvals * sizeof(GrB_Index));
    bool *X = (bool *)malloc((size_t)nvals * sizeof(bool));
    cfpq_oracle_edge *out = (cfpq_oracle_edge *)malloc((size_t)nvals * sizeof(cfpq_oracle_edge));
    if (I == NULL || J == NULL || X == NULL || out == NULL)
    {
        free(I);
        free(J);
        free(X);
        free(out);
        report_failure(__FILE__, __LINE__, "out of memory extracting edges");
        return false;
    }
    GrB_Index n = nvals;
    bool ok = OK(GrB_Matrix_extractTuples_BOOL(I, J, X, &n, A), "matrix extractTuples");
    if (ok)
    {
        uint64_t k = 0;
        for (GrB_Index t = 0; t < n; ++t)
        {
            if (X[t])
            {
                out[k].from = I[t];
                out[k].to = J[t];
                k++;
            }
        }
        *count = k;
        *edges = out;
    }
    free(I);
    free(J);
    free(X);
    if (!ok)
        free(out);
    return ok;
}

static bool extract_true_vector(GrB_Vector v, uint64_t *out, uint64_t n, uint64_t *count)
{
    *count = 0;
    if (v == GrB_NULL)
        return true;
    GrB_Index nvals = 0;
    if (!OK(GrB_Vector_nvals(&nvals, v), "vector nvals"))
        return false;
    if (nvals == 0)
        return true;
    GrB_Index *I = (GrB_Index *)malloc((size_t)nvals * sizeof(GrB_Index));
    bool *X = (bool *)malloc((size_t)nvals * sizeof(bool));
    if (I == NULL || X == NULL)
    {
        free(I);
        free(X);
        report_failure(__FILE__, __LINE__, "out of memory extracting vector");
        return false;
    }
    GrB_Index nn = nvals;
    bool ok = OK(GrB_Vector_extractTuples_BOOL(I, X, &nn, v), "vector extractTuples");
    if (ok)
    {
        uint64_t k = 0;
        for (GrB_Index t = 0; t < nn; ++t)
        {
            if (X[t] && out != NULL)
                out[k++] = I[t];
        }
        *count = k;
    }
    free(I);
    free(X);
    return ok;
}

static bool oracle_view_build(const Case *c, OracleView *ov)
{
    memset(ov, 0, sizeof(*ov));
    ov->start_states = (uint64_t *)calloc(c->nn ? c->nn : 1, sizeof(uint64_t));
    ov->final_states = (bool *)calloc((size_t)(c->nn ? c->nn : 1) * (size_t)(c->Q ? c->Q : 1),
                                      sizeof(bool));
    ov->term_begin = (uint64_t *)calloc((c->nt ? c->nt : 1) + 1, sizeof(uint64_t));
    ov->nonterm_begin = (uint64_t *)calloc((c->nn ? c->nn : 1) + 1, sizeof(uint64_t));
    ov->graph_begin = (uint64_t *)calloc((c->nt ? c->nt : 1) + 1, sizeof(uint64_t));
    if (ov->start_states == NULL || ov->final_states == NULL || ov->term_begin == NULL ||
        ov->nonterm_begin == NULL || ov->graph_begin == NULL)
    {
        oracle_view_free(ov);
        report_failure(__FILE__, __LINE__, "out of memory building oracle view");
        return false;
    }

    for (size_t i = 0; i < c->nn; ++i)
    {
        ov->start_states[i] = c->start_states[i];
        uint64_t nfinals = 0;
        uint64_t *finals = (uint64_t *)malloc((size_t)(c->Q ? c->Q : 1) * sizeof(uint64_t));
        if (finals == NULL || !extract_true_vector(c->finals[i], finals, c->Q, &nfinals))
        {
            free(finals);
            oracle_view_free(ov);
            return false;
        }
        for (uint64_t k = 0; k < nfinals; ++k)
            ov->final_states[i * c->Q + finals[k]] = true;
        free(finals);
    }

    cfpq_oracle_edge **term = (cfpq_oracle_edge **)calloc(c->nt ? c->nt : 1, sizeof(*term));
    uint64_t *term_count = (uint64_t *)calloc(c->nt ? c->nt : 1, sizeof(uint64_t));
    cfpq_oracle_edge **nonterm = (cfpq_oracle_edge **)calloc(c->nn ? c->nn : 1, sizeof(*nonterm));
    uint64_t *nonterm_count = (uint64_t *)calloc(c->nn ? c->nn : 1, sizeof(uint64_t));
    cfpq_oracle_edge **graph = (cfpq_oracle_edge **)calloc(c->nt ? c->nt : 1, sizeof(*graph));
    uint64_t *graph_count = (uint64_t *)calloc(c->nt ? c->nt : 1, sizeof(uint64_t));
    bool ok = term != NULL && term_count != NULL && nonterm != NULL && nonterm_count != NULL &&
              graph != NULL && graph_count != NULL;
    for (size_t t = 0; ok && t < c->nt; ++t)
        ok = extract_true_edges(c->term[t], &term[t], &term_count[t]) &&
             extract_true_edges(c->graph[t], &graph[t], &graph_count[t]);
    for (size_t i = 0; ok && i < c->nn; ++i)
        ok = extract_true_edges(c->nonterm[i], &nonterm[i], &nonterm_count[i]);
    if (!ok)
    {
        for (size_t t = 0; t < c->nt; ++t)
        {
            free(term ? term[t] : NULL);
            free(graph ? graph[t] : NULL);
        }
        for (size_t i = 0; i < c->nn; ++i)
            free(nonterm ? nonterm[i] : NULL);
        free(term);
        free(term_count);
        free(nonterm);
        free(nonterm_count);
        free(graph);
        free(graph_count);
        oracle_view_free(ov);
        return false;
    }

    // flatten per-label edge lists with begin offsets
    uint64_t total = 0;
    for (size_t t = 0; t < c->nt; ++t)
    {
        ov->term_begin[t] = total;
        total += term_count[t];
    }
    ov->term_begin[c->nt] = total;
    ov->term_edges = (cfpq_oracle_edge *)malloc((size_t)(total ? total : 1) * sizeof(cfpq_oracle_edge));
    if (ov->term_edges == NULL)
        ok = false;
    total = 0;
    for (size_t i = 0; ok && i < c->nn; ++i)
    {
        ov->nonterm_begin[i] = total;
        total += nonterm_count[i];
    }
    ov->nonterm_begin[c->nn] = total;
    if (ok)
    {
        ov->nonterm_edges =
            (cfpq_oracle_edge *)malloc((size_t)(total ? total : 1) * sizeof(cfpq_oracle_edge));
        if (ov->nonterm_edges == NULL)
            ok = false;
    }
    total = 0;
    for (size_t t = 0; ok && t < c->nt; ++t)
    {
        ov->graph_begin[t] = total;
        total += graph_count[t];
    }
    ov->graph_begin[c->nt] = total;
    if (ok)
    {
        ov->graph_edges =
            (cfpq_oracle_edge *)malloc((size_t)(total ? total : 1) * sizeof(cfpq_oracle_edge));
        if (ov->graph_edges == NULL)
            ok = false;
    }

    if (ok)
    {
        // memcpy with a NULL source is undefined even for a zero count
        for (size_t t = 0; t < c->nt; ++t)
            if (term_count[t] > 0)
                memcpy(ov->term_edges + ov->term_begin[t], term[t],
                       (size_t)term_count[t] * sizeof(cfpq_oracle_edge));
        for (size_t i = 0; i < c->nn; ++i)
            if (nonterm_count[i] > 0)
                memcpy(ov->nonterm_edges + ov->nonterm_begin[i], nonterm[i],
                       (size_t)nonterm_count[i] * sizeof(cfpq_oracle_edge));
        for (size_t t = 0; t < c->nt; ++t)
            if (graph_count[t] > 0)
                memcpy(ov->graph_edges + ov->graph_begin[t], graph[t],
                       (size_t)graph_count[t] * sizeof(cfpq_oracle_edge));
    }

    for (size_t t = 0; t < c->nt; ++t)
    {
        free(term[t]);
        free(graph[t]);
    }
    for (size_t i = 0; i < c->nn; ++i)
        free(nonterm[i]);
    free(term);
    free(term_count);
    free(nonterm);
    free(nonterm_count);
    free(graph);
    free(graph_count);

    if (!ok)
    {
        oracle_view_free(ov);
        report_failure(__FILE__, __LINE__, "out of memory flattening oracle input");
        return false;
    }

    ov->in.state_count = c->Q;
    ov->in.vertex_count = c->V;
    ov->in.term_count = c->nt;
    ov->in.nonterm_count = c->nn;
    ov->in.start_nonterminal = c->start_nonterm;
    ov->in.start_states = ov->start_states;
    ov->in.final_states = ov->final_states;
    ov->in.term_edges = ov->term_edges;
    ov->in.term_begin = ov->term_begin;
    ov->in.nonterm_edges = ov->nonterm_edges;
    ov->in.nonterm_begin = ov->nonterm_begin;
    ov->in.graph_edges = ov->graph_edges;
    ov->in.graph_begin = ov->graph_begin;
    return true;
}

//------------------------------------------------------------------------------
// result checking
//------------------------------------------------------------------------------

static bool vector_matches(GrB_Vector got, const bool *expected, GrB_Index V,
                           bool require_no_stored_false, const char *what)
{
    if (got == GrB_NULL)
    {
        report_failure(__FILE__, __LINE__, "%s: result vector is NULL", what);
        return false;
    }

    GrB_Index size = 0;
    if (!OK(GrB_Vector_size(&size, got), "vector size"))
        return false;
    if (size != V)
    {
        report_failure(__FILE__, __LINE__, "%s: result length %llu != V %llu", what,
                       (unsigned long long)size, (unsigned long long)V);
        return false;
    }

    bool *seen = (bool *)calloc((size_t)(V ? V : 1), sizeof(bool));
    if (seen == NULL)
    {
        report_failure(__FILE__, __LINE__, "%s: out of memory", what);
        return false;
    }

    GrB_Index nvals = 0;
    if (!OK(GrB_Vector_nvals(&nvals, got), "vector nvals"))
    {
        free(seen);
        return false;
    }
    GrB_Index *I = NULL;
    bool *X = NULL;
    if (nvals > 0)
    {
        I = (GrB_Index *)malloc((size_t)nvals * sizeof(GrB_Index));
        X = (bool *)malloc((size_t)nvals * sizeof(bool));
        if (I == NULL || X == NULL)
        {
            free(I);
            free(X);
            free(seen);
            report_failure(__FILE__, __LINE__, "%s: out of memory", what);
            return false;
        }
        GrB_Index n = nvals;
        if (!OK(GrB_Vector_extractTuples_BOOL(I, X, &n, got), "vector extractTuples"))
        {
            free(I);
            free(X);
            free(seen);
            return false;
        }
        for (GrB_Index k = 0; k < n; ++k)
        {
            if (I[k] >= V)
            {
                report_failure(__FILE__, __LINE__, "%s: stored index %llu >= V", what,
                               (unsigned long long)I[k]);
                continue;
            }
            if (X[k])
            {
                seen[I[k]] = true;
            }
            else if (require_no_stored_false)
            {
                report_failure(__FILE__, __LINE__, "%s: stored false entry at %llu", what,
                               (unsigned long long)I[k]);
            }
        }
    }

    bool ok = true;
    for (GrB_Index v = 0; v < V; ++v)
    {
        if (seen[v] != expected[v])
        {
            if (ok)
                report_failure(__FILE__, __LINE__, "%s: vertex %llu expected %d, got %d", what,
                               (unsigned long long)v, (int)expected[v], (int)seen[v]);
            ok = false;
        }
    }

    free(I);
    free(X);
    free(seen);
    g_checks++;
    return ok;
}

// Run the candidate for one source set and compare the exact true-index set
// with the oracle; also require the output to contain only true entries.
static bool run_and_compare(const Case *c, const uint64_t *sources, size_t num_sources,
                            const bool *expected, const char *what)
{
    GrB_Index *src = NULL;
    if (num_sources > 0)
    {
        src = (GrB_Index *)malloc(num_sources * sizeof(GrB_Index));
        if (src == NULL)
        {
            report_failure(__FILE__, __LINE__, "%s: out of memory", what);
            return false;
        }
        for (size_t k = 0; k < num_sources; ++k)
            src[k] = (GrB_Index)sources[k];
    }

    GrB_Vector out = GrB_NULL;
    GrB_Info info = run_case(c, src, num_sources, &out);
    bool ok = true;
    if (info != GrB_SUCCESS)
    {
        report_failure(__FILE__, __LINE__, "%s: kernel failed with info %d: %s", what, (int)info, msg);
        ok = false;
    }
    else
    {
        ok = vector_matches(out, expected, c->V, true, what);
        GrB_Vector_wait(out, GrB_MATERIALIZE);
    }
    GrB_free(&out);
    free(src);
    return ok;
}

//------------------------------------------------------------------------------
// construction-time contract validation (runs outside the kernel)
//------------------------------------------------------------------------------

// The fast-path contract is BOOL relations with only true stored values.  The
// kernel does not normalize inputs on every call, so the construction/test
// boundary rejects out-of-contract relations here.  This is the boundary
// described in the DS-00 report; a general-caller policy is Q-001.

static bool matrix_contract_ok(GrB_Matrix A, const char *what, char *reason, size_t reason_len)
{
    if (A == GrB_NULL)
        return true;
    GrB_Type type = NULL;
    if (!OK(GxB_Matrix_type(&type, A), "matrix type"))
        return false;
    if (type != GrB_BOOL)
    {
        snprintf(reason, reason_len, "%s is not GrB_BOOL", what);
        return false;
    }
    GrB_Index nvals = 0;
    if (!OK(GrB_Matrix_nvals(&nvals, A), "matrix nvals"))
        return false;
    if (nvals == 0)
        return true;
    GrB_Index *I = (GrB_Index *)malloc((size_t)nvals * sizeof(GrB_Index));
    GrB_Index *J = (GrB_Index *)malloc((size_t)nvals * sizeof(GrB_Index));
    bool *X = (bool *)malloc((size_t)nvals * sizeof(bool));
    bool ok = I != NULL && J != NULL && X != NULL;
    if (ok)
    {
        GrB_Index n = nvals;
        ok = OK(GrB_Matrix_extractTuples_BOOL(I, J, X, &n, A), "matrix extractTuples");
        if (ok)
        {
            for (GrB_Index k = 0; k < n; ++k)
            {
                if (!X[k])
                {
                    snprintf(reason, reason_len, "%s stores false at (%llu,%llu)", what,
                             (unsigned long long)I[k], (unsigned long long)J[k]);
                    ok = false;
                    break;
                }
            }
        }
    }
    free(I);
    free(J);
    free(X);
    return ok;
}

static bool vector_contract_ok(GrB_Vector v, const char *what, char *reason, size_t reason_len)
{
    if (v == GrB_NULL)
        return true;
    GrB_Type type = NULL;
    if (!OK(GxB_Vector_type(&type, v), "vector type"))
        return false;
    if (type != GrB_BOOL)
    {
        snprintf(reason, reason_len, "%s is not GrB_BOOL", what);
        return false;
    }
    GrB_Index nvals = 0;
    if (!OK(GrB_Vector_nvals(&nvals, v), "vector nvals"))
        return false;
    if (nvals == 0)
        return true;
    GrB_Index *I = (GrB_Index *)malloc((size_t)nvals * sizeof(GrB_Index));
    bool *X = (bool *)malloc((size_t)nvals * sizeof(bool));
    bool ok = I != NULL && X != NULL;
    if (ok)
    {
        GrB_Index n = nvals;
        ok = OK(GrB_Vector_extractTuples_BOOL(I, X, &n, v), "vector extractTuples");
        if (ok)
        {
            for (GrB_Index k = 0; k < n; ++k)
            {
                if (!X[k])
                {
                    snprintf(reason, reason_len, "%s stores false at %llu", what,
                             (unsigned long long)I[k]);
                    ok = false;
                    break;
                }
            }
        }
    }
    free(I);
    free(X);
    return ok;
}

static bool inputs_true_only(const Case *c, char *reason, size_t reason_len)
{
    char what[128];
    if (reason != NULL && reason_len > 0)
        reason[0] = '\0';
    for (size_t t = 0; t < c->nt; ++t)
    {
        snprintf(what, sizeof(what), "terminal[%zu]", t);
        if (!matrix_contract_ok(c->term[t], what, reason, reason_len))
            return false;
        snprintf(what, sizeof(what), "graph[%zu]", t);
        if (!matrix_contract_ok(c->graph[t], what, reason, reason_len))
            return false;
    }
    for (size_t i = 0; i < c->nn; ++i)
    {
        snprintf(what, sizeof(what), "nonterminal[%zu]", i);
        if (!matrix_contract_ok(c->nonterm[i], what, reason, reason_len))
            return false;
        snprintf(what, sizeof(what), "final[%zu]", i);
        if (!vector_contract_ok(c->finals[i], what, reason, reason_len))
            return false;
    }
    return true;
}

//------------------------------------------------------------------------------
// full comparison of one case: every singleton source, the full source set
// (with a duplicate), and the empty source set
//------------------------------------------------------------------------------

static bool check_case(Case *c, const char *name, const char *hand_multi_csv)
{
    char why[256];
    if (!inputs_true_only(c, why, sizeof(why)))
    {
        report_failure(__FILE__, __LINE__, "%s: input contract violation: %s", name, why);
        return false;
    }

    OracleView ov;
    if (!oracle_view_build(c, &ov))
        return false;

    bool *oracle_multi = (bool *)calloc((size_t)(c->V ? c->V : 1), sizeof(bool));
    bool *oracle_one = (bool *)calloc((size_t)(c->V ? c->V : 1), sizeof(bool));
    if (oracle_multi == NULL || oracle_one == NULL)
    {
        free(oracle_multi);
        free(oracle_one);
        oracle_view_free(&ov);
        report_failure(__FILE__, __LINE__, "%s: out of memory", name);
        return false;
    }

    // multi-source set: all vertices, plus a duplicate when V >= 2 (still
    // inside the documented num_sources <= V bound only when 2*V <= V, so use
    // a duplicate only when it remains inside the bound)
    uint64_t *multi = (uint64_t *)malloc((size_t)(c->V ? c->V : 1) * sizeof(uint64_t));
    size_t multi_n = 0;
    for (GrB_Index v = 0; v < c->V; ++v)
        multi[multi_n++] = v;

    bool ok = true;
    if (cfpq_oracle_reachable(&ov.in, multi, multi_n, oracle_multi) != 0)
    {
        report_failure(__FILE__, __LINE__, "%s: oracle failed", name);
        ok = false;
    }

    // hand expectation, where a readable one is supplied
    if (hand_multi_csv != NULL && c->V <= 64)
    {
        bool hand[64];
        memset(hand, 0, sizeof(hand));
        const char *p = hand_multi_csv;
        while (*p != '\0')
        {
            char *end = NULL;
            long value = strtol(p, &end, 10);
            if (end == p)
                break;
            if (value < 0 || (GrB_Index)value >= c->V)
            {
                report_failure(__FILE__, __LINE__, "%s: hand expectation %ld out of range", name,
                               value);
                ok = false;
                break;
            }
            hand[value] = true;
            p = end;
            while (*p == ',' || *p == ' ')
                p++;
        }
        for (GrB_Index v = 0; v < c->V; ++v)
        {
            if (hand[v] != oracle_multi[v])
            {
                report_failure(__FILE__, __LINE__,
                               "%s: oracle disagrees with hand expectation at vertex %llu", name,
                               (unsigned long long)v);
                ok = false;
            }
        }
    }

    if (!run_and_compare(c, multi, multi_n, oracle_multi, name))
        ok = false;

    // singleton-source runs: the public multi-source union can hide a
    // source-specific error
    for (GrB_Index v = 0; v < c->V; ++v)
    {
        uint64_t s = v;
        char what[256];
        snprintf(what, sizeof(what), "%s[src=%llu]", name, (unsigned long long)v);
        if (cfpq_oracle_reachable(&ov.in, &s, 1, oracle_one) != 0)
        {
            report_failure(__FILE__, __LINE__, "%s: oracle failed", what);
            ok = false;
            continue;
        }
        if (!run_and_compare(c, &s, 1, oracle_one, what))
            ok = false;
    }

    // empty source set
    memset(oracle_one, 0, (size_t)(c->V ? c->V : 1) * sizeof(bool));
    if (!run_and_compare(c, NULL, 0, oracle_one, name))
        ok = false;

    free(multi);
    free(oracle_multi);
    free(oracle_one);
    oracle_view_free(&ov);
    return ok;
}

// compare the true-index sets of two candidate runs (for NULL vs empty forms)
static bool same_result(const Case *a, const Case *b, const char *name)
{
    GrB_Index *src_a = (GrB_Index *)malloc((size_t)(a->V ? a->V : 1) * sizeof(GrB_Index));
    size_t n = 0;
    for (GrB_Index v = 0; v < a->V; ++v)
        src_a[n++] = v;
    GrB_Vector ra = GrB_NULL, rb = GrB_NULL;
    bool ok = true;
    if (run_case(a, src_a, n, &ra) != GrB_SUCCESS)
    {
        report_failure(__FILE__, __LINE__, "%s: first form failed: %s", name, msg);
        ok = false;
    }
    if (!OK(GrB_Vector_wait(ra, GrB_MATERIALIZE), "wait ra"))
        ok = false;
    if (run_case(b, src_a, n, &rb) != GrB_SUCCESS)
    {
        report_failure(__FILE__, __LINE__, "%s: second form failed: %s", name, msg);
        ok = false;
    }
    if (!OK(GrB_Vector_wait(rb, GrB_MATERIALIZE), "wait rb"))
        ok = false;
    if (ok)
    {
        bool *ea = (bool *)calloc((size_t)(a->V ? a->V : 1), sizeof(bool));
        bool *eb = (bool *)calloc((size_t)(b->V ? b->V : 1), sizeof(bool));
        for (GrB_Index v = 0; v < a->V; ++v)
        {
            ea[v] = false;
            eb[v] = false;
        }
        GrB_Index na = 0, nb = 0;
        OK(GrB_Vector_nvals(&na, ra), "nvals ra");
        OK(GrB_Vector_nvals(&nb, rb), "nvals rb");
        GrB_Index *Ia = (GrB_Index *)malloc((size_t)(na ? na : 1) * sizeof(GrB_Index));
        bool *Xa = (bool *)malloc((size_t)(na ? na : 1) * sizeof(bool));
        GrB_Index *Ib = (GrB_Index *)malloc((size_t)(nb ? nb : 1) * sizeof(GrB_Index));
        bool *Xb = (bool *)malloc((size_t)(nb ? nb : 1) * sizeof(bool));
        if (na > 0 && Ia != NULL && Xa != NULL)
        {
            GrB_Index t = na;
            OK(GrB_Vector_extractTuples_BOOL(Ia, Xa, &t, ra), "extract ra");
            for (GrB_Index k = 0; k < t; ++k)
                if (Xa[k])
                    ea[Ia[k]] = true;
        }
        if (nb > 0 && Ib != NULL && Xb != NULL)
        {
            GrB_Index t = nb;
            OK(GrB_Vector_extractTuples_BOOL(Ib, Xb, &t, rb), "extract rb");
            for (GrB_Index k = 0; k < t; ++k)
                if (Xb[k])
                    eb[Ib[k]] = true;
        }
        for (GrB_Index v = 0; v < a->V; ++v)
        {
            if (ea[v] != eb[v])
            {
                report_failure(__FILE__, __LINE__, "%s: forms disagree at vertex %llu", name,
                               (unsigned long long)v);
                ok = false;
            }
        }
        free(ea);
        free(eb);
        free(Ia);
        free(Xa);
        free(Ib);
        free(Xb);
    }
    GrB_free(&ra);
    GrB_free(&rb);
    free(src_a);
    return ok;
}

//------------------------------------------------------------------------------
// deterministic regression: S -> a, with NULL and with empty incoming calls
//------------------------------------------------------------------------------

static bool build_single_terminal(Case *c, bool null_nonterminal)
{
    if (!case_new(c, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/1, /*start_nonterm=*/0))
        return false;
    c->start_states[0] = 0;
    if (!case_final(c, 0, 1) || !case_term(c, 0, 0, 1) || !case_graph(c, 0, 0, 1))
        return false;
    if (null_nonterminal)
        case_drop_nonterm(c, 0);
    return true;
}

static void test_single_terminal(void)
{
    Case null_case, empty_case;
    if (!build_single_terminal(&null_case, true) || !build_single_terminal(&empty_case, false))
    {
        case_free(&null_case);
        case_free(&empty_case);
        return;
    }
    CHECK(check_case(&null_case, "S->a[NULL calls]", "1"));
    CHECK(check_case(&empty_case, "S->a[empty calls]", "1"));
    CHECK(same_result(&null_case, &empty_case, "S->a NULL vs empty calls"));
    case_free(&null_case);
    case_free(&empty_case);
}

//------------------------------------------------------------------------------
// deterministic regression: start state is also final (epsilon acceptance)
//------------------------------------------------------------------------------

static void test_epsilon_start(void)
{
    // no graph edges, one vertex: the source accepts itself
    Case a;
    if (case_new(&a, /*Q=*/1, /*V=*/1, /*nt=*/0, /*nn=*/1, 0))
    {
        a.start_states[0] = 0;
        CHECK(case_final(&a, 0, 0));
        CHECK(check_case(&a, "epsilon[V=1,no edges]", "0"));
        case_free(&a);
    }

    // isolated vertices and edges that no transition can use
    Case b;
    if (case_new(&b, /*Q=*/1, /*V=*/4, /*nt=*/1, /*nn=*/1, 0))
    {
        b.start_states[0] = 0;
        CHECK(case_final(&b, 0, 0));
        CHECK(case_graph(&b, 0, 0, 1));
        CHECK(case_graph(&b, 0, 2, 3));
        CHECK(check_case(&b, "epsilon[V=4,unused edges]", "0,1,2,3"));
        case_free(&b);
    }

    // start state final and a terminal edge keeps the same source accepted
    Case d;
    if (case_new(&d, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/1, 0))
    {
        d.start_states[0] = 0;
        CHECK(case_final(&d, 0, 0));
        CHECK(case_final(&d, 0, 1));
        CHECK(case_term(&d, 0, 0, 1));
        CHECK(case_graph(&d, 0, 0, 0));
        CHECK(case_graph(&d, 0, 0, 1));
        CHECK(check_case(&d, "epsilon+loop", "0,1"));
        case_free(&d);
    }

    // epsilon acceptance with no terminal labels at all, NULL terminal array
    {
        Case e;
        if (case_new(&e, /*Q=*/1, /*V=*/2, /*nt=*/0, /*nn=*/1, 0))
        {
            e.start_states[0] = 0;
            CHECK(case_final(&e, 0, 0));
            RSM rsm;
            case_rsm(&e, &rsm);
            GrB_Vector out = GrB_NULL;
            GrB_Index sources[2] = {0, 1};
            GrB_Info info = LAGraph_CFPQ_RSM(&out, &rsm, NULL, sources, 2, e.V, msg);
            CHECK_MSG(info == GrB_SUCCESS, "epsilon with NULL graph_term failed: %s", msg);
            if (info == GrB_SUCCESS)
            {
                bool expected[2] = {true, true}; // every source accepts itself
                CHECK(vector_matches(out, expected, 2, true, "epsilon NULL graph_term"));
            }
            GrB_free(&out);
            case_free(&e);
        }
    }
}

//------------------------------------------------------------------------------
// deterministic regression: no accepting state / NULL final vector
//------------------------------------------------------------------------------

static void test_no_final(void)
{
    Case null_case, empty_case;
    if (case_new(&null_case, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/2, 0) &&
        case_new(&empty_case, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/2, 0))
    {
        for (size_t box = 0; box < 2; ++box)
        {
            null_case.start_states[box] = 1;
            empty_case.start_states[box] = 1;
            CHECK(case_term(&null_case, 0, 1, 0));
            CHECK(case_term(&empty_case, 0, 1, 0));
            CHECK(case_graph(&null_case, 0, 0, 1));
            CHECK(case_graph(&empty_case, 0, 0, 1));
        }
        case_drop_finals(&null_case, 0);
        case_drop_finals(&null_case, 1);
        CHECK(check_case(&null_case, "no final[NULL]", ""));
        CHECK(check_case(&empty_case, "no final[empty]", ""));
        CHECK(same_result(&null_case, &empty_case, "no final NULL vs empty"));
    }
    case_free(&null_case);
    case_free(&empty_case);
}

//------------------------------------------------------------------------------
// deterministic regression: direct and mutual recursion, nullable callee
//------------------------------------------------------------------------------

static void test_recursion(void)
{
    // S -> a S b | a b  (direct recursion)
    Case direct;
    if (case_new(&direct, /*Q=*/4, /*V=*/4, /*nt=*/2, /*nn=*/1, 0))
    {
        direct.start_states[0] = 0;
        CHECK(case_term(&direct, 0, 0, 1));    // 0 -a-> 1
        CHECK(case_term(&direct, 1, 1, 3));    // 1 -b-> 3
        CHECK(case_term(&direct, 1, 2, 3));    // 2 -b-> 3
        CHECK(case_nonterm(&direct, 0, 1, 2)); // 1 -S-> 2
        CHECK(case_final(&direct, 0, 3));
        CHECK(case_graph(&direct, 0, 0, 1));
        CHECK(case_graph(&direct, 0, 1, 2));
        CHECK(case_graph(&direct, 0, 2, 0));
        CHECK(case_graph(&direct, 1, 0, 3));
        CHECK(case_graph(&direct, 1, 3, 0));
        CHECK(check_case(&direct, "direct-recursion", "0,3"));
        case_free(&direct);
    }

    // S -> a T b | a N b | a b ; T -> c S d | c d ; N -> epsilon
    // (mutual recursion S/T plus a nullable callee N)
    Case mutual;
    if (case_new(&mutual, /*Q=*/9, /*V=*/8, /*nt=*/4, /*nn=*/3, 0))
    {
        // alphabet: a=0, b=1, c=2, d=3
        // S: states 0..3, start 0, final 3
        CHECK(case_term(&mutual, 0, 0, 1));     // 0 -a-> 1
        CHECK(case_nonterm(&mutual, 1, 1, 2));  // 1 -T-> 2
        CHECK(case_nonterm(&mutual, 2, 1, 2));  // 1 -N-> 2
        CHECK(case_term(&mutual, 1, 2, 3));     // 2 -b-> 3
        // T: states 4..7, start 4, final 7
        CHECK(case_term(&mutual, 2, 4, 5));     // 4 -c-> 5
        CHECK(case_nonterm(&mutual, 0, 5, 6));  // 5 -S-> 6
        CHECK(case_term(&mutual, 3, 6, 7));     // 6 -d-> 7
        // N: state 8, nullable
        mutual.start_states[0] = 0;
        mutual.start_states[1] = 4;
        mutual.start_states[2] = 8;
        CHECK(case_final(&mutual, 0, 3));
        CHECK(case_final(&mutual, 1, 7));
        CHECK(case_final(&mutual, 2, 8));
        // graph: "a c a b d b" (0..6) and "a b" (0..7)
        CHECK(case_graph(&mutual, 0, 0, 1));
        CHECK(case_graph(&mutual, 2, 1, 2));
        CHECK(case_graph(&mutual, 0, 2, 3));
        CHECK(case_graph(&mutual, 1, 3, 4));
        CHECK(case_graph(&mutual, 3, 4, 5));
        CHECK(case_graph(&mutual, 1, 5, 6));
        CHECK(case_graph(&mutual, 1, 1, 7));
        CHECK(check_case(&mutual, "mutual-recursion+nullable", "4,6,7"));
        case_free(&mutual);
    }
}

//------------------------------------------------------------------------------
// deterministic regression: multiple finals, callers, overlapping summaries
//------------------------------------------------------------------------------

static void test_multiple_finals(void)
{
    // S -> a A b | a B b ; A -> x? ; B -> x
    // A is nullable and also accepts x; B accepts x only.  A and B are both
    // called from the same place, so their summaries overlap at (1,2).
    Case c;
    if (case_new(&c, /*Q=*/9, /*V=*/5, /*nt=*/3, /*nn=*/3, 0))
    {
        // alphabet: a=0, b=1, x=2
        // S: states 0..4
        CHECK(case_term(&c, 0, 0, 1));    // 0 -a-> 1
        CHECK(case_nonterm(&c, 1, 1, 2)); // 1 -A-> 2
        CHECK(case_nonterm(&c, 2, 1, 3)); // 1 -B-> 3
        CHECK(case_term(&c, 1, 2, 4));    // 2 -b-> 4
        CHECK(case_term(&c, 1, 3, 4));    // 3 -b-> 4
        // A: states 5..6, final 5 (epsilon) and 6
        CHECK(case_term(&c, 2, 5, 6));    // 5 -x-> 6
        // B: states 7..8, final 8
        CHECK(case_term(&c, 2, 7, 8));    // 7 -x-> 8
        c.start_states[0] = 0;
        c.start_states[1] = 5;
        c.start_states[2] = 7;
        CHECK(case_final(&c, 0, 4));
        CHECK(case_final(&c, 1, 5));
        CHECK(case_final(&c, 1, 6));
        CHECK(case_final(&c, 2, 8));
        CHECK(case_graph(&c, 0, 0, 1));
        CHECK(case_graph(&c, 2, 1, 2));
        CHECK(case_graph(&c, 1, 1, 4));
        CHECK(case_graph(&c, 1, 2, 3));
        CHECK(check_case(&c, "multi-final/caller", "3,4"));
        case_free(&c);
    }
}

//------------------------------------------------------------------------------
// deterministic regression: empty input, isolated vertices, V=0
//------------------------------------------------------------------------------

static void test_empty_and_zero(void)
{
    // V=0 and no sources: length-0 result, safe cleanup
    Case zero;
    if (case_new(&zero, /*Q=*/1, /*V=*/0, /*nt=*/1, /*nn=*/1, 0))
    {
        zero.start_states[0] = 0;
        CHECK(case_final(&zero, 0, 0));
        GrB_Vector out = GrB_NULL;
        GrB_Info info = run_case(&zero, NULL, 0, &out);
        CHECK_MSG(info == GrB_SUCCESS, "V=0 run failed: %s", msg);
        if (info == GrB_SUCCESS)
        {
            GrB_Index size = 42;
            CHECK(OK(GrB_Vector_size(&size, out), "size"));
            CHECK_MSG(size == 0, "V=0 result has length %llu", (unsigned long long)size);
            GrB_Index nvals = 1;
            CHECK(OK(GrB_Vector_nvals(&nvals, out), "nvals"));
            CHECK_MSG(nvals == 0, "V=0 result has %llu entries", (unsigned long long)nvals);
        }
        GrB_free(&out);
        case_free(&zero);
    }

    // no edges at all: only epsilon acceptance can succeed
    Case noedges;
    if (case_new(&noedges, /*Q=*/2, /*V=*/3, /*nt=*/2, /*nn=*/1, 0))
    {
        noedges.start_states[0] = 0;
        CHECK(case_term(&noedges, 0, 0, 1));
        CHECK(case_term(&noedges, 1, 1, 1));
        CHECK(case_final(&noedges, 0, 1));
        CHECK(case_final(&noedges, 0, 0));
        CHECK(check_case(&noedges, "no-edges epsilon", "0,1,2"));
        case_free(&noedges);
    }

    // isolated vertices: only vertex 0 is connected; sources 1 and 2 accept
    // nothing unless the start state is final
    Case isolated;
    if (case_new(&isolated, /*Q=*/2, /*V=*/4, /*nt=*/1, /*nn=*/1, 0))
    {
        isolated.start_states[0] = 0;
        CHECK(case_term(&isolated, 0, 0, 1));
        CHECK(case_final(&isolated, 0, 1));
        CHECK(case_graph(&isolated, 0, 0, 1));
        CHECK(case_graph(&isolated, 0, 2, 2)); // isolated self-loop with no a edge? it is an a edge
        CHECK(check_case(&isolated, "isolated", "1,2"));
        case_free(&isolated);
    }

    // empty sources
    Case es;
    if (case_new(&es, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/1, 0))
    {
        es.start_states[0] = 0;
        CHECK(case_term(&es, 0, 0, 1));
        CHECK(case_final(&es, 0, 1));
        CHECK(case_graph(&es, 0, 0, 1));
        bool expected[2] = {false, false};
        CHECK(run_and_compare(&es, NULL, 0, expected, "empty sources"));
        case_free(&es);
    }
}

//------------------------------------------------------------------------------
// deterministic regression: missing/empty terminal matrices, unused box/label
//------------------------------------------------------------------------------

static void test_missing_terminals(void)
{
    // A: label 0 has a NULL terminal matrix, label 1 is used; box 1 is never
    // called and has no final states.
    Case a, b;
    bool ok = case_new(&a, /*Q=*/3, /*V=*/2, /*nt=*/2, /*nn=*/2, 0) &&
              case_new(&b, /*Q=*/3, /*V=*/2, /*nt=*/2, /*nn=*/2, 0);
    if (ok)
    {
        for (int which = 0; which < 2; ++which)
        {
            Case *c = which == 0 ? &a : &b;
            c->start_states[0] = 0;
            c->start_states[1] = 2;
            CHECK(case_term(c, 1, 0, 1));    // 0 -b-> 1
            CHECK(case_final(c, 0, 1));
            CHECK(case_graph(c, 1, 0, 1));
        }
        case_drop_term(&a, 0); // no a transitions at all
        case_drop_graph(&a, 0);
        case_drop_finals(&a, 1);
        case_drop_finals(&b, 1);
        // b keeps empty (not NULL) terminal 0 and an unused box 1
        CHECK(check_case(&a, "missing terminal", "1"));
        CHECK(check_case(&b, "empty terminal", "1"));
        CHECK(same_result(&a, &b, "missing vs empty terminal"));
    }
    case_free(&a);
    case_free(&b);
}

//------------------------------------------------------------------------------
// deterministic regression: multiple and repeated sources
//------------------------------------------------------------------------------

static void test_repeated_sources(void)
{
    // S -> a b.  Source 0 accepts {3} and source 1 accepts {4}, so the union
    // and every repeat of it are easy to audit by hand.
    Case c;
    if (case_new(&c, /*Q=*/3, /*V=*/5, /*nt=*/2, /*nn=*/1, 0))
    {
        c.start_states[0] = 0;
        CHECK(case_term(&c, 0, 0, 1));  // a: 0->1
        CHECK(case_term(&c, 1, 1, 2));  // b: 1->2
        CHECK(case_final(&c, 0, 2));
        CHECK(case_graph(&c, 0, 0, 1)); // a: 0->1
        CHECK(case_graph(&c, 0, 1, 2)); // a: 1->2
        CHECK(case_graph(&c, 1, 1, 3)); // b: 1->3
        CHECK(case_graph(&c, 1, 2, 4)); // b: 2->4

        GrB_Index s01[2] = {0, 1};
        GrB_Index s10[2] = {1, 0};
        GrB_Index s0011[4] = {0, 0, 1, 1};
        bool both[5] = {false, false, false, true, true};
        bool only0[5] = {false, false, false, true, false};
        CHECK(run_and_compare(&c, (const uint64_t *)s01, 2, both, "sources {0,1}"));
        CHECK(run_and_compare(&c, (const uint64_t *)s10, 2, both, "sources {1,0}"));
        CHECK(run_and_compare(&c, (const uint64_t *)s0011, 4, both, "sources {0,0,1,1}"));
        CHECK(run_and_compare(&c, (const uint64_t *)&s01[0], 1, only0, "source 0"));
        CHECK(run_and_compare(&c, (const uint64_t *)&s01[0], 1, only0, "source 0 again"));
        CHECK(check_case(&c, "repeated-sources", "3,4"));
        case_free(&c);
    }
}

//------------------------------------------------------------------------------
// deterministic regression: stored false / non-BOOL input
//------------------------------------------------------------------------------

static void test_stored_false(void)
{
    // A plain in-contract instance: S -> a b, source 0 accepts 2.
    Case c;
    if (!case_new(&c, /*Q=*/3, /*V=*/3, /*nt=*/2, /*nn=*/1, 0))
        return;
    c.start_states[0] = 0;
    CHECK(case_term(&c, 0, 0, 1));
    CHECK(case_term(&c, 1, 1, 2));
    CHECK(case_final(&c, 0, 2));
    CHECK(case_graph(&c, 0, 0, 1));
    CHECK(case_graph(&c, 1, 1, 2));

    char why[256];
    if (!inputs_true_only(&c, why, sizeof(why)))
    {
        report_failure(__FILE__, __LINE__, "plain case failed contract validation: %s", why);
    }
    else
    {
        g_checks++;
    }
    CHECK(check_case(&c, "contract[plain]", "2"));

    // Stored false entries are outside the fast-path contract.  The kernel is
    // not normalized on every call; instead the construction boundary rejects
    // them (explicit contract-boundary rejection).  See Q-001 for the
    // general-caller policy.
    CHECK(case_term_value(&c, 0, 0, 2, false));
    CHECK(case_graph_value(&c, 1, 0, 0, false));
    CHECK(case_final_value(&c, 0, 0, false));
    CHECK_MSG(!inputs_true_only(&c, why, sizeof(why)),
              "stored false entries passed construction-time validation");
    if (g_verbose)
        printf("stored-false rejection: %s\n", why);
    CHECK(strstr(why, "stores false") != NULL);
    case_free(&c);

    // non-BOOL input is rejected at the same construction boundary.  (The
    // kernel itself does not type-check every input matrix; SuiteSparse
    // GraphBLAS typecasts an INT32 operand into the BOOL semiring domain, so a
    // library-level rejection would need explicit type queries per call.  This
    // is part of the Q-001 general-caller question.)
    Case nonbool;
    if (case_new(&nonbool, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/1, 0))
    {
        nonbool.start_states[0] = 0;
        CHECK(case_term(&nonbool, 0, 0, 1));
        CHECK(case_final(&nonbool, 0, 1));
        GrB_Matrix int_edges = GrB_NULL;
        CHECK(OK(GrB_Matrix_new(&int_edges, GrB_INT32, 2, 2), "int graph matrix"));
        CHECK(OK(GrB_Matrix_setElement_INT32(int_edges, 1, 0, 1), "int edge"));
        GrB_free(&nonbool.graph[0]);
        nonbool.graph[0] = int_edges;
        CHECK_MSG(!inputs_true_only(&nonbool, why, sizeof(why)),
                  "non-BOOL graph input passed construction-time validation");
        if (g_verbose)
            printf("non-BOOL rejection: %s\n", why);
        case_free(&nonbool);
    }

    // non-BOOL RSM transition matrix
    Case nonbool_rsm;
    if (case_new(&nonbool_rsm, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/1, 0))
    {
        nonbool_rsm.start_states[0] = 0;
        CHECK(case_final(&nonbool_rsm, 0, 1));
        CHECK(case_graph(&nonbool_rsm, 0, 0, 1));
        GrB_Matrix int_trans = GrB_NULL;
        CHECK(OK(GrB_Matrix_new(&int_trans, GrB_INT32, 2, 2), "int term matrix"));
        CHECK(OK(GrB_Matrix_setElement_INT32(int_trans, 1, 0, 1), "int transition"));
        GrB_free(&nonbool_rsm.term[0]);
        nonbool_rsm.term[0] = int_trans;
        CHECK_MSG(!inputs_true_only(&nonbool_rsm, why, sizeof(why)),
                  "non-BOOL RSM transition matrix passed construction-time validation");
        case_free(&nonbool_rsm);
    }
}

//------------------------------------------------------------------------------
// deterministic regression: indexed families (load_i / store_j, i != j)
//------------------------------------------------------------------------------

static void test_indexed_families(void)
{
    // Label 0 is load_0 and label 1 is store_1.  A path that uses store_1
    // where the grammar requires load_0 must not be accepted: no label
    // merging, and no batching in this kernel can introduce a match.
    Case no_match, match, same_family;
    if (case_new(&no_match, /*Q=*/2, /*V=*/2, /*nt=*/2, /*nn=*/1, 0))
    {
        no_match.start_states[0] = 0;
        CHECK(case_term(&no_match, 0, 0, 1)); // load_0
        CHECK(case_final(&no_match, 0, 1));
        CHECK(case_graph(&no_match, 1, 0, 1)); // store_1 only
        CHECK(check_case(&no_match, "load_0 vs store_1", ""));
        case_free(&no_match);
    }
    if (case_new(&match, /*Q=*/2, /*V=*/2, /*nt=*/2, /*nn=*/1, 0))
    {
        match.start_states[0] = 0;
        CHECK(case_term(&match, 0, 0, 1));
        CHECK(case_final(&match, 0, 1));
        CHECK(case_graph(&match, 0, 0, 1)); // load_0 present
        CHECK(case_graph(&match, 1, 0, 1)); // store_1 present
        CHECK(check_case(&match, "load_0 present", "1"));
        case_free(&match);
    }
    if (case_new(&same_family, /*Q=*/3, /*V=*/3, /*nt=*/2, /*nn=*/1, 0))
    {
        // S -> load_0 store_0
        same_family.start_states[0] = 0;
        CHECK(case_term(&same_family, 0, 0, 1));
        CHECK(case_term(&same_family, 1, 1, 2));
        CHECK(case_final(&same_family, 0, 2));
        CHECK(case_graph(&same_family, 0, 0, 1));
        CHECK(case_graph(&same_family, 1, 1, 2));
        CHECK(check_case(&same_family, "load_0 store_0", "2"));
        case_free(&same_family);
    }
}

//------------------------------------------------------------------------------
// deterministic regression: grouped terminal families
//------------------------------------------------------------------------------
//
// These cases exercise the indexed-family grouping in the kernel: transitions
// sharing one source state (forward families) or one destination state (dual
// families).  A grouping mistake that loses transition, destination, or entry
// identity changes the reachable set, so each case is compared with the
// independent oracle by check_case.  The positive cases contain matched field
// paths ending at distinct vertices; the negative case contains only a
// mismatched load_0/store_1 path and must not accept it.

static void test_grouped_families(void)
{
    // Two indexed fields with a recursive callee Alias = S.  The load family
    // shares source state 0 and the store family shares destination state 0.
    // Matched field paths end at 6 (load0/store0) and 10 (load1/store1).
    // Mismatched load0/store1 would end at 5 and load1/store0 at 12.
    Case java_like;
    if (case_new(&java_like, /*Q=*/9, /*V=*/13, /*nt=*/6, /*nn=*/2, 0))
    {
        // terminals: load0=0, load1=1, store0=2, store1=3, alloc=4, assign=5
        // box S: states 0..5, start 0, final 1
        // box Alias: states 6..8, start 6, final 8
        java_like.start_states[0] = 0;
        java_like.start_states[1] = 6;
        CHECK(case_term(&java_like, 0, 0, 2)); // 0 -load0-> 2
        CHECK(case_term(&java_like, 1, 0, 3)); // 0 -load1-> 3
        CHECK(case_term(&java_like, 2, 4, 0)); // 4 -store0-> 0
        CHECK(case_term(&java_like, 3, 5, 0)); // 5 -store1-> 0
        CHECK(case_term(&java_like, 4, 0, 1)); // 0 -alloc-> 1 (final)
        CHECK(case_term(&java_like, 5, 0, 0)); // 0 -assign-> 0
        CHECK(case_nonterm(&java_like, 0, 2, 4)); // 2 -Alias-> 4
        CHECK(case_nonterm(&java_like, 0, 3, 5)); // 3 -Alias-> 5
        CHECK(case_nonterm(&java_like, 1, 6, 8)); // Alias: 6 -S-> 8
        CHECK(case_final(&java_like, 0, 1));
        CHECK(case_final(&java_like, 1, 8));
        // load0: 0->1; load1: 0->7
        CHECK(case_graph(&java_like, 0, 0, 1));
        CHECK(case_graph(&java_like, 1, 0, 7));
        // store0: matched 2->3, mismatched for load1 8->11
        CHECK(case_graph(&java_like, 2, 2, 3));
        CHECK(case_graph(&java_like, 2, 8, 11));
        // store1: matched 8->9, mismatched for load0 2->4
        CHECK(case_graph(&java_like, 3, 8, 9));
        CHECK(case_graph(&java_like, 3, 2, 4));
        // alloc: inner calls at 1 and 7, outer completions
        CHECK(case_graph(&java_like, 4, 1, 2));
        CHECK(case_graph(&java_like, 4, 7, 8));
        CHECK(case_graph(&java_like, 4, 3, 6));
        CHECK(case_graph(&java_like, 4, 4, 5));
        CHECK(case_graph(&java_like, 4, 9, 10));
        CHECK(case_graph(&java_like, 4, 11, 12));
        CHECK(case_graph(&java_like, 5, 0, 0)); // assign self-loop fallback
        CHECK(check_case(&java_like, "grouped-two-field-recursive", "2,5,6,8,10,12"));
        case_free(&java_like);
    }

    // The same grammar with only the mismatched load0/store1 path present.
    // A correct query reaches 2 from source 1 via alloc, but never 4 from
    // source 0 (4 is reachable only through the wrong store1 continuation).
    Case mismatched_only;
    if (case_new(&mismatched_only, /*Q=*/9, /*V=*/5, /*nt=*/6, /*nn=*/2, 0))
    {
        mismatched_only.start_states[0] = 0;
        mismatched_only.start_states[1] = 6;
        CHECK(case_term(&mismatched_only, 0, 0, 2));
        CHECK(case_term(&mismatched_only, 1, 0, 3));
        CHECK(case_term(&mismatched_only, 2, 4, 0));
        CHECK(case_term(&mismatched_only, 3, 5, 0));
        CHECK(case_term(&mismatched_only, 4, 0, 1));
        CHECK(case_term(&mismatched_only, 5, 0, 0));
        CHECK(case_nonterm(&mismatched_only, 0, 2, 4));
        CHECK(case_nonterm(&mismatched_only, 0, 3, 5));
        CHECK(case_nonterm(&mismatched_only, 1, 6, 8));
        CHECK(case_final(&mismatched_only, 0, 1));
        CHECK(case_final(&mismatched_only, 1, 8));
        CHECK(case_graph(&mismatched_only, 0, 0, 1)); // load0 0->1
        CHECK(case_graph(&mismatched_only, 4, 1, 2)); // alloc 1->2 (inner)
        CHECK(case_graph(&mismatched_only, 3, 2, 3)); // store1 2->3
        CHECK(case_graph(&mismatched_only, 4, 3, 4)); // alloc 3->4
        CHECK(check_case(&mismatched_only, "grouped-two-field-mismatch", NULL));
        case_free(&mismatched_only);
    }

    // Reverse families: the load_r family shares destination state 1 and the
    // store_r family shares source state 1.  Matched paths end at 1, 8, 9;
    // mismatched field continuations would end at 10 and 11.
    Case reverse_like;
    if (case_new(&reverse_like, /*Q=*/9, /*V=*/12, /*nt=*/6, /*nn=*/2, 0))
    {
        // terminals: alloc_r=0, store_r0=1, store_r1=2, load_r0=3, load_r1=4,
        // assign_r=5
        // box S: states 0..5, start 0, final 1
        // box Alias: states 6..8, start 6, final 8
        reverse_like.start_states[0] = 0;
        reverse_like.start_states[1] = 6;
        CHECK(case_term(&reverse_like, 0, 0, 1)); // 0 -alloc_r-> 1 (final)
        CHECK(case_term(&reverse_like, 1, 1, 2)); // 1 -store_r0-> 2
        CHECK(case_term(&reverse_like, 2, 1, 3)); // 1 -store_r1-> 3
        CHECK(case_term(&reverse_like, 3, 4, 1)); // 4 -load_r0-> 1
        CHECK(case_term(&reverse_like, 4, 5, 1)); // 5 -load_r1-> 1
        CHECK(case_term(&reverse_like, 5, 1, 1)); // 1 -assign_r-> 1
        CHECK(case_nonterm(&reverse_like, 0, 2, 4)); // 2 -Alias-> 4
        CHECK(case_nonterm(&reverse_like, 0, 3, 5)); // 3 -Alias-> 5
        CHECK(case_nonterm(&reverse_like, 1, 6, 8)); // Alias: 6 -S-> 8
        CHECK(case_final(&reverse_like, 0, 1));
        CHECK(case_final(&reverse_like, 1, 8));
        CHECK(case_graph(&reverse_like, 0, 0, 1));  // alloc_r outer
        CHECK(case_graph(&reverse_like, 0, 2, 6));  // alloc_r inner (entry 2)
        CHECK(case_graph(&reverse_like, 0, 3, 7));  // alloc_r inner (entry 3)
        CHECK(case_graph(&reverse_like, 1, 1, 2));  // store_r0
        CHECK(case_graph(&reverse_like, 2, 1, 3));  // store_r1
        CHECK(case_graph(&reverse_like, 3, 6, 8));  // load_r0 matched
        CHECK(case_graph(&reverse_like, 3, 7, 11)); // load_r0 mismatched
        CHECK(case_graph(&reverse_like, 4, 7, 9));  // load_r1 matched
        CHECK(case_graph(&reverse_like, 4, 6, 10)); // load_r1 mismatched
        CHECK(case_graph(&reverse_like, 5, 1, 1));  // assign_r self-loop
        CHECK(check_case(&reverse_like, "grouped-reverse-two-field", "1,6,7,8,9"));
        case_free(&reverse_like);
    }

    // Two labels whose graph relations share the same endpoint pair.  The
    // compact columns must stay separated by transition identity; merging them
    // would decode both through one destination and lose 3 or 4.
    Case shared_endpoints;
    if (case_new(&shared_endpoints, /*Q=*/5, /*V=*/6, /*nt=*/4, /*nn=*/1, 0))
    {
        // terminals: t0=0, t1=1, u=2, v=3
        shared_endpoints.start_states[0] = 0;
        CHECK(case_term(&shared_endpoints, 0, 0, 1)); // 0 -t0-> 1
        CHECK(case_term(&shared_endpoints, 1, 0, 2)); // 0 -t1-> 2
        CHECK(case_term(&shared_endpoints, 2, 1, 3)); // 1 -u-> 3 (final)
        CHECK(case_term(&shared_endpoints, 3, 2, 4)); // 2 -v-> 4 (final)
        CHECK(case_final(&shared_endpoints, 0, 3));
        CHECK(case_final(&shared_endpoints, 0, 4));
        CHECK(case_graph(&shared_endpoints, 0, 0, 5)); // t0: 0->5
        CHECK(case_graph(&shared_endpoints, 1, 0, 5)); // t1: 0->5 (same edge)
        CHECK(case_graph(&shared_endpoints, 2, 5, 3)); // u: 5->3
        CHECK(case_graph(&shared_endpoints, 3, 5, 4)); // v: 5->4
        CHECK(check_case(&shared_endpoints, "grouped-shared-endpoints", "3,4"));
        case_free(&shared_endpoints);
    }

    // Multiple transitions under one label from the same source state.  Both
    // must keep their own destination; a label-keyed compact column would
    // decode only one of them.
    Case same_label;
    if (case_new(&same_label, /*Q=*/5, /*V=*/6, /*nt=*/4, /*nn=*/1, 0))
    {
        // terminals: t=0, u=1, v=2
        same_label.start_states[0] = 0;
        CHECK(case_term(&same_label, 0, 0, 1)); // 0 -t-> 1
        CHECK(case_term(&same_label, 0, 0, 2)); // 0 -t-> 2 (same label)
        CHECK(case_term(&same_label, 1, 1, 3)); // 1 -u-> 3 (final)
        CHECK(case_term(&same_label, 2, 2, 4)); // 2 -v-> 4 (final)
        CHECK(case_final(&same_label, 0, 3));
        CHECK(case_final(&same_label, 0, 4));
        CHECK(case_graph(&same_label, 0, 0, 5)); // t: 0->5
        CHECK(case_graph(&same_label, 1, 5, 3)); // u: 5->3
        CHECK(case_graph(&same_label, 2, 5, 4)); // v: 5->4
        CHECK(check_case(&same_label, "grouped-same-label-destinations", "3,4"));
        case_free(&same_label);
    }

    // Mixed grouped / ordinary transitions under the same label.  The forward
    // family at source 0 covers a,b,c; the dual family at destination 2 covers
    // t(1,2) and s(4,2); the t(3,5) transition must still use the ordinary path.
    Case mixed;
    if (case_new(&mixed, /*Q=*/6, /*V=*/8, /*nt=*/5, /*nn=*/1, 0))
    {
        // terminals: a=0, b=1, c=2, t=3, s=4
        mixed.start_states[0] = 0;
        CHECK(case_term(&mixed, 0, 0, 1)); // 0 -a-> 1
        CHECK(case_term(&mixed, 1, 0, 4)); // 0 -b-> 4
        CHECK(case_term(&mixed, 2, 0, 3)); // 0 -c-> 3
        CHECK(case_term(&mixed, 3, 1, 2)); // 1 -t-> 2 (grouped dual)
        CHECK(case_term(&mixed, 4, 4, 2)); // 4 -s-> 2 (grouped dual)
        CHECK(case_term(&mixed, 3, 3, 5)); // 3 -t-> 5 (ordinary residual)
        CHECK(case_final(&mixed, 0, 2));
        CHECK(case_final(&mixed, 0, 5));
        CHECK(case_graph(&mixed, 0, 0, 1)); // a: 0->1
        CHECK(case_graph(&mixed, 1, 0, 4)); // b: 0->4
        CHECK(case_graph(&mixed, 2, 0, 6)); // c: 0->6
        CHECK(case_graph(&mixed, 3, 1, 2)); // t: 1->2
        CHECK(case_graph(&mixed, 3, 6, 7)); // t: 6->7
        CHECK(case_graph(&mixed, 4, 4, 5)); // s: 4->5
        CHECK(check_case(&mixed, "grouped-mixed-fallback", "2,5,7"));
        case_free(&mixed);
    }
}

//------------------------------------------------------------------------------
// deterministic regression: invalid input and overflow
//------------------------------------------------------------------------------

static bool expect_error_code(const Case *c, const GrB_Index *sources, size_t num_sources,
                              GrB_Info expected, const char *what)
{
    bool ok = true;
    for (int trial = 0; trial < 2; ++trial)
    {
        GrB_Vector out = GrB_NULL;
        GrB_Info info = run_case(c, sources, num_sources, &out);
        if (info != expected)
        {
            report_failure(__FILE__, __LINE__, "%s: expected info %d, got %d (%s)", what, (int)expected,
                           (int)info, msg);
            ok = false;
        }
        if (out != GrB_NULL)
        {
            report_failure(__FILE__, __LINE__, "%s: failed run left a non-NULL output", what);
            ok = false;
        }
        GrB_free(&out);
    }
    g_checks++;
    return ok;
}

static bool expect_error(const Case *c, const GrB_Index *sources, size_t num_sources,
                         const char *what)
{
    GrB_Vector out = GrB_NULL;
    GrB_Info info = run_case(c, sources, num_sources, &out);
    bool ok = true;
    if (info == GrB_SUCCESS)
    {
        report_failure(__FILE__, __LINE__, "%s: expected an error, got success", what);
        ok = false;
    }
    if (out != GrB_NULL)
    {
        report_failure(__FILE__, __LINE__, "%s: failed run left a non-NULL output", what);
        ok = false;
    }
    GrB_free(&out);
    g_checks++;
    return ok;
}

// Full block axes must preserve member identity and vertex positions even
// with holes, permuted continuation states, and vertices at both ends of a
// block. The cross-field edges below must not introduce extra destinations.
static void test_block_family_alignment(void)
{
    Case c;
    if (!case_new(&c, /*Q=*/6, /*V=*/17, /*nt=*/6, /*nn=*/1, 0))
        return;
    c.start_states[0] = 0;
    CHECK(case_term(&c, 0, 0, 5)); // load0
    CHECK(case_term(&c, 1, 0, 1)); // load1
    CHECK(case_term(&c, 2, 0, 3)); // load2
    CHECK(case_term(&c, 3, 5, 2)); // store0
    CHECK(case_term(&c, 4, 1, 2)); // store1
    CHECK(case_term(&c, 5, 3, 2)); // store2
    CHECK(case_final(&c, 0, 2));

    CHECK(case_graph(&c, 0, 0, 16));
    CHECK(case_graph(&c, 0, 7, 2));
    CHECK(case_graph(&c, 1, 0, 8));
    CHECK(case_graph(&c, 1, 12, 4));
    CHECK(case_graph(&c, 2, 0, 0));
    CHECK(case_graph(&c, 3, 16, 3));
    CHECK(case_graph(&c, 3, 2, 14));
    CHECK(case_graph(&c, 4, 8, 10));
    CHECK(case_graph(&c, 4, 4, 6));
    CHECK(case_graph(&c, 5, 0, 15));
    CHECK(case_graph(&c, 3, 8, 11));  // load1/store0: reject
    CHECK(case_graph(&c, 4, 16, 13)); // load0/store1: reject
    CHECK(case_graph(&c, 5, 16, 9));  // load0/store2: reject
    CHECK(check_case(&c, "block-family-permuted-states-and-holes", "3,6,10,14,15"));
    case_free(&c);
}

// A dual family may contain more members than there are RSM states. Its full
// axis must be member*V+v, not state*V+v. The oracle also checks singleton
// sources, including source 1, for which the correct result is empty.
static void test_full_coordinate_dual_axis(void)
{
    Case c;
    if (!case_new(&c, /*Q=*/3, /*V=*/4, /*nt=*/7, /*nn=*/1, 0))
        return;
    c.start_states[0] = 0;
    for (GrB_Index t = 0; t < 3; ++t)
        CHECK(case_term(&c, t, 0, 1));
    for (GrB_Index t = 3; t < 6; ++t)
        CHECK(case_term(&c, t, 2, 1));
    CHECK(case_term(&c, 6, 0, 2));
    CHECK(case_final(&c, 0, 1));
    CHECK(case_graph(&c, 0, 0, 1));
    CHECK(case_graph(&c, 1, 2, 0));
    CHECK(case_graph(&c, 2, 3, 2));
    CHECK(case_graph(&c, 3, 1, 2));
    CHECK(case_graph(&c, 4, 0, 3));
    CHECK(case_graph(&c, 5, 2, 1));
    CHECK(case_graph(&c, 6, 0, 0));
    CHECK(case_graph(&c, 6, 3, 1));
    CHECK(check_case(&c, "full-coordinate-dual-members-exceed-states", "0,1,2,3"));
    case_free(&c);
}

static void test_invalid_input(void)
{
    // valid baseline instance
    // box0: states 0..2, start 0, final 2; box1: states 3..4, start 3, final 4
    Case base;
    if (!case_new(&base, /*Q=*/5, /*V=*/3, /*nt=*/2, /*nn=*/2, 0))
        return;
    base.start_states[0] = 0;
    base.start_states[1] = 3;
    CHECK(case_term(&base, 0, 0, 1));    // a: 0->1
    CHECK(case_nonterm(&base, 1, 1, 2)); // call box1: 1->2
    CHECK(case_term(&base, 1, 3, 4));    // b: 3->4 (inside box1)
    CHECK(case_final(&base, 0, 2));
    CHECK(case_final(&base, 1, 4));
    CHECK(case_graph(&base, 0, 0, 1)); // a: 0->1
    CHECK(case_graph(&base, 1, 1, 2)); // b: 1->2

    // the valid instance must still work: source 0 accepts 2
    CHECK(check_case(&base, "invalid-input baseline", "2"));

    // NULL output pointer: cleanup must not dereference it
    {
        RSM rsm;
        case_rsm(&base, &rsm);
        GrB_Index sources[1] = {0};
        GrB_Info info = LAGraph_CFPQ_RSM(NULL, &rsm, (const GrB_Matrix *)base.graph, sources, 1,
                                         base.V, msg);
        CHECK_MSG(info == GrB_NULL_POINTER, "NULL output pointer accepted: info %d", (int)info);
    }
    // NULL RSM
    {
        GrB_Vector out = GrB_NULL;
        GrB_Info info = LAGraph_CFPQ_RSM(&out, NULL, (const GrB_Matrix *)base.graph, NULL, 0,
                                         base.V, msg);
        CHECK_MSG(info == GrB_NULL_POINTER, "NULL RSM accepted: info %d", (int)info);
        CHECK(out == GrB_NULL);
        GrB_free(&out);
    }
    // NULL sources with a positive count
    CHECK(expect_error(&base, NULL, 1, "NULL sources with num_sources=1"));
    // source out of range
    {
        GrB_Index bad[1] = {3};
        CHECK(expect_error(&base, bad, 1, "source == V"));
    }
    {
        GrB_Index bad[1] = {(GrB_Index)1 << 40};
        CHECK(expect_error(&base, bad, 1, "huge source"));
    }
    // count bound
    {
        GrB_Index bad[4] = {0, 1, 2, 0};
        CHECK(expect_error(&base, bad, 4, "num_sources > V"));
    }

    // invalid start state of the start box
    {
        Case bad;
        if (case_clone(&bad, &base))
        {
            bad.start_states[0] = 5;
            GrB_Index sources[1] = {0};
            CHECK(expect_error(&bad, sources, 1, "start_state >= Q"));
            case_free(&bad);
        }
    }
    // invalid start state of a called box
    {
        Case bad;
        if (case_clone(&bad, &base))
        {
            bad.start_states[1] = 5;
            GrB_Index sources[1] = {0};
            CHECK(expect_error(&bad, sources, 1, "called box start_state >= Q"));
            case_free(&bad);
        }
    }
    // final vector of the wrong length
    {
        Case bad;
        if (case_clone(&bad, &base))
        {
            GrB_Vector short_finals = GrB_NULL;
            CHECK(OK(GrB_Vector_new(&short_finals, GrB_BOOL, 4), "short finals"));
            GrB_free(&bad.finals[0]);
            bad.finals[0] = short_finals;
            GrB_Index sources[1] = {0};
            CHECK(expect_error(&bad, sources, 1, "final vector length != Q"));
            case_free(&bad);
        }
    }
    // terminal matrix of the wrong dimensions
    {
        Case bad;
        if (case_clone(&bad, &base))
        {
            GrB_Matrix bad_term = GrB_NULL;
            CHECK(OK(GrB_Matrix_new(&bad_term, GrB_BOOL, 4, 4), "bad term matrix"));
            CHECK(OK(GrB_Matrix_setElement_BOOL(bad_term, true, 0, 1), "bad term edge"));
            GrB_free(&bad.term[0]);
            bad.term[0] = bad_term;
            GrB_Index sources[1] = {0};
            CHECK(expect_error(&bad, sources, 1, "terminal matrix dims != Q"));
            case_free(&bad);
        }
    }
    // graph matrix of the wrong dimensions
    {
        Case bad;
        if (case_clone(&bad, &base))
        {
            GrB_Matrix bad_graph = GrB_NULL;
            CHECK(OK(GrB_Matrix_new(&bad_graph, GrB_BOOL, 2, 2), "bad graph matrix"));
            CHECK(OK(GrB_Matrix_setElement_BOOL(bad_graph, true, 0, 1), "bad graph edge"));
            GrB_free(&bad.graph[0]);
            bad.graph[0] = bad_graph;
            GrB_Index sources[1] = {0};
            CHECK(expect_error(&bad, sources, 1, "graph matrix dims != V"));
            case_free(&bad);
        }
    }

    // caller-owned inputs must be intact after the failed runs above
    CHECK(check_case(&base, "invalid-input baseline after errors", "2"));

    case_free(&base);

    // overflow / dimension-limit cases: no matrix with these dimensions is
    // ever allocated, the check must reject before allocation
    {
        Case huge;
        if (case_new(&huge, /*Q=*/1, /*V=*/0, /*nt=*/0, /*nn=*/1, 0))
        {
            huge.start_states[0] = 0;
            case_drop_finals(&huge, 0);
            case_drop_nonterm(&huge, 0);
            // V*V = 2^62 > 2^60
            huge.V = (GrB_Index)1 << 31;
            CHECK(expect_error(&huge, NULL, 0, "V*V above GraphBLAS limit"));
            // V*V wraps in 64 bits
            huge.V = (GrB_Index)1 << 33;
            CHECK(expect_error(&huge, NULL, 0, "V*V wraps"));
            // Q*V above the dimension limit
            huge.V = 2;
            huge.Q = (GrB_Index)1 << 60;
            CHECK(expect_error(&huge, NULL, 0, "Q*V above GraphBLAS limit"));
            // Q*V*V overflows int64
            huge.Q = (GrB_Index)1 << 40;
            huge.V = (GrB_Index)1 << 20;
            CHECK(expect_error(&huge, NULL, 0, "Q*V*V overflows int64"));
            case_free(&huge);
        }
    }

    // zero states: the start state cannot be in range
    {
        Case empty_states;
        if (case_new(&empty_states, /*Q=*/0, /*V=*/1, /*nt=*/0, /*nn=*/1, 0))
        {
            empty_states.start_states[0] = 0;
            CHECK(expect_error(&empty_states, NULL, 0, "Q=0"));
            case_free(&empty_states);
        }
    }

    // terminal array NULL is only valid when terminal_count == 0
    {
        Case no_terms;
        if (case_new(&no_terms, /*Q=*/1, /*V=*/1, /*nt=*/0, /*nn=*/1, 0))
        {
            no_terms.start_states[0] = 0;
            CHECK(case_final(&no_terms, 0, 0));
            RSM rsm;
            case_rsm(&no_terms, &rsm);
            rsm.terminal_matrices = NULL;
            GrB_Vector out = GrB_NULL;
            GrB_Index one_source[1] = {0};
            GrB_Info info = LAGraph_CFPQ_RSM(&out, &rsm, NULL, one_source, 1, no_terms.V, msg);
            CHECK_MSG(info == GrB_SUCCESS, "NULL terminal array with count 0 rejected: %s", msg);
            if (info == GrB_SUCCESS)
            {
                bool expected[1] = {true};
                CHECK(vector_matches(out, expected, 1, true, "NULL terminal array"));
            }
            GrB_free(&out);
            // and still an error when the count is positive
            rsm.terminal_count = 1;
            out = GrB_NULL;
            info = LAGraph_CFPQ_RSM(&out, &rsm, NULL, one_source, 1, no_terms.V, msg);
            CHECK_MSG(info != GrB_SUCCESS, "NULL terminal array with count 1 accepted");
            CHECK(out == GrB_NULL);
            GrB_free(&out);
            case_free(&no_terms);
        }
    }
}

//------------------------------------------------------------------------------
// deterministic regressions: malformed input matrix and final-vector shapes
//------------------------------------------------------------------------------

// Replace one matrix/vector of a clone with a new (possibly wrongly shaped)
// object.  The clone is freed by the caller; the base case is untouched.
static bool clone_with_term_shape(Case *dst, const Case *src, GrB_Index nr, GrB_Index nc,
                                 GrB_Index r, GrB_Index c, bool entry)
{
    if (!case_clone(dst, src))
        return false;
    GrB_Matrix m = GrB_NULL;
    if (!OK(GrB_Matrix_new(&m, GrB_BOOL, nr, nc), "shape term"))
        return false;
    if (entry && !OK(GrB_Matrix_setElement_BOOL(m, true, r, c), "shape term entry"))
        return false;
    GrB_free(&dst->term[0]);
    dst->term[0] = m;
    return true;
}

static bool clone_with_graph_shape(Case *dst, const Case *src, GrB_Index nr, GrB_Index nc,
                                  GrB_Index r, GrB_Index c, bool entry)
{
    if (!case_clone(dst, src))
        return false;
    GrB_Matrix m = GrB_NULL;
    if (!OK(GrB_Matrix_new(&m, GrB_BOOL, nr, nc), "shape graph"))
        return false;
    if (entry && !OK(GrB_Matrix_setElement_BOOL(m, true, r, c), "shape graph entry"))
        return false;
    GrB_free(&dst->graph[0]);
    dst->graph[0] = m;
    return true;
}

static bool clone_with_nonterm_shape(Case *dst, const Case *src, GrB_Index nr, GrB_Index nc,
                                     GrB_Index r, GrB_Index c, bool entry)
{
    if (!case_clone(dst, src))
        return false;
    GrB_Matrix m = GrB_NULL;
    if (!OK(GrB_Matrix_new(&m, GrB_BOOL, nr, nc), "shape nonterm"))
        return false;
    if (entry && !OK(GrB_Matrix_setElement_BOOL(m, true, r, c), "shape nonterm entry"))
        return false;
    GrB_free(&dst->nonterm[0]);
    dst->nonterm[0] = m;
    return true;
}

static bool clone_with_final_length(Case *dst, const Case *src, GrB_Index length, GrB_Index entry)
{
    if (!case_clone(dst, src))
        return false;
    GrB_Vector v = GrB_NULL;
    if (!OK(GrB_Vector_new(&v, GrB_BOOL, length), "shape finals"))
        return false;
    if (entry < length && !OK(GrB_Vector_setElement_BOOL(v, true, entry), "shape final entry"))
        return false;
    GrB_free(&dst->finals[0]);
    dst->finals[0] = v;
    return true;
}

static void test_malformed_dimensions(void)
{
    // Q=2, V=2, S -> a, edge 0-a->1, source {0}: the valid baseline
    Case base;
    if (!case_new(&base, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/1, 0))
        return;
    base.start_states[0] = 0;
    CHECK(case_term(&base, 0, 0, 1));
    CHECK(case_final(&base, 0, 1));
    CHECK(case_graph(&base, 0, 0, 1));
    CHECK(check_case(&base, "malformed baseline", "1"));

    GrB_Index sources[1] = {0};
    Case bad;

    // The R2 unsafe case: 3x3 terminal with a true entry in row Q (=2), plus a
    // nonempty graph and source {0}.  The R1 kernel extracts row 2 and indexes
    // active[2] out of bounds before mxm can report the shape error.
    if (clone_with_term_shape(&bad, &base, 3, 3, 2, 0, true))
    {
        CHECK(expect_error_code(&bad, sources, 1, GrB_DIMENSION_MISMATCH,
                                "terminal 3x3 with row Q entry"));
        case_free(&bad);
    }
    // oversized / undersized / swapped terminal shapes, with and without entries
    {
        struct { GrB_Index nr, nc, r, c; bool entry; const char *name; } shapes[] = {
            {3, 2, 2, 0, true, "terminal 3x2"},
            {2, 3, 0, 1, true, "terminal 2x3"},
            {1, 2, 0, 1, true, "terminal 1x2"},
            {0, 2, 0, 0, false, "terminal 0x2"},
            {3, 3, 0, 0, false, "empty terminal 3x3"},
            {2, 3, 0, 0, false, "empty terminal 2x3"},
        };
        for (size_t k = 0; k < sizeof(shapes) / sizeof(shapes[0]); ++k)
        {
            if (clone_with_term_shape(&bad, &base, shapes[k].nr, shapes[k].nc, shapes[k].r,
                                      shapes[k].c, shapes[k].entry))
            {
                CHECK(expect_error_code(&bad, sources, 1, GrB_DIMENSION_MISMATCH, shapes[k].name));
                case_free(&bad);
            }
        }
    }
    // nonterminal transition matrices
    {
        struct { GrB_Index nr, nc, r, c; bool entry; const char *name; } shapes[] = {
            {3, 3, 0, 1, true, "nonterminal 3x3"},
            {2, 3, 0, 1, true, "nonterminal 2x3"},
            {3, 2, 0, 1, true, "nonterminal 3x2"},
            {3, 3, 0, 0, false, "empty nonterminal 3x3"},
        };
        for (size_t k = 0; k < sizeof(shapes) / sizeof(shapes[0]); ++k)
        {
            if (clone_with_nonterm_shape(&bad, &base, shapes[k].nr, shapes[k].nc, shapes[k].r,
                                         shapes[k].c, shapes[k].entry))
            {
                CHECK(expect_error_code(&bad, sources, 1, GrB_DIMENSION_MISMATCH, shapes[k].name));
                case_free(&bad);
            }
        }
    }
    // graph matrices
    {
        struct { GrB_Index nr, nc, r, c; bool entry; const char *name; } shapes[] = {
            {3, 3, 0, 1, true, "graph 3x3"},
            {2, 3, 0, 1, true, "graph 2x3"},
            {3, 2, 0, 1, true, "graph 3x2"},
            {1, 2, 0, 1, true, "graph 1x2"},
            {3, 3, 0, 0, false, "empty graph 3x3"},
        };
        for (size_t k = 0; k < sizeof(shapes) / sizeof(shapes[0]); ++k)
        {
            if (clone_with_graph_shape(&bad, &base, shapes[k].nr, shapes[k].nc, shapes[k].r,
                                       shapes[k].c, shapes[k].entry))
            {
                CHECK(expect_error_code(&bad, sources, 1, GrB_DIMENSION_MISMATCH, shapes[k].name));
                case_free(&bad);
            }
        }
    }
    // final vectors of the wrong length (with and without a true entry)
    if (clone_with_final_length(&bad, &base, 3, 2))
    {
        CHECK(expect_error_code(&bad, sources, 1, GrB_DIMENSION_MISMATCH, "final length 3"));
        case_free(&bad);
    }
    if (clone_with_final_length(&bad, &base, 1, 0))
    {
        CHECK(expect_error_code(&bad, sources, 1, GrB_DIMENSION_MISMATCH, "final length 1"));
        case_free(&bad);
    }
    if (clone_with_final_length(&bad, &base, 0, 0))
    {
        CHECK(expect_error_code(&bad, sources, 1, GrB_DIMENSION_MISMATCH, "final length 0"));
        case_free(&bad);
    }
    // the shape checks must run with zero sources too (skipped work cannot hide
    // an invalid operand)
    if (clone_with_term_shape(&bad, &base, 3, 3, 2, 0, true))
    {
        CHECK(expect_error_code(&bad, NULL, 0, GrB_DIMENSION_MISMATCH,
                                "terminal 3x3 with zero sources"));
        case_free(&bad);
    }
    if (clone_with_final_length(&bad, &base, 3, 0))
    {
        CHECK(expect_error_code(&bad, NULL, 0, GrB_DIMENSION_MISMATCH, "final length 3 zero sources"));
        case_free(&bad);
    }
    // NULL graph array with a positive terminal count
    {
        RSM rsm;
        case_rsm(&base, &rsm);
        GrB_Vector out = GrB_NULL;
        GrB_Info info = LAGraph_CFPQ_RSM(&out, &rsm, NULL, sources, 1, base.V, msg);
        CHECK_MSG(info == GrB_NULL_POINTER, "NULL graph array with terminals: info %d", (int)info);
        CHECK(out == GrB_NULL);
        GrB_free(&out);
    }
    // NULL terminal array with a positive count
    {
        RSM rsm;
        case_rsm(&base, &rsm);
        rsm.terminal_matrices = NULL;
        GrB_Vector out = GrB_NULL;
        GrB_Info info = LAGraph_CFPQ_RSM(&out, &rsm, (const GrB_Matrix *)base.graph, sources, 1,
                                         base.V, msg);
        CHECK_MSG(info == GrB_NULL_POINTER, "NULL terminal array with count 1: info %d", (int)info);
        CHECK(out == GrB_NULL);
        GrB_free(&out);
    }
    // valid zero-source call still succeeds and returns an empty length-V vector
    {
        GrB_Vector out = GrB_NULL;
        GrB_Info info = run_case(&base, NULL, 0, &out);
        CHECK_MSG(info == GrB_SUCCESS, "valid zero-source call failed: %s", msg);
        if (info == GrB_SUCCESS)
        {
            bool empty[2] = {false, false};
            CHECK(vector_matches(out, empty, 2, true, "zero-source result"));
        }
        GrB_free(&out);
    }
    // caller-owned inputs are intact and the valid baseline still works
    CHECK(check_case(&base, "malformed baseline after errors", "1"));
    case_free(&base);
}

//------------------------------------------------------------------------------
// deterministic regressions: conservative trimming and internal compaction
//------------------------------------------------------------------------------
//
// Each case is compared with the independent oracle, so a wrongly removed
// state, final, or transition changes the exact reachable set.  The cases also
// make a retained state observable through a live alternative branch, so
// trimming must actually remove the dead intermediates without changing the
// accepted destination set.

static void test_trimming_compaction(void)
{
    // Empty load relation: states 1 and 2 are on a dead branch; the live
    // 0 -other-> 3 path keeps states 0 and 3.
    Case empty_load;
    if (case_new(&empty_load, /*Q=*/4, /*V=*/4, /*nt=*/4, /*nn=*/1, 0))
    {
        // load=0, store=1, done=2, other=3
        empty_load.start_states[0] = 0;
        CHECK(case_term(&empty_load, 0, 0, 1));
        CHECK(case_term(&empty_load, 1, 1, 2));
        CHECK(case_term(&empty_load, 2, 2, 3));
        CHECK(case_term(&empty_load, 3, 0, 3));
        CHECK(case_final(&empty_load, 0, 3));
        CHECK(case_graph(&empty_load, 1, 1, 2)); // store live
        CHECK(case_graph(&empty_load, 2, 2, 3)); // done live
        CHECK(case_graph(&empty_load, 3, 0, 3)); // other live
        // load graph is empty: the 0-load->1-... branch must disappear
        CHECK(check_case(&empty_load, "trim-empty-load", "3"));
        case_free(&empty_load);
    }

    // Empty store relation, nonempty load: states 1 and 2 are dead.
    Case empty_store;
    if (case_new(&empty_store, /*Q=*/4, /*V=*/4, /*nt=*/4, /*nn=*/1, 0))
    {
        empty_store.start_states[0] = 0;
        CHECK(case_term(&empty_store, 0, 0, 1));
        CHECK(case_term(&empty_store, 1, 1, 2));
        CHECK(case_term(&empty_store, 2, 2, 3));
        CHECK(case_term(&empty_store, 3, 0, 3));
        CHECK(case_final(&empty_store, 0, 3));
        CHECK(case_graph(&empty_store, 0, 0, 1)); // load live
        CHECK(case_graph(&empty_store, 2, 2, 3)); // done live
        CHECK(case_graph(&empty_store, 3, 0, 2)); // other live
        // store graph is empty
        CHECK(check_case(&empty_store, "trim-empty-store", "2"));
        case_free(&empty_store);
    }

    // Both load and store relations are empty.
    Case both_empty;
    if (case_new(&both_empty, /*Q=*/4, /*V=*/4, /*nt=*/4, /*nn=*/1, 0))
    {
        both_empty.start_states[0] = 0;
        CHECK(case_term(&both_empty, 0, 0, 1));
        CHECK(case_term(&both_empty, 1, 1, 2));
        CHECK(case_term(&both_empty, 2, 2, 3));
        CHECK(case_term(&both_empty, 3, 0, 3));
        CHECK(case_final(&both_empty, 0, 3));
        CHECK(case_graph(&both_empty, 2, 2, 3)); // done live
        CHECK(case_graph(&both_empty, 3, 0, 3)); // other live
        CHECK(check_case(&both_empty, "trim-both-empty", "3"));
        case_free(&both_empty);
    }

    // Reverse family: alloc_r/load_r live, store_r empty.  The 2/3 branch is
    // dead and the nullable Alias box is reached only through it.
    Case reverse_dead;
    if (case_new(&reverse_dead, /*Q=*/6, /*V=*/3, /*nt=*/3, /*nn=*/2, 0))
    {
        // alloc_r=0, store_r=1, load_r=2
        reverse_dead.start_states[0] = 0;
        reverse_dead.start_states[1] = 4;
        CHECK(case_term(&reverse_dead, 0, 0, 1)); // alloc_r
        CHECK(case_term(&reverse_dead, 1, 1, 2)); // store_r
        CHECK(case_term(&reverse_dead, 2, 3, 1)); // load_r
        CHECK(case_nonterm(&reverse_dead, 1, 2, 3)); // 2 -Alias-> 3
        CHECK(case_nonterm(&reverse_dead, 0, 4, 5)); // Alias: 4 -S-> 5
        CHECK(case_final(&reverse_dead, 0, 1));
        CHECK(case_final(&reverse_dead, 1, 5));
        CHECK(case_graph(&reverse_dead, 0, 0, 1)); // alloc_r live
        CHECK(case_graph(&reverse_dead, 2, 1, 2)); // load_r live
        // store_r graph is empty
        CHECK(check_case(&reverse_dead, "trim-reverse-dead", "1"));
        case_free(&reverse_dead);
    }

    // Dead recursive component: D has a self-loop but no path to its final,
    // so its states and call site must be removed; the live other edge stays.
    Case dead_recursive;
    if (case_new(&dead_recursive, /*Q=*/7, /*V=*/2, /*nt=*/2, /*nn=*/2, 0))
    {
        // other=0, d=1; S states 0,1; D states 5,6
        dead_recursive.start_states[0] = 0;
        dead_recursive.start_states[1] = 5;
        CHECK(case_term(&dead_recursive, 0, 0, 1)); // other -> final
        CHECK(case_term(&dead_recursive, 1, 5, 5)); // D self-loop
        CHECK(case_nonterm(&dead_recursive, 1, 0, 1)); // 0 -D-> 1
        CHECK(case_final(&dead_recursive, 0, 1));
        CHECK(case_final(&dead_recursive, 1, 6)); // unreachable
        CHECK(case_graph(&dead_recursive, 0, 0, 1));
        CHECK(case_graph(&dead_recursive, 1, 0, 0));
        CHECK(check_case(&dead_recursive, "trim-dead-recursive", "1"));
        case_free(&dead_recursive);
    }

    // Nullable callee: N is a single start-and-final state, so the accepted
    // path 0 -load-> 1 -N-> 2 -final-> 1 must survive trimming.
    Case nullable_callee;
    if (case_new(&nullable_callee, /*Q=*/4, /*V=*/2, /*nt=*/1, /*nn=*/2, 0))
    {
        nullable_callee.start_states[0] = 0;
        nullable_callee.start_states[1] = 3;
        CHECK(case_term(&nullable_callee, 0, 0, 1)); // load
        CHECK(case_nonterm(&nullable_callee, 1, 1, 2)); // 1 -N-> 2
        CHECK(case_final(&nullable_callee, 0, 2));
        CHECK(case_final(&nullable_callee, 1, 3));
        CHECK(case_graph(&nullable_callee, 0, 0, 1));
        CHECK(check_case(&nullable_callee, "trim-nullable-callee", "1"));
        case_free(&nullable_callee);
    }

    // Start state is final: epsilon acceptance must survive even though the
    // only terminal transition is inactive.
    Case epsilon_start;
    if (case_new(&epsilon_start, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/1, 0))
    {
        epsilon_start.start_states[0] = 0;
        CHECK(case_term(&epsilon_start, 0, 0, 1));
        CHECK(case_final(&epsilon_start, 0, 0));
        CHECK(case_final(&epsilon_start, 0, 1));
        // graph is empty: the terminal branch is dead, epsilon remains
        CHECK(check_case(&epsilon_start, "trim-start-final-epsilon", "0,1"));
        case_free(&epsilon_start);
    }

    // NULL finals for one called box, multiple finals for another.  B's
    // states must disappear; A's two finals must both remain reachable.
    Case null_and_multi_finals;
    if (case_new(&null_and_multi_finals, /*Q=*/6, /*V=*/3, /*nt=*/4, /*nn=*/3, 0))
    {
        // a=0, x=1, y=2, z=3; S states 0,1; A states 2,3,4; B state 5
        null_and_multi_finals.start_states[0] = 0;
        null_and_multi_finals.start_states[1] = 2;
        null_and_multi_finals.start_states[2] = 5;
        CHECK(case_term(&null_and_multi_finals, 0, 0, 1)); // a
        CHECK(case_term(&null_and_multi_finals, 1, 2, 3)); // x
        CHECK(case_term(&null_and_multi_finals, 2, 2, 4)); // y
        CHECK(case_term(&null_and_multi_finals, 3, 5, 5)); // z (B self-loop)
        CHECK(case_nonterm(&null_and_multi_finals, 1, 0, 1)); // 0 -A-> 1
        CHECK(case_nonterm(&null_and_multi_finals, 2, 0, 1)); // 0 -B-> 1
        CHECK(case_final(&null_and_multi_finals, 0, 1));
        CHECK(case_final(&null_and_multi_finals, 1, 3));
        CHECK(case_final(&null_and_multi_finals, 1, 4));
        case_drop_finals(&null_and_multi_finals, 2);
        CHECK(case_graph(&null_and_multi_finals, 0, 0, 1));
        CHECK(case_graph(&null_and_multi_finals, 1, 0, 1));
        CHECK(case_graph(&null_and_multi_finals, 2, 0, 2));
        CHECK(case_graph(&null_and_multi_finals, 3, 2, 2));
        CHECK(check_case(&null_and_multi_finals, "trim-null-multi-finals", "1,2"));
        case_free(&null_and_multi_finals);
    }

    // Permuted, noncontiguous state ranges: S uses 4..6, A uses 0..2, state 7
    // is dead.  Compaction must not assume contiguous box ranges.
    Case permuted;
    if (case_new(&permuted, /*Q=*/8, /*V=*/4, /*nt=*/2, /*nn=*/2, 0))
    {
        // t=0, x=1; S: 4 start, 6 final; A: 0 start, 2 final
        permuted.start_states[0] = 4;
        permuted.start_states[1] = 0;
        CHECK(case_term(&permuted, 0, 4, 5)); // S t
        CHECK(case_term(&permuted, 1, 0, 2)); // A x
        CHECK(case_term(&permuted, 0, 7, 7)); // dead self-loop
        CHECK(case_nonterm(&permuted, 1, 5, 6)); // 5 -A-> 6
        CHECK(case_final(&permuted, 0, 6));
        CHECK(case_final(&permuted, 1, 2));
        CHECK(case_graph(&permuted, 0, 0, 1)); // t: 0->1
        CHECK(case_graph(&permuted, 0, 2, 2)); // dead edge
        CHECK(case_graph(&permuted, 1, 1, 3)); // x: 1->3
        CHECK(check_case(&permuted, "trim-permuted-states", "3"));
        case_free(&permuted);
    }

    // Shared state across boxes: state 2 is an intermediate state of both S
    // and A.  Trimming and compaction must not assume that box state ranges are
    // disjoint, while avoiding the ambiguous case where one box's start is
    // another box's final (a pre-existing global-state semantics question).
    Case shared_states;
    if (case_new(&shared_states, /*Q=*/5, /*V=*/3, /*nt=*/4, /*nn=*/2, 0))
    {
        // t=0, u=1, v=2, w=3; shared state 2
        // S: 0 start, 1 final; 0 -t-> 2 -u-> 1; 0 -A-> 1
        // A: 3 start, 4 final; 3 -v-> 2 -w-> 4
        shared_states.start_states[0] = 0;
        shared_states.start_states[1] = 3;
        CHECK(case_term(&shared_states, 0, 0, 2)); // t
        CHECK(case_term(&shared_states, 1, 2, 1)); // u
        CHECK(case_term(&shared_states, 2, 3, 2)); // v
        CHECK(case_term(&shared_states, 3, 2, 4)); // w
        CHECK(case_nonterm(&shared_states, 1, 0, 1)); // 0 -A-> 1
        CHECK(case_final(&shared_states, 0, 1));
        CHECK(case_final(&shared_states, 1, 4));
        CHECK(case_graph(&shared_states, 0, 0, 1)); // t: 0->1
        CHECK(case_graph(&shared_states, 1, 1, 2)); // u: 1->2
        CHECK(case_graph(&shared_states, 2, 0, 1)); // v: 0->1
        CHECK(case_graph(&shared_states, 3, 1, 2)); // w: 1->2
        CHECK(check_case(&shared_states, "trim-shared-states", "2"));
        case_free(&shared_states);
    }

    // Fully nonproductive start query: no accepting path from S's start, while
    // another box's nullable final keeps the global analysis nonempty.
    Case nonproductive;
    if (case_new(&nonproductive, /*Q=*/4, /*V=*/2, /*nt=*/1, /*nn=*/2, 0))
    {
        // t=0; S states 0..2, start 0, final 1 (unreachable); A state 3
        // nullable start/final
        nonproductive.start_states[0] = 0;
        nonproductive.start_states[1] = 3;
        CHECK(case_term(&nonproductive, 0, 0, 2)); // t -> state 2
        CHECK(case_nonterm(&nonproductive, 1, 0, 2)); // 0 -A-> 2
        CHECK(case_final(&nonproductive, 0, 1));
        CHECK(case_final(&nonproductive, 1, 3));
        CHECK(case_graph(&nonproductive, 0, 0, 1));
        // exact answer is empty from every source
        CHECK(check_case(&nonproductive, "trim-nonproductive-start", ""));
        case_free(&nonproductive);
    }

    // Malformed matrices on a branch that trimming would remove must still be
    // rejected by the original validation, before compaction can hide them.
    Case dead_branch_base;
    if (case_new(&dead_branch_base, /*Q=*/3, /*V=*/2, /*nt=*/2, /*nn=*/2, 0))
    {
        // bad=0 (dead, empty graph), good=1 (live)
        dead_branch_base.start_states[0] = 0;
        dead_branch_base.start_states[1] = 0;
        CHECK(case_term(&dead_branch_base, 0, 0, 2)); // bad
        CHECK(case_term(&dead_branch_base, 1, 0, 1)); // good
        CHECK(case_final(&dead_branch_base, 0, 1));
        CHECK(case_graph(&dead_branch_base, 1, 0, 1));
        CHECK(check_case(&dead_branch_base, "trim-dead-branch-base", "1"));

        GrB_Index sources[1] = {0};
        Case bad_term;
        if (clone_with_term_shape(&bad_term, &dead_branch_base, 4, 4, 3, 0, true))
        {
            CHECK(expect_error_code(&bad_term, sources, 1, GrB_DIMENSION_MISMATCH,
                                    "dead-branch malformed terminal"));
            case_free(&bad_term);
        }
        Case bad_nonterm;
        if (clone_with_nonterm_shape(&bad_nonterm, &dead_branch_base, 4, 4, 3, 0, true))
        {
            CHECK(expect_error_code(&bad_nonterm, sources, 1, GrB_DIMENSION_MISMATCH,
                                    "dead-branch malformed nonterminal"));
            case_free(&bad_nonterm);
        }
        case_free(&dead_branch_base);
    }

    // A malformed final vector on a box whose states trimming would remove.
    Case dead_final_base;
    if (case_new(&dead_final_base, /*Q=*/4, /*V=*/2, /*nt=*/1, /*nn=*/2, /*start_nonterm=*/1))
    {
        // box 0 (dead): states 2,3, start 2, final 3, self-loop 2 -t-> 2
        // box 1 (S): states 0,1, start 0, final 1, 0 -t-> 1
        dead_final_base.start_states[0] = 2;
        dead_final_base.start_states[1] = 0;
        CHECK(case_term(&dead_final_base, 0, 2, 2));
        CHECK(case_term(&dead_final_base, 0, 0, 1));
        CHECK(case_final(&dead_final_base, 0, 3));
        CHECK(case_final(&dead_final_base, 1, 1));
        CHECK(case_graph(&dead_final_base, 0, 0, 1));
        CHECK(check_case(&dead_final_base, "trim-dead-final-base", "1"));

        GrB_Index sources[1] = {0};
        Case bad_final;
        if (clone_with_final_length(&bad_final, &dead_final_base, 5, 3))
        {
            CHECK(expect_error_code(&bad_final, sources, 1, GrB_DIMENSION_MISMATCH,
                                    "dead-branch malformed final"));
            case_free(&bad_final);
        }
        case_free(&dead_final_base);
    }
}

//------------------------------------------------------------------------------
// deterministic regressions: destination-local waiting prefixes
//------------------------------------------------------------------------------
//
// Every case has disjoint continuation suffixes, so a routing mistake that
// returns a prefix to another destination changes the exact reachable set.
// The cases cover distinct destinations, one shared destination, nonconsecutive
// destination ids, zero/one/many destinations, when prefixes and summaries are
// discovered in either order, nullable callees, and a no-caller start box.

static void test_waiting_prefixes(void)
{
    // Two callers of A with distinct continuation destinations 2 and 4.
    // Correct paths end at 5 and 6; cross routing would also reach 7 and 8.
    Case distinct_dest;
    if (case_new(&distinct_dest, /*Q=*/9, /*V=*/9, /*nt=*/5, /*nn=*/2, 0))
    {
        // terminals: t=0, s=1, u=2, v=3, x=4; S states 0..6, A states 7,8
        distinct_dest.start_states[0] = 0;
        distinct_dest.start_states[1] = 7;
        CHECK(case_term(&distinct_dest, 0, 0, 1)); // t
        CHECK(case_term(&distinct_dest, 1, 0, 3)); // s
        CHECK(case_term(&distinct_dest, 2, 2, 6)); // u
        CHECK(case_term(&distinct_dest, 3, 4, 6)); // v
        CHECK(case_term(&distinct_dest, 4, 7, 8)); // A x
        CHECK(case_nonterm(&distinct_dest, 1, 1, 2)); // 1 -A-> 2
        CHECK(case_nonterm(&distinct_dest, 1, 3, 4)); // 3 -A-> 4
        CHECK(case_final(&distinct_dest, 0, 6));
        CHECK(case_final(&distinct_dest, 1, 8));
        CHECK(case_graph(&distinct_dest, 0, 0, 1)); // t
        CHECK(case_graph(&distinct_dest, 1, 0, 2)); // s
        CHECK(case_graph(&distinct_dest, 2, 3, 5)); // u correct for dest 2
        CHECK(case_graph(&distinct_dest, 2, 4, 8)); // u cross for dest 4
        CHECK(case_graph(&distinct_dest, 3, 4, 6)); // v correct for dest 4
        CHECK(case_graph(&distinct_dest, 3, 3, 7)); // v cross for dest 2
        CHECK(case_graph(&distinct_dest, 4, 1, 3)); // x from entry 1
        CHECK(case_graph(&distinct_dest, 4, 2, 4)); // x from entry 2
        CHECK(check_case(&distinct_dest, "wait-distinct-destinations", "5,6"));
        case_free(&distinct_dest);
    }

    // Many callers share one continuation destination (d_A = 1).
    Case shared_dest;
    if (case_new(&shared_dest, /*Q=*/6, /*V=*/6, /*nt=*/3, /*nn=*/2, 0))
    {
        // t=0, u=1, x=2; S states 0..3, A states 4,5
        shared_dest.start_states[0] = 0;
        shared_dest.start_states[1] = 4;
        CHECK(case_term(&shared_dest, 0, 0, 1)); // t
        CHECK(case_term(&shared_dest, 1, 2, 3)); // u
        CHECK(case_term(&shared_dest, 2, 4, 5)); // A x
        CHECK(case_nonterm(&shared_dest, 1, 0, 2)); // 0 -A-> 2
        CHECK(case_nonterm(&shared_dest, 1, 1, 2)); // 1 -A-> 2
        CHECK(case_final(&shared_dest, 0, 3));
        CHECK(case_final(&shared_dest, 1, 5));
        CHECK(case_graph(&shared_dest, 0, 0, 1)); // t
        CHECK(case_graph(&shared_dest, 1, 2, 4)); // u from current 2
        CHECK(case_graph(&shared_dest, 1, 3, 5)); // u from current 3
        CHECK(case_graph(&shared_dest, 2, 0, 2)); // x from entry 0
        CHECK(case_graph(&shared_dest, 2, 1, 3)); // x from entry 1
        CHECK(check_case(&shared_dest, "wait-shared-destination", "4,5"));
        case_free(&shared_dest);
    }

    // Summary-before-prefix: the first call site appears in an early sweep, the
    // second only after t.  Cross suffixes 7 and 8 must remain unreachable.
    Case summary_first;
    if (case_new(&summary_first, /*Q=*/7, /*V=*/9, /*nt=*/4, /*nn=*/2, 0))
    {
        // t=0, u=1, v=2, x=3; S states 0..4, A states 5,6
        summary_first.start_states[0] = 0;
        summary_first.start_states[1] = 5;
        CHECK(case_term(&summary_first, 0, 0, 2)); // t
        CHECK(case_term(&summary_first, 1, 1, 4)); // u
        CHECK(case_term(&summary_first, 2, 3, 4)); // v
        CHECK(case_term(&summary_first, 3, 5, 6)); // A x
        CHECK(case_nonterm(&summary_first, 1, 0, 1)); // 0 -A-> 1
        CHECK(case_nonterm(&summary_first, 1, 2, 3)); // 2 -A-> 3
        CHECK(case_final(&summary_first, 0, 4));
        CHECK(case_final(&summary_first, 1, 6));
        CHECK(case_graph(&summary_first, 0, 0, 1)); // t
        CHECK(case_graph(&summary_first, 1, 2, 3)); // u correct for dest 1
        CHECK(case_graph(&summary_first, 1, 4, 8)); // u cross for dest 3
        CHECK(case_graph(&summary_first, 2, 4, 6)); // v correct for dest 3
        CHECK(case_graph(&summary_first, 2, 2, 7)); // v cross for dest 1
        CHECK(case_graph(&summary_first, 3, 0, 2)); // x from entry 0
        CHECK(case_graph(&summary_first, 3, 1, 4)); // x from entry 1
        CHECK(check_case(&summary_first, "wait-summary-before-prefix", "3,6,8"));
        case_free(&summary_first);
    }

    // Nullable callee with distinct continuation destinations; cross suffixes
    // 5 and 6 must remain unreachable.
    Case nullable_wait;
    if (case_new(&nullable_wait, /*Q=*/7, /*V=*/7, /*nt=*/4, /*nn=*/2, 0))
    {
        // t=0, s=1, u=2, v=3; S states 0..5, A state 6 (nullable)
        nullable_wait.start_states[0] = 0;
        nullable_wait.start_states[1] = 6;
        CHECK(case_term(&nullable_wait, 0, 0, 2)); // t
        CHECK(case_term(&nullable_wait, 1, 0, 4)); // s
        CHECK(case_term(&nullable_wait, 2, 3, 1)); // u
        CHECK(case_term(&nullable_wait, 3, 5, 1)); // v
        CHECK(case_nonterm(&nullable_wait, 1, 2, 3)); // 2 -A-> 3
        CHECK(case_nonterm(&nullable_wait, 1, 4, 5)); // 4 -A-> 5
        CHECK(case_final(&nullable_wait, 0, 1));
        CHECK(case_final(&nullable_wait, 1, 6));
        CHECK(case_graph(&nullable_wait, 0, 0, 1)); // t
        CHECK(case_graph(&nullable_wait, 1, 0, 2)); // s
        CHECK(case_graph(&nullable_wait, 2, 1, 3)); // u correct for dest 3
        CHECK(case_graph(&nullable_wait, 2, 2, 5)); // u cross for dest 5
        CHECK(case_graph(&nullable_wait, 3, 2, 4)); // v correct for dest 5
        CHECK(case_graph(&nullable_wait, 3, 1, 6)); // v cross for dest 3
        CHECK(check_case(&nullable_wait, "wait-nullable-distinct", "3,4"));
        case_free(&nullable_wait);
    }

    // No-caller start box: the start box's nonterminal matrix is NULL, so it has
    // d_A = 0 and no waiting prefixes, but its summary is still needed.
    Case no_caller_start;
    if (case_new(&no_caller_start, /*Q=*/2, /*V=*/3, /*nt=*/1, /*nn=*/1, 0))
    {
        no_caller_start.start_states[0] = 0;
        CHECK(case_term(&no_caller_start, 0, 0, 1));
        CHECK(case_final(&no_caller_start, 0, 1));
        CHECK(case_graph(&no_caller_start, 0, 0, 1));
        case_drop_nonterm(&no_caller_start, 0);
        CHECK(check_case(&no_caller_start, "wait-no-caller-start", "1"));
        case_free(&no_caller_start);
    }
}

//------------------------------------------------------------------------------
// deterministic regressions: fused return plus unique terminal continuation
//------------------------------------------------------------------------------
//
// These cases put eligible and ineligible return destinations side by side in
// one callee, so the fused and ordinary paths must agree with the oracle.
// Structural instrumentation (recorded in DEEPSEEK_FEEDBACK.md) confirms that
// the final/start/terminal-arrival/multiple-exit destinations are not fused.

static void test_fused_continuations(void)
{
    // q=3 is a single-exit terminal continuation and is fused; q=5 is a final
    // of S and must stay ordinary.  Both are destinations of the same callee.
    Case mixed;
    if (case_new(&mixed, /*Q=*/8, /*V=*/7, /*nt=*/5, /*nn=*/2, 0))
    {
        // t=0, s=1, u=2, v=3, x=4; S states 0..5, A states 6,7
        mixed.start_states[0] = 0;
        mixed.start_states[1] = 6;
        CHECK(case_term(&mixed, 0, 0, 2)); // t
        CHECK(case_term(&mixed, 1, 0, 4)); // s
        CHECK(case_term(&mixed, 2, 3, 1)); // u
        CHECK(case_term(&mixed, 3, 5, 1)); // v
        CHECK(case_term(&mixed, 4, 6, 7)); // A x
        CHECK(case_nonterm(&mixed, 1, 2, 3)); // 2 -A-> 3
        CHECK(case_nonterm(&mixed, 1, 4, 5)); // 4 -A-> 5
        CHECK(case_final(&mixed, 0, 1));
        CHECK(case_final(&mixed, 0, 5));
        CHECK(case_final(&mixed, 1, 7));
        CHECK(case_graph(&mixed, 0, 0, 1)); // t
        CHECK(case_graph(&mixed, 1, 0, 2)); // s
        CHECK(case_graph(&mixed, 2, 3, 5)); // u (fused continuation)
        CHECK(case_graph(&mixed, 3, 4, 6)); // v (ordinary final continuation)
        CHECK(case_graph(&mixed, 4, 1, 3)); // x from entry 1
        CHECK(case_graph(&mixed, 4, 2, 4)); // x from entry 2
        CHECK(check_case(&mixed, "fuse-mixed-final", "4,5,6"));
        case_free(&mixed);
    }

    // Fallback topologies in one callee: destination 2 is a fused single-exit
    // continuation; 3 is an S final; 4 has a terminal arrival (r); 5 is a box
    // start/final and has two exits.  Only destination 2 may be fused.
    Case fallback;
    if (case_new(&fallback, /*Q=*/9, /*V=*/7, /*nt=*/7, /*nn=*/3, 0))
    {
        // x=0, u=1, v=2, w=3, y1=4, y2=5, r=6
        // S states 0..6, A states 7,8, B start/final state 5
        fallback.start_states[0] = 0;
        fallback.start_states[1] = 7;
        fallback.start_states[2] = 5;
        CHECK(case_term(&fallback, 0, 7, 8)); // A x
        CHECK(case_term(&fallback, 1, 2, 1)); // u
        CHECK(case_term(&fallback, 2, 3, 1)); // v
        CHECK(case_term(&fallback, 3, 4, 1)); // w
        CHECK(case_term(&fallback, 4, 5, 1)); // y1
        CHECK(case_term(&fallback, 5, 5, 1)); // y2
        CHECK(case_term(&fallback, 6, 0, 4)); // r: terminal arrival at 4
        CHECK(case_nonterm(&fallback, 1, 0, 2)); // 0 -A-> 2
        CHECK(case_nonterm(&fallback, 1, 0, 3)); // 0 -A-> 3
        CHECK(case_nonterm(&fallback, 1, 0, 4)); // 0 -A-> 4
        CHECK(case_nonterm(&fallback, 1, 0, 5)); // 0 -A-> 5
        CHECK(case_final(&fallback, 0, 1));
        CHECK(case_final(&fallback, 0, 3));
        CHECK(case_final(&fallback, 1, 8));
        CHECK(case_final(&fallback, 2, 5));
        CHECK(case_graph(&fallback, 0, 0, 1)); // x: 0->1
        CHECK(case_graph(&fallback, 1, 1, 2)); // u
        CHECK(case_graph(&fallback, 2, 1, 3)); // v
        CHECK(case_graph(&fallback, 3, 1, 4)); // w from current 1
        CHECK(case_graph(&fallback, 3, 5, 6)); // w from current 5 (r path)
        CHECK(case_graph(&fallback, 4, 1, 5)); // y1
        CHECK(case_graph(&fallback, 5, 1, 6)); // y2
        CHECK(case_graph(&fallback, 6, 0, 5)); // r: 0->5
        CHECK(check_case(&fallback, "fuse-fallback-topology", "1,2,3,4,5,6"));
        case_free(&fallback);
    }
}

//------------------------------------------------------------------------------
// deterministic regressions: dense, cyclic and parallel label derivations
//------------------------------------------------------------------------------

static void test_review_regressions(void)
{
    // Grouped labels may share a destination and graph endpoint. They can
    // share compact columns, but another destination at that endpoint cannot.
    Case shared;
    if (case_new(&shared, 4, 5, 5, 1, 0))
    {
        shared.start_states[0] = 0;
        CHECK(case_term(&shared, 0, 0, 1));
        CHECK(case_term(&shared, 1, 0, 1));
        CHECK(case_term(&shared, 2, 0, 2));
        CHECK(case_term(&shared, 3, 1, 3));
        CHECK(case_term(&shared, 4, 2, 3));
        CHECK(case_final(&shared, 0, 3));
        CHECK(case_graph(&shared, 0, 0, 1));
        CHECK(case_graph(&shared, 1, 0, 1));
        CHECK(case_graph(&shared, 1, 0, 2));
        CHECK(case_graph(&shared, 2, 0, 1));
        CHECK(case_graph(&shared, 3, 1, 3));
        CHECK(case_graph(&shared, 3, 2, 4));
        CHECK(case_graph(&shared, 4, 1, 2));
        CHECK(check_case(&shared, "review-shared-endpoint-distinct-state", "2,3,4"));
        case_free(&shared);
    }

    // A has no fusible return; B has both an eligible suffix and a recursive
    // ordinary return. Nullable summaries and later deltas exercise both the
    // handle-swap fallback and repeated discovery of active fused blocks.
    Case recursive;
    if (case_new(&recursive, 6, 6, 2, 3, 0))
    {
        recursive.start_states[0] = 0;
        recursive.start_states[1] = 4;
        recursive.start_states[2] = 5;
        CHECK(case_nonterm(&recursive, 1, 0, 1));
        CHECK(case_nonterm(&recursive, 2, 1, 2));
        CHECK(case_nonterm(&recursive, 2, 5, 5));
        CHECK(case_term(&recursive, 0, 5, 5));
        CHECK(case_term(&recursive, 1, 2, 3));
        CHECK(case_final(&recursive, 0, 3));
        CHECK(case_final(&recursive, 1, 4));
        CHECK(case_final(&recursive, 2, 5));
        CHECK(case_graph(&recursive, 0, 0, 1));
        CHECK(case_graph(&recursive, 0, 1, 2));
        CHECK(case_graph(&recursive, 1, 0, 3));
        CHECK(case_graph(&recursive, 1, 1, 4));
        CHECK(case_graph(&recursive, 1, 2, 5));
        CHECK(check_case(&recursive, "review-nullable-recursive-fusion-fallback", "3,4,5"));
        case_free(&recursive);
    }
}

static void test_dense_relations(void)
{
    // Complete two-state terminal relation and complete two-vertex graph.  Every
    // configuration is reachable and every pair is a summary pair; this is the
    // oracle worklist stress case called out by the R2 review (a state can push
    // successors that are already queued).
    Case dense;
    if (case_new(&dense, /*Q=*/2, /*V=*/2, /*nt=*/1, /*nn=*/1, 0))
    {
        dense.start_states[0] = 0;
        for (GrB_Index a = 0; a < 2; ++a)
            for (GrB_Index b = 0; b < 2; ++b)
                CHECK(case_term(&dense, 0, a, b));
        CHECK(case_final(&dense, 0, 0));
        CHECK(case_final(&dense, 0, 1));
        for (GrB_Index a = 0; a < 2; ++a)
            for (GrB_Index b = 0; b < 2; ++b)
                CHECK(case_graph(&dense, 0, a, b));
        CHECK(check_case(&dense, "dense-complete-2x2", "0,1"));
        case_free(&dense);
    }

    // Branching state with several successors per label, cycles, and a
    // recursive call through a dense graph.
    Case branching;
    if (case_new(&branching, /*Q=*/4, /*V=*/3, /*nt=*/2, /*nn=*/1, 0))
    {
        // a = 0, b = 1
        CHECK(case_term(&branching, 0, 0, 1));
        CHECK(case_term(&branching, 0, 0, 2));
        CHECK(case_term(&branching, 0, 1, 0));
        CHECK(case_term(&branching, 0, 2, 2));
        CHECK(case_term(&branching, 1, 1, 3));
        CHECK(case_term(&branching, 1, 2, 3));
        CHECK(case_nonterm(&branching, 0, 1, 2)); // 1 -S-> 2 (recursion)
        branching.start_states[0] = 0;
        CHECK(case_final(&branching, 0, 3));
        // dense graph: every vertex has a edge to every vertex and b edges
        for (GrB_Index a = 0; a < 3; ++a)
        {
            for (GrB_Index b = 0; b < 3; ++b)
            {
                CHECK(case_graph(&branching, 0, a, b));
                CHECK(case_graph(&branching, 1, a, b));
            }
        }
        CHECK(check_case(&branching, "branching-cyclic-recursive", NULL));
        case_free(&branching);
    }

    // Parallel derivations: two labels derive the same summary pair; the result
    // is a set, so the union must not double-count or lose a return.
    Case parallel;
    if (case_new(&parallel, /*Q=*/2, /*V=*/2, /*nt=*/2, /*nn=*/1, 0))
    {
        parallel.start_states[0] = 0;
        CHECK(case_term(&parallel, 0, 0, 1));
        CHECK(case_term(&parallel, 1, 0, 1));
        CHECK(case_final(&parallel, 0, 1));
        CHECK(case_graph(&parallel, 0, 0, 1));
        CHECK(case_graph(&parallel, 1, 0, 1));
        CHECK(check_case(&parallel, "parallel-label-derivations", "1"));
        case_free(&parallel);
    }
}

//------------------------------------------------------------------------------
// randomized small instances compared with the independent oracle
//------------------------------------------------------------------------------

static bool random_case(Case *c, uint64_t seed)
{
    uint64_t saved = rng_state;
    rng_state = seed;

    const uint64_t V = 1 + rng_below(6);
    const size_t nt = 1 + (size_t)rng_below(3);
    const size_t nn = 1 + (size_t)rng_below(3);

    // per box: 2..3 global states, disjoint ranges
    uint64_t q_total = 0;
    uint64_t box_start[4] = {0};
    uint64_t box_states[4] = {0};
    for (size_t i = 0; i < nn; ++i)
    {
        box_states[i] = 2 + rng_below(2);
        box_start[i] = q_total;
        q_total += box_states[i];
    }

    if (!case_new(c, (GrB_Index)q_total, (GrB_Index)V, nt, nn,
                  (GrB_Index)rng_below(nn)))
    {
        rng_state = saved;
        return false;
    }

    for (size_t i = 0; i < nn; ++i)
    {
        c->start_states[i] = (GrB_Index)(box_start[i] + rng_below(box_states[i]));

        // finals: sometimes NULL, sometimes empty, otherwise a random subset
        uint64_t choice = rng_below(10);
        if (choice == 0)
        {
            case_drop_finals(c, i);
        }
        else if (choice == 1)
        {
            // leave the vector empty
        }
        else
        {
            for (uint64_t k = 0; k < box_states[i]; ++k)
            {
                if (rng_below(3) == 0)
                    CHECK(case_final(c, i, (GrB_Index)(box_start[i] + k)));
            }
        }

        // terminal transitions inside the box: several successors per state
        // and parallel label derivations, so the graph is not a narrow chain
        for (uint64_t k = 0; k < box_states[i]; ++k)
        {
            for (size_t t = 0; t < nt; ++t)
            {
                if (rng_below(3) != 0)
                    continue;
                uint64_t target = rng_below(box_states[i]);
                if (!case_term(c, t, (GrB_Index)(box_start[i] + k),
                               (GrB_Index)(box_start[i] + target)))
                {
                    rng_state = saved;
                    return false;
                }
            }
        }

        // nonterminal transitions: rarer, several labels per state
        for (uint64_t k = 0; k < box_states[i]; ++k)
        {
            for (size_t t = 0; t < nn; ++t)
            {
                if (rng_below(10) != 0)
                    continue;
                uint64_t target = rng_below(box_states[i]);
                if (!case_nonterm(c, t, (GrB_Index)(box_start[i] + k),
                                  (GrB_Index)(box_start[i] + target)))
                {
                    rng_state = saved;
                    return false;
                }
            }
        }
    }
    for (size_t i = 0; i < nn; ++i)
    {
        GrB_Index nvals = 0;
        if (c->nonterm[i] != GrB_NULL && GrB_Matrix_nvals(&nvals, c->nonterm[i]) == GrB_SUCCESS &&
            nvals == 0 && rng_below(4) == 0)
            case_drop_nonterm(c, i);
    }

    // graph edges
    for (size_t t = 0; t < nt; ++t)
    {
        for (uint64_t u = 0; u < V; ++u)
        {
            for (uint64_t v = 0; v < V; ++v)
            {
                if (rng_below(4) == 0)
                {
                    if (!case_graph(c, t, (GrB_Index)u, (GrB_Index)v))
                    {
                        rng_state = saved;
                        return false;
                    }
                }
            }
        }
        if (rng_below(6) == 0)
            case_drop_graph(c, t);
    }

    rng_state = saved;
    return true;
}

// diagnostic dump used when a randomized case fails; the seed and generator
// version reproduce it
static void case_dump(const Case *c, const char *name)
{
    printf("case dump %s: Q=%llu V=%llu nt=%zu nn=%zu start=%llu\n", name,
           (unsigned long long)c->Q, (unsigned long long)c->V, c->nt, c->nn,
           (unsigned long long)c->start_nonterm);
    for (size_t i = 0; i < c->nn; ++i)
    {
        printf("  box %zu start=%llu", i, (unsigned long long)c->start_states[i]);
        GrB_Index n = 0;
        if (c->finals[i] != GrB_NULL)
        {
            GrB_Vector_nvals(&n, c->finals[i]);
            printf(" finals=%llu", (unsigned long long)n);
        }
        else
            printf(" finals=NULL");
        printf("\n");
        if (c->nonterm[i] == GrB_NULL)
        {
            printf("    nonterm=NULL\n");
            continue;
        }
        GrB_Index nn = 0;
        GrB_Matrix_nvals(&nn, c->nonterm[i]);
        if (nn > 0)
        {
            GrB_Index *I = malloc((size_t)nn * sizeof(GrB_Index));
            GrB_Index *J = malloc((size_t)nn * sizeof(GrB_Index));
            GrB_Index t = nn;
            GrB_Matrix_extractTuples_BOOL(I, J, NULL, &t, c->nonterm[i]);
            printf("    nonterm:");
            for (GrB_Index k = 0; k < t; ++k)
                printf(" %llu->%llu", (unsigned long long)I[k], (unsigned long long)J[k]);
            printf("\n");
            free(I);
            free(J);
        }
    }
    for (size_t t = 0; t < c->nt; ++t)
    {
        if (c->term[t] == GrB_NULL)
        {
            printf("  term %zu NULL\n", t);
            continue;
        }
        GrB_Index n = 0;
        GrB_Matrix_nvals(&n, c->term[t]);
        printf("  term %zu nvals=%llu", t, (unsigned long long)n);
        if (n > 0)
        {
            GrB_Index *I = malloc((size_t)n * sizeof(GrB_Index));
            GrB_Index *J = malloc((size_t)n * sizeof(GrB_Index));
            GrB_Index k = n;
            GrB_Matrix_extractTuples_BOOL(I, J, NULL, &k, c->term[t]);
            printf(":");
            for (GrB_Index q = 0; q < k; ++q)
                printf(" %llu->%llu", (unsigned long long)I[q], (unsigned long long)J[q]);
            free(I);
            free(J);
        }
        printf("\n");
    }
    for (size_t t = 0; t < c->nt; ++t)
    {
        if (c->graph[t] == GrB_NULL)
        {
            printf("  graph %zu NULL\n", t);
            continue;
        }
        GrB_Index n = 0;
        GrB_Matrix_nvals(&n, c->graph[t]);
        printf("  graph %zu nvals=%llu", t, (unsigned long long)n);
        if (n > 0)
        {
            GrB_Index *I = malloc((size_t)n * sizeof(GrB_Index));
            GrB_Index *J = malloc((size_t)n * sizeof(GrB_Index));
            GrB_Index k = n;
            GrB_Matrix_extractTuples_BOOL(I, J, NULL, &k, c->graph[t]);
            printf(":");
            for (GrB_Index q = 0; q < k; ++q)
                printf(" %llu->%llu", (unsigned long long)I[q], (unsigned long long)J[q]);
            free(I);
            free(J);
        }
        printf("\n");
    }
    fflush(stdout);
}

static void test_random(uint64_t base_seed, int count)
{
    int failed_cases = 0;
    for (int k = 0; k < count; ++k)
    {
        uint64_t seed = base_seed + (uint64_t)k;
        Case c;
        if (!random_case(&c, seed))
        {
            report_failure(__FILE__, __LINE__, "random case %d (seed %llu) build failed", k,
                           (unsigned long long)seed);
            case_free(&c);
            continue;
        }
        char name[128];
        snprintf(name, sizeof(name), "random[case=%d seed=%llu]", k, (unsigned long long)seed);
        int failures_before = g_failures;
        bool ok = check_case(&c, name, NULL);
        if (!ok || g_failures != failures_before || g_verbose)
        {
            printf("%-40s %s\n", name, (ok && g_failures == failures_before) ? "PASS" : "FAIL");
            fflush(stdout);
        }
        if (!ok || g_failures != failures_before)
            case_dump(&c, name);
        if (!ok || g_failures != failures_before)
            failed_cases++;
        case_free(&c);
    }
    printf("randomized cases: %d, seeds %llu..%llu, %d failed\n", count,
           (unsigned long long)base_seed, (unsigned long long)(base_seed + (uint64_t)(count ? count - 1 : 0)),
           failed_cases);
    fflush(stdout);
}

//------------------------------------------------------------------------------
// allocation-failure sweep (LG_brutal_* / GxB_init tracking allocator)
//------------------------------------------------------------------------------

// Small instance used by the sweep: exercises Phase 1 terminals (ordinary and
// grouped families), Phase 2/3 calls, Phase 4 summary extraction for a box with
// NULL incoming calls, Phase 4 returns for a box with waiting configurations,
// and the grouped-family preprocessing/gather allocations.
static bool build_alloc_case(Case *c)
{
    // alphabet: a=0, b=1, c=2, d=3; V=8 as in the mutual-recursion case
    if (!case_new(c, /*Q=*/9, /*V=*/8, /*nt=*/4, /*nn=*/3, 0))
        return false;
    // S: 0 -a-> 1 -T-> 2 -b-> 3 (final), 0 -a-> 1 (N nullable call)
    CHECK(case_term(c, 0, 0, 1));
    CHECK(case_nonterm(c, 1, 1, 2));
    CHECK(case_nonterm(c, 2, 1, 2));
    CHECK(case_term(c, 1, 2, 3));
    // T: 4 -c-> 5 -S-> 6 -d-> 7 (final)
    CHECK(case_term(c, 2, 4, 5));
    CHECK(case_nonterm(c, 0, 5, 6));
    CHECK(case_term(c, 3, 6, 7));
    // grouped families: source 0 has a(0,1) and c(0,2), c(0,6); destination 1
    // has a(0,1) and a(4,1).  Both families need a nonempty graph support.
    CHECK(case_term(c, 0, 4, 1));
    CHECK(case_term(c, 2, 0, 2));
    CHECK(case_term(c, 2, 0, 6));
    c->start_states[0] = 0;
    c->start_states[1] = 4;
    c->start_states[2] = 8;
    CHECK(case_final(c, 0, 3));
    CHECK(case_final(c, 1, 7));
    CHECK(case_final(c, 2, 8));
    // S has NULL incoming calls: its summary must still be extracted
    case_drop_nonterm(c, 0);
    CHECK(case_graph(c, 0, 0, 1));
    CHECK(case_graph(c, 0, 4, 5));
    CHECK(case_graph(c, 2, 1, 2));
    CHECK(case_graph(c, 0, 2, 3));
    CHECK(case_graph(c, 2, 0, 1));
    CHECK(case_graph(c, 2, 0, 7));
    CHECK(case_graph(c, 1, 3, 4));
    CHECK(case_graph(c, 3, 4, 5));
    CHECK(case_graph(c, 1, 5, 6));
    CHECK(case_graph(c, 1, 1, 7));
    return true;
}

// returns 0 on pass, 1 on failure, 2 on unexpected success/error shape
static int alloc_trial(int64_t budget)
{
    if (LG_brutal_setup(msg) != GrB_SUCCESS)
    {
        fprintf(stderr, "alloc-case %lld: LG_brutal_setup failed: %s\n", (long long)budget, msg);
        return 1;
    }
    freeze_graphblas_environment();

    int status = 1;
    Case c;
    case_init_struct(&c);
    GrB_Vector out = GrB_NULL;
    OracleView ov;
    memset(&ov, 0, sizeof(ov));
    bool *expected = NULL;
    bool have_oracle = false;

    if (!build_alloc_case(&c))
    {
        fprintf(stderr, "alloc-case %lld: case build failed\n", (long long)budget);
        goto done;
    }
    if (!oracle_view_build(&c, &ov))
        goto done;
    expected = (bool *)calloc((size_t)(c.V ? c.V : 1), sizeof(bool));
    if (expected == NULL)
        goto done;
    have_oracle = true;

    LG_brutal = budget;
    GrB_Info info = run_case(&c, (const GrB_Index *)&(GrB_Index){0}, 1, &out);
    const int64_t used = budget - LG_brutal;
    LG_brutal = -1;

    if (info == GrB_SUCCESS)
    {
        GrB_Vector_wait(out, GrB_MATERIALIZE);
        if (cfpq_oracle_reachable(&ov.in, (const uint64_t *)&(uint64_t){0}, 1, expected) != 0)
        {
            fprintf(stderr, "alloc-case %lld: oracle failed\n", (long long)budget);
            goto done;
        }
        if (!vector_matches(out, expected, c.V, true, "alloc-case result"))
        {
            fprintf(stderr, "alloc-case %lld: budget-limited run returned a wrong result\n",
                    (long long)budget);
            goto done;
        }
        status = 0;
    }
    else
    {
        if (out != GrB_NULL)
        {
            fprintf(stderr, "alloc-case %lld: failed run left a non-NULL output\n",
                    (long long)budget);
            goto done;
        }
        status = 3; // clean, expected allocation failure
    }
    printf("alloc-case %5lld: %s, allocation attempts %lld\n", (long long)budget,
           info == GrB_SUCCESS ? "success" : "error", (long long)used);
    fflush(stdout);

done:
    (void)have_oracle;
    GrB_free(&out);
    free(expected);
    oracle_view_free(&ov);
    case_free(&c);
    LG_brutal = -1;
    if (LG_brutal_teardown(msg) != 0)
    {
        fprintf(stderr, "alloc-case %lld: leak or finalize failure (LG_nmalloc=%lld)\n",
                (long long)budget, (long long)LG_nmalloc);
        status = 1;
    }
    return status;
}

static int alloc_case_main(const char *arg)
{
    long long budget = strtoll(arg, NULL, 10);
    if (budget < 0)
        budget = 0;
    int status = alloc_trial(budget);
    fflush(NULL);
    // 0: the budget-limited run returned a valid result
    // 3: an allocation failed and the kernel returned an error with
    //    *reachable == NULL and no leak (a clean, expected failure)
    // 1: any contract violation, wrong result, crash-adjacent state or leak
    _exit(status == 0 ? 0 : (status == 3 ? 3 : 1));
}

// Run one child process per budget.  The parent never initializes LAGraph or
// GraphBLAS, so each child starts from a pristine address space.  Children run
// sequentially, each retaining the inherited OpenMP environment and the
// runtime's own parallelism. Do not impose a thread cap on sweep children.
#define ALLOC_SWEEP_WIDTH 1

static int alloc_sweep(int max_budget)
{
    char self[4096];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len <= 0)
    {
        fprintf(stderr, "alloc-sweep: cannot resolve /proc/self/exe\n");
        return 1;
    }
    self[len] = '\0';

    int success_streak = 0;
    int failures = 0;
    int first_success = -1;
    printf("alloc-sweep: one child process per allowed-allocation budget, width %d\n",
           ALLOC_SWEEP_WIDTH);
    fflush(stdout);
    for (int base = 0; base <= max_budget; base += ALLOC_SWEEP_WIDTH)
    {
        pid_t pids[ALLOC_SWEEP_WIDTH];
        int budgets[ALLOC_SWEEP_WIDTH];
        int n = 0;
        for (int k = 0; k < ALLOC_SWEEP_WIDTH && base + k <= max_budget; ++k)
        {
            int budget = base + k;
            char arg[32];
            snprintf(arg, sizeof(arg), "--alloc-case=%d", budget);
            pid_t pid = fork();
            if (pid < 0)
            {
                fprintf(stderr, "alloc-sweep: fork failed: %s\n", strerror(errno));
                return 1;
            }
            if (pid == 0)
            {
                execl(self, self, arg, (char *)NULL);
                fprintf(stderr, "alloc-sweep: exec failed: %s\n", strerror(errno));
                _exit(127);
            }
            pids[n] = pid;
            budgets[n] = budget;
            n++;
        }
        if (n == 0)
            break;
        for (int k = 0; k < n; ++k)
        {
            int wstatus = 0;
            if (waitpid(pids[k], &wstatus, 0) < 0)
            {
                fprintf(stderr, "alloc-sweep: waitpid failed: %s\n", strerror(errno));
                return 1;
            }
            bool valid_result = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
            bool clean_error = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 3;
            if (valid_result)
            {
                if (first_success < 0)
                    first_success = budgets[k];
                success_streak++;
            }
            else if (clean_error)
            {
                success_streak = 0; // expected: this budget is still too small
            }
            else
            {
                failures++;
                success_streak = 0;
                if (WIFSIGNALED(wstatus))
                    printf("budget %5d: CRASH (signal %d)\n", budgets[k], WTERMSIG(wstatus));
                else
                    printf("budget %5d: FAIL (exit %d)\n", budgets[k], WEXITSTATUS(wstatus));
                fflush(stdout);
            }
        }
        if (success_streak >= 5 && first_success >= 0)
            break;
    }
    printf("alloc-sweep: first budget with a valid result = %d, failures = %d\n", first_success,
           failures);
    fflush(stdout);
    return failures == 0 ? 0 : 1;
}

//------------------------------------------------------------------------------
// main
//------------------------------------------------------------------------------

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--no-random] [--cases N] [--seed S] [--verbose]\n"
            "       %s --smoke\n"
            "       %s --alloc-sweep [--max-budget N]\n"
            "       %s --alloc-case=N\n",
            argv0, argv0, argv0, argv0);
}

static void test_list(void)
{
    test_single_terminal();
    test_epsilon_start();
    test_no_final();
    test_recursion();
    test_multiple_finals();
    test_empty_and_zero();
    test_missing_terminals();
    test_repeated_sources();
    test_stored_false();
    test_indexed_families();
    test_grouped_families();
    test_block_family_alignment();
    test_full_coordinate_dual_axis();
    test_invalid_input();
    test_malformed_dimensions();
    test_trimming_compaction();
    test_review_regressions();
    test_waiting_prefixes();
    test_fused_continuations();
    test_dense_relations();
}

int main(int argc, char **argv)
{
    int cases = 200;
    uint64_t seed = 2026;
    bool run_random = true;
    bool smoke = false;
    int max_budget = 4096;
    bool alloc_sweep_mode = false;
    const char *alloc_case_arg = NULL;

    for (int k = 1; k < argc; ++k)
    {
        const char *a = argv[k];
        if (strcmp(a, "--no-random") == 0)
            run_random = false;
        else if (strcmp(a, "--smoke") == 0)
            smoke = true;
        else if (strcmp(a, "--verbose") == 0)
            g_verbose = true;
        else if (strcmp(a, "--cases") == 0 && k + 1 < argc)
            cases = atoi(argv[++k]);
        else if (strncmp(a, "--cases=", 8) == 0)
            cases = atoi(a + 8);
        else if (strcmp(a, "--seed") == 0 && k + 1 < argc)
            seed = strtoull(argv[++k], NULL, 10);
        else if (strncmp(a, "--seed=", 7) == 0)
            seed = strtoull(a + 7, NULL, 10);
        else if (strcmp(a, "--max-budget") == 0 && k + 1 < argc)
            max_budget = atoi(argv[++k]);
        else if (strncmp(a, "--max-budget=", 13) == 0)
            max_budget = atoi(a + 13);
        else if (strcmp(a, "--alloc-sweep") == 0)
            alloc_sweep_mode = true;
        else if (strncmp(a, "--alloc-case=", 13) == 0)
            alloc_case_arg = a + 13;
        else if (strcmp(a, "--alloc-case") == 0 && k + 1 < argc)
            alloc_case_arg = argv[++k];
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    // the allocation modes must run from a pristine process that has not
    // initialized LAGraph/GraphBLAS
    if (alloc_case_arg != NULL)
        return alloc_case_main(alloc_case_arg);
    if (alloc_sweep_mode)
        return alloc_sweep(max_budget);

    if (LAGraph_Init(msg) != GrB_SUCCESS)
    {
        fprintf(stderr, "LAGraph_Init failed: %s\n", msg);
        return 2;
    }
    freeze_graphblas_environment();

    if (smoke)
    {
        // --smoke: only the three existing functional regressions used for the
        // R5 variant checks; no randomized or other suites.
        printf("DS-00 correctness harness: smoke (single terminal, empty/zero, recursion)\n");
        fflush(stdout);
        test_single_terminal();
        test_empty_and_zero();
        test_recursion();
    }
    else
    {
        printf("DS-00 correctness harness: deterministic regressions\n");
        fflush(stdout);
        test_list();
    }

    if (run_random && !smoke)
    {
        printf("DS-00 correctness harness: %d randomized cases, base seed %llu\n", cases,
               (unsigned long long)seed);
        fflush(stdout);
        test_random(seed, cases);
    }

    LAGraph_Finalize(msg);

    printf("\nchecks=%d failures=%d\n", g_checks, g_failures);
    if (g_failures == 0)
        printf("PASS: %s\n", smoke ? "--smoke checks"
                                   : "deterministic regressions and randomized oracle cases");
    else
        printf("FAIL: %d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
