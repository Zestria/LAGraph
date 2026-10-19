//------------------------------------------------------------------------------
// test_CFPQ_RSM_blocked.c: tests for LAGraph_CFPQ_RSM_blocked
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
// This file ports the python-graphblas test suite of
// "lacfpq_blocked_with_tests.py" to C.  Each Python grammar is expressed as a
// set of context-free productions and expanded to an RSM by the same
// construction used by the Python reference:
//
//      each production  A -> X1 X2 ... Xn
//
// becomes a chain of fresh states inside box B_A, starting at the (shared)
// start state of B_A.  The last state of the chain is final.  An empty
// right-hand side [] makes the start state itself final (epsilon production).
//
// Terminal and non-terminal symbols are encoded in a production as follows:
//
//      TERM(t)  = t          terminal with label index t (t >= 0)
//      NT(a)    = -a - 1     non-terminal with index a (a >= 0)
//
// The graph is likewise built from a list of (src, label, dst) edges, exactly
// as in the Python tests.
//
//------------------------------------------------------------------------------

#include "LAGraph.h"
#include "LAGraphX.h"
#include "LAGraph_test.h"
#include <GraphBLAS.h>
#include <stddef.h>
#include <stdlib.h>

char msg[LAGRAPH_MSG_LEN];

static RSM *rsm = NULL;
static size_t V = 0;
static size_t n_graph_term = 0;
static GrB_Matrix *graph_term = NULL;
static GrB_Vector result = GrB_NULL;

//==============================================================================
// test setup / teardown
//==============================================================================

static void setup(void)
{
    LAGraph_Init(msg);
}

static void teardown(void)
{
    LAGraph_Finalize(msg);
}

static RSM *alloc_rsm(GrB_Index state_count, GrB_Index term_count, GrB_Index nonterm_count,
                      GrB_Index start_nonterm)
{
    RSM *r = NULL;
    LAGraph_Malloc((void **)&r, 1, sizeof(RSM), msg);
    if (r == NULL)
        return NULL;

    r->state_count = state_count;
    r->terminal_count = term_count;
    r->nonterminal_count = nonterm_count;
    r->start_nonterminal = start_nonterm;

    LAGraph_Calloc((void **)&r->terminal_matrices, term_count, sizeof(GrB_Matrix), msg);
    LAGraph_Calloc((void **)&r->nonterminal_matrices, nonterm_count, sizeof(GrB_Matrix), msg);
    LAGraph_Calloc((void **)&r->start_states, nonterm_count, sizeof(GrB_Index), msg);
    LAGraph_Calloc((void **)&r->final_states, nonterm_count, sizeof(GrB_Vector), msg);

    for (size_t i = 0; i < term_count; ++i)
        GrB_Matrix_new(&r->terminal_matrices[i], GrB_BOOL, state_count, state_count);

    for (size_t i = 0; i < nonterm_count; ++i)
    {
        GrB_Matrix_new(&r->nonterminal_matrices[i], GrB_BOOL, state_count, state_count);
        GrB_Vector_new(&r->final_states[i], GrB_BOOL, state_count);
    }

    return r;
}

static void free_rsm(void)
{
    if (rsm == NULL)
        return;

    if (rsm->terminal_matrices != NULL)
    {
        for (size_t i = 0; i < rsm->terminal_count; ++i)
            GrB_free(&rsm->terminal_matrices[i]);
        LAGraph_Free((void **)&rsm->terminal_matrices, msg);
    }
    if (rsm->nonterminal_matrices != NULL)
    {
        for (size_t i = 0; i < rsm->nonterminal_count; ++i)
            GrB_free(&rsm->nonterminal_matrices[i]);
        LAGraph_Free((void **)&rsm->nonterminal_matrices, msg);
    }

    if (rsm->final_states != NULL)
    {
        for (size_t i = 0; i < rsm->nonterminal_count; ++i)
        {
            GrB_free(&rsm->final_states[i]);
        }
        LAGraph_Free((void **)&rsm->final_states, msg);
    }

    LAGraph_Free((void **)&rsm->start_states, msg);
    LAGraph_Free((void **)&rsm, msg);
}

static void free_graph(void)
{
    if (graph_term == NULL)
        return;
    for (size_t i = 0; i < n_graph_term; ++i)
        GrB_free(&graph_term[i]);
    LAGraph_Free((void **)&graph_term, msg);
    n_graph_term = 0;
    V = 0;
}

static void free_workspace(void)
{
    GrB_free(&result);
    free_rsm();
    free_graph();
}

static void check_reachable(GrB_Index *expected, GrB_Index n_expected, const char *test_name)
{
    GrB_Index nvals = 0;
    OK(GrB_Vector_nvals(&nvals, result));

    bool *got = (bool *)calloc(V, sizeof(bool));
    TEST_CHECK(got != NULL);

    GrB_Index *idx = GrB_NULL;
    bool *val = GrB_NULL;

    if (nvals > 0)
    {
        idx = (GrB_Index *)malloc(nvals * sizeof(GrB_Index));
        val = (bool *)malloc(nvals * sizeof(bool));
        if (!TEST_CHECK(idx != NULL && val != NULL))
        {
            free(got);
            free(idx);
            free(val);
            return;
        }
        OK(GrB_Vector_extractTuples_BOOL(idx, val, &nvals, result));
        for (GrB_Index k = 0; k < nvals; ++k)
            if (val[k])
                got[idx[k]] = true;
        free(idx);
        free(val);
    }

    // every expected vertex must be reachable
    for (GrB_Index k = 0; k < n_expected; ++k)
    {
        TEST_CHECK(got[expected[k]]);
        TEST_MSG("%s: vertex %llu should be reachable", test_name, (unsigned long long)expected[k]);
    }

    GrB_Index actual_count = 0;
    for (GrB_Index v = 0; v < V; ++v)
        if (got[v])
            actual_count++;

    TEST_CHECK(actual_count == n_expected);
    TEST_MSG("%s: expected %llu reachable vertices, got %llu", test_name,
             (unsigned long long)n_expected, (unsigned long long)actual_count);

    free(got);
}

//==============================================================================
// RSM / graph builders
//==============================================================================

#define TERM(sym) ((int)(sym))
#define NT(sym) (-((int)(sym)) - 1)

// a single production right-hand side
typedef struct
{
    const int *rhs; // symbols; terminal >= 0, non-terminal < 0
    size_t len;     // number of symbols (0 for epsilon)
} Prod;

// a single graph edge
typedef struct
{
    GrB_Index src;
    int label; // terminal label index
    GrB_Index dst;
} Edge;

// a single RSM transition
typedef struct
{
    GrB_Index src;
    int sym; // terminal >= 0, non-terminal < 0
    GrB_Index dst;
} Trans;

// add one transition, encoded with TERM/NT, to the RSM under construction
static void add_transition(int sym, GrB_Index src, GrB_Index dst)
{
    if (sym >= 0)
    {
        OK(GrB_Matrix_setElement_BOOL(rsm->terminal_matrices[sym], true, src, dst));
    }
    else
    {
        GrB_Index nt = (GrB_Index)(-sym - 1);
        OK(GrB_Matrix_setElement_BOOL(rsm->nonterminal_matrices[nt], true, src, dst));
    }
}

// build an RSM from a context-free grammar, using the same state layout as the
// Python build_rsm() function: each non-terminal gets one start state, and
// each production gets its own chain of fresh states starting at that state.
// prods[nt] is the list of productions of non-terminal nt.
static void init_rsm_from_grammar(size_t num_term, size_t num_nonterm, size_t start_nt,
                                  const Prod *const *prods, const size_t *nprods)
{
    // count the states: one start state plus one state per RHS symbol
    GrB_Index Q = 0;
    for (size_t nt = 0; nt < num_nonterm; ++nt)
    {
        Q += 1;
        for (size_t p = 0; p < nprods[nt]; ++p)
        {
            Q += (GrB_Index)prods[nt][p].len;
        }
    }

    rsm = alloc_rsm(Q, (GrB_Index)num_term, (GrB_Index)num_nonterm, (GrB_Index)start_nt);
    if (rsm == NULL)
        return;

    GrB_Index next_state = 0;
    for (size_t nt = 0; nt < num_nonterm; ++nt)
    {
        GrB_Index start = next_state++;
        rsm->start_states[nt] = start;

        for (size_t p = 0; p < nprods[nt]; ++p)
        {
            const Prod *prod = &prods[nt][p];
            GrB_Index cur = start;
            for (size_t k = 0; k < prod->len; ++k)
            {
                GrB_Index nxt = next_state++;
                add_transition(prod->rhs[k], cur, nxt);
                cur = nxt;
            }
            OK(GrB_Vector_setElement_BOOL(rsm->final_states[nt], true, cur));
        }
    }
}

// build an RSM directly from transitions and final states
static void init_rsm_raw(size_t num_term, size_t num_nonterm, size_t start_nt, GrB_Index Q,
                         const GrB_Index *start_states, const Trans *trans, size_t ntrans,
                         const GrB_Index *const *finals, const size_t *nfinals)
{
    rsm = alloc_rsm(Q, (GrB_Index)num_term, (GrB_Index)num_nonterm, (GrB_Index)start_nt);
    if (rsm == NULL)
        return;

    for (size_t nt = 0; nt < num_nonterm; ++nt)
    {
        rsm->start_states[nt] = start_states[nt];
        for (size_t k = 0; k < nfinals[nt]; ++k)
        {
            OK(GrB_Vector_setElement_BOOL(rsm->final_states[nt], true, finals[nt][k]));
        }
    }

    for (size_t t = 0; t < ntrans; ++t)
    {
        add_transition(trans[t].sym, trans[t].src, trans[t].dst);
    }
}

// build the per-terminal graph matrices from an edge list
static void init_graph(GrB_Index n_vertices, size_t n_labels, const Edge *edges, size_t n_edges)
{
    V = (size_t)n_vertices;
    n_graph_term = n_labels;
    LAGraph_Calloc((void **)&graph_term, n_labels, sizeof(GrB_Matrix), msg);
    for (size_t i = 0; i < n_labels; ++i)
        OK(GrB_Matrix_new(&graph_term[i], GrB_BOOL, V, V));

    for (size_t e = 0; e < n_edges; ++e)
    {
        TEST_CHECK(edges[e].label >= 0 && (size_t)edges[e].label < n_labels);
        OK(GrB_Matrix_setElement_BOOL(graph_term[edges[e].label], true, edges[e].src,
                                       edges[e].dst));
    }
}

//==============================================================================
// grammars
//==============================================================================

// S -> a S b | a b
static const int g_anbn_0[] = {TERM(0), NT(0), TERM(1)};
static const int g_anbn_1[] = {TERM(0), TERM(1)};
static const Prod g_anbn_S[] = {{g_anbn_0, 3}, {g_anbn_1, 2}};

static void init_rsm_anbn(void)
{
    static const Prod *const prods[] = {g_anbn_S};
    static const size_t nprods[] = {2};
    init_rsm_from_grammar(2, 1, 0, prods, nprods);
}

// S -> a S | a
static const int g_right_0[] = {TERM(0), NT(0)};
static const int g_right_1[] = {TERM(0)};
static const Prod g_right_S[] = {{g_right_0, 2}, {g_right_1, 1}};

static void init_rsm_right_recursive(void)
{
    static const Prod *const prods[] = {g_right_S};
    static const size_t nprods[] = {2};
    init_rsm_from_grammar(1, 1, 0, prods, nprods);
}

// S -> S a | epsilon
static const int g_left_0[] = {NT(0), TERM(0)};
static const Prod g_left_S[] = {{g_left_0, 2}, {NULL, 0}};

static void init_rsm_left_recursive(void)
{
    static const Prod *const prods[] = {g_left_S};
    static const size_t nprods[] = {2};
    init_rsm_from_grammar(1, 1, 0, prods, nprods);
}

// S -> a S b | epsilon
static const int g_dyck_0[] = {TERM(0), NT(0), TERM(1)};
static const Prod g_dyck_S[] = {{g_dyck_0, 3}, {NULL, 0}};

static void init_rsm_dyck(void)
{
    static const Prod *const prods[] = {g_dyck_S};
    static const size_t nprods[] = {2};
    init_rsm_from_grammar(2, 1, 0, prods, nprods);
}

// S -> a A ; A -> S b | b
static const int g_mutual_S_0[] = {TERM(0), NT(1)};
static const Prod g_mutual_S[] = {{g_mutual_S_0, 2}};
static const int g_mutual_A_0[] = {NT(0), TERM(1)};
static const int g_mutual_A_1[] = {TERM(1)};
static const Prod g_mutual_A[] = {{g_mutual_A_0, 2}, {g_mutual_A_1, 1}};

static void init_rsm_mutual_recursion(void)
{
    static const Prod *const prods[] = {g_mutual_S, g_mutual_A};
    static const size_t nprods[] = {1, 2};
    init_rsm_from_grammar(2, 2, 0, prods, nprods);
}

// S -> epsilon
static const Prod g_eps_S[] = {{NULL, 0}};

static void init_rsm_epsilon(void)
{
    static const Prod *const prods[] = {g_eps_S};
    static const size_t nprods[] = {1};
    init_rsm_from_grammar(1, 1, 0, prods, nprods);
}

// S -> a S a | epsilon
static const int g_even_0[] = {TERM(0), NT(0), TERM(0)};
static const Prod g_even_S[] = {{g_even_0, 3}, {NULL, 0}};

static void init_rsm_even_palindrome(void)
{
    static const Prod *const prods[] = {g_even_S};
    static const size_t nprods[] = {2};
    init_rsm_from_grammar(1, 1, 0, prods, nprods);
}

// S -> S S | a | epsilon
static const int g_ambig_0[] = {NT(0), NT(0)};
static const int g_ambig_1[] = {TERM(0)};
static const Prod g_ambig_S[] = {{g_ambig_0, 2}, {g_ambig_1, 1}, {NULL, 0}};

static void init_rsm_ambiguous(void)
{
    static const Prod *const prods[] = {g_ambig_S};
    static const size_t nprods[] = {3};
    init_rsm_from_grammar(1, 1, 0, prods, nprods);
}

// S -> A B ; A -> a ; B -> b
static const int g_three_S_0[] = {NT(1), NT(2)};
static const Prod g_three_S[] = {{g_three_S_0, 2}};
static const int g_three_A_0[] = {TERM(0)};
static const Prod g_three_A[] = {{g_three_A_0, 1}};
static const int g_three_B_0[] = {TERM(1)};
static const Prod g_three_B[] = {{g_three_B_0, 1}};

static void init_rsm_three_nonterminals(void)
{
    static const Prod *const prods[] = {g_three_S, g_three_A, g_three_B};
    static const size_t nprods[] = {1, 1, 1};
    init_rsm_from_grammar(2, 3, 0, prods, nprods);
}

// S -> M b | a b ; M -> a S   (CNF of S -> a S b | a b)
static const int g_cnf_S_0[] = {NT(1), TERM(1)};
static const int g_cnf_S_1[] = {TERM(0), TERM(1)};
static const Prod g_cnf_S[] = {{g_cnf_S_0, 2}, {g_cnf_S_1, 2}};
static const int g_cnf_M_0[] = {TERM(0), NT(0)};
static const Prod g_cnf_M[] = {{g_cnf_M_0, 2}};

static void init_rsm_cnf(void)
{
    static const Prod *const prods[] = {g_cnf_S, g_cnf_M};
    static const size_t nprods[] = {2, 1};
    init_rsm_from_grammar(2, 2, 0, prods, nprods);
}

// S -> a S b | a b | c S d | c d
static const int g_abcd_0[] = {TERM(0), NT(0), TERM(1)};
static const int g_abcd_1[] = {TERM(0), TERM(1)};
static const int g_abcd_2[] = {TERM(2), NT(0), TERM(3)};
static const int g_abcd_3[] = {TERM(2), TERM(3)};
static const Prod g_abcd_S[] = {
    {g_abcd_0, 3}, {g_abcd_1, 2}, {g_abcd_2, 3}, {g_abcd_3, 2}};

static void init_rsm_abcd(void)
{
    static const Prod *const prods[] = {g_abcd_S};
    static const size_t nprods[] = {4};
    init_rsm_from_grammar(4, 1, 0, prods, nprods);
}

// S -> a S b | a b | c S d | c d | S S
static const int g_concat_4[] = {NT(0), NT(0)};
static const Prod g_concat_S[] = {
    {g_abcd_0, 3}, {g_abcd_1, 2}, {g_abcd_2, 3}, {g_abcd_3, 2}, {g_concat_4, 2}};

static void init_rsm_abcd_concat(void)
{
    static const Prod *const prods[] = {g_concat_S};
    static const size_t nprods[] = {5};
    init_rsm_from_grammar(4, 1, 0, prods, nprods);
}

// raw RSM: S: 0 -a-> 1 -S-> 2 -b-> 0, start = 0, final = {0}
static const Trans g_raw_anbn_trans[] = {
    {0, TERM(0), 1},
    {1, NT(0), 2},
    {2, TERM(1), 0},
};

static void init_rsm_raw_anbn(void)
{
    static const GrB_Index starts[] = {0};
    static const GrB_Index finals_0[] = {0};
    static const GrB_Index *const finals[] = {finals_0};
    static const size_t nfinals[] = {1};
    init_rsm_raw(2, 1, 0, 3, starts, g_raw_anbn_trans,
                 sizeof(g_raw_anbn_trans) / sizeof(g_raw_anbn_trans[0]), finals, nfinals);
}

// raw RSM: S: 0 -a-> 1 -S-> 2 -b-> 0, 0 -c-> 3 -S-> 4 -d-> 0,
//          start = 0, final = {0}
static const Trans g_raw_abcd_trans[] = {
    {0, TERM(0), 1},
    {1, NT(0), 2},
    {2, TERM(1), 0},
    {0, TERM(2), 3},
    {3, NT(0), 4},
    {4, TERM(3), 0},
};

static void init_rsm_raw_abcd(void)
{
    static const GrB_Index starts[] = {0};
    static const GrB_Index finals_0[] = {0};
    static const GrB_Index *const finals[] = {finals_0};
    static const size_t nfinals[] = {1};
    init_rsm_raw(4, 1, 0, 5, starts, g_raw_abcd_trans,
                 sizeof(g_raw_abcd_trans) / sizeof(g_raw_abcd_trans[0]), finals, nfinals);
}

//==============================================================================
// graphs
//==============================================================================

// 0-a->1; 1-a->2; 2-a->0; 1-b->3; 3-b->1   (V=4, a=0, b=1)
static const Edge g_cyclic[] = {
    {0, 0, 1},
    {1, 0, 2},
    {2, 0, 0},
    {1, 1, 3},
    {3, 1, 1},
};

// 0-a->1; 1-a->2; 2-a->3   (V=4, a=0)
static const Edge g_chain_right[] = {
    {0, 0, 1},
    {1, 0, 2},
    {2, 0, 3},
};

// 0-a->1; 1-a->2   (V=3, a=0)
static const Edge g_chain_left[] = {
    {0, 0, 1},
    {1, 0, 2},
};

// 0-a->1; 1-a->2; 2-b->3; 3-b->4   (V=5, a=0, b=1)
static const Edge g_dyck_linear[] = {
    {0, 0, 1},
    {1, 0, 2},
    {2, 1, 3},
    {3, 1, 4},
};

// 0-a->0; 0-b->0   (V=1, a=0, b=1)
static const Edge g_dyck_self_loop[] = {
    {0, 0, 0},
    {0, 1, 0},
};

// 0-a->1   (V=2, a=0)
static const Edge g_epsilon[] = {
    {0, 0, 1},
};

// 0-a->1; 1-a->0; 0-a->2   (V=3, a=0)
static const Edge g_even[] = {
    {0, 0, 1},
    {1, 0, 0},
    {0, 0, 2},
};

// 0-a->1; 1-a->0; 0-b->2   (V=3, a=0, b=1)
static const Edge g_no_match[] = {
    {0, 0, 1},
    {1, 0, 0},
    {0, 1, 2},
};

// 0-a->1; 1-a->2; 2-a->3   (V=4, a=0)  -- same shape as g_chain_right
static const Edge g_ambig[] = {
    {0, 0, 1},
    {1, 0, 2},
    {2, 0, 3},
};

// 0-a->1; 1-b->2; 0-b->3   (V=4, a=0, b=1)
static const Edge g_three[] = {
    {0, 0, 1},
    {1, 1, 2},
    {0, 1, 3},
};

// 0-a->1; 1-a->2; 2-a->0;
// 1-b->3; 3-b->4; 4-b->5; 5-b->6; 6-b->1   (V=7, a=0, b=1)
static const Edge g_cyclic_3a_5b[] = {
    {0, 0, 1},
    {1, 0, 2},
    {2, 0, 0},
    {1, 1, 3},
    {3, 1, 4},
    {4, 1, 5},
    {5, 1, 6},
    {6, 1, 1},
};

// 0-a->0; 0-b->1; 1-b->1   (V=2, a=0, b=1)
static const Edge g_reg_ab[] = {
    {0, 0, 0},
    {0, 1, 1},
    {1, 1, 1},
};

// 0-a->1; 1-a->2; 2-a->0   (V=3, a=0)
static const Edge g_left_cycle[] = {
    {0, 0, 1},
    {1, 0, 2},
    {2, 0, 0},
};

// 0-a->1; 1-a->2; 2-a->0; 1-d->3; 3-d->4; 4-d->0;
// 0-b->5; 5-b->0; 6-c->0; 0-c->6   (V=7, a=0, b=1, c=2, d=3)
static const Edge g_abcd_start1[] = {
    {0, 0, 1},
    {1, 0, 2},
    {2, 0, 0},
    {1, 3, 3},
    {3, 3, 4},
    {4, 3, 0},
    {0, 1, 5},
    {5, 1, 0},
    {6, 2, 0},
    {0, 2, 6},
};

// same as g_abcd_start1 but with 0-d->3 instead of 1-d->3
static const Edge g_abcd_start6[] = {
    {0, 0, 1},
    {1, 0, 2},
    {2, 0, 0},
    {0, 3, 3},
    {3, 3, 4},
    {4, 3, 0},
    {0, 1, 5},
    {5, 1, 0},
    {6, 2, 0},
    {0, 2, 6},
};

#define N_EDGES(a) (sizeof(a) / sizeof((a)[0]))

#define run_algorithm(sources, num_sources)                                                        \
    LAGraph_CFPQ_RSM_blocked(&result, rsm, graph_term, (sources), (num_sources), V, msg);

//==============================================================================
// test cases
//==============================================================================

//------------------------------------------------------------------------------
// cyclic a^n b^n, start = 2 : expected {1, 3}
//------------------------------------------------------------------------------
void test_cyclic_anbn_start2(void)
{
    setup();
    init_rsm_anbn();
    init_graph(4, 2, g_cyclic, N_EDGES(g_cyclic));

    GrB_Index sources[] = {2};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {1, 3};
        check_reachable(expected, 2, "cyclic_anbn_start2");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// cyclic a^n b^n, start = 0 : expected {1, 3}
//------------------------------------------------------------------------------
void test_cyclic_anbn_start0(void)
{
    setup();
    init_rsm_anbn();
    init_graph(4, 2, g_cyclic, N_EDGES(g_cyclic));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {1, 3};
        check_reachable(expected, 2, "cyclic_anbn_start0");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// right-recursive S -> a S | a, start = 0 : expected {1, 2, 3}
//------------------------------------------------------------------------------
void test_right_recursive(void)
{
    setup();
    init_rsm_right_recursive();
    init_graph(4, 1, g_chain_right, N_EDGES(g_chain_right));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {1, 2, 3};
        check_reachable(expected, 3, "right_recursive");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// left-recursive S -> S a | epsilon, start = 0 : expected {0, 1, 2}
//------------------------------------------------------------------------------
void test_left_recursive(void)
{
    setup();
    init_rsm_left_recursive();
    init_graph(3, 1, g_chain_left, N_EDGES(g_chain_left));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0, 1, 2};
        check_reachable(expected, 3, "left_recursive");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// Dyck S -> a S b | epsilon, start = 0 : expected {0, 4}
//------------------------------------------------------------------------------
void test_dyck(void)
{
    setup();
    init_rsm_dyck();
    init_graph(5, 2, g_dyck_linear, N_EDGES(g_dyck_linear));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0, 4};
        check_reachable(expected, 2, "dyck");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// Dyck self-loop, start = 0 : expected {0}
//------------------------------------------------------------------------------
void test_dyck_self_loop(void)
{
    setup();
    init_rsm_dyck();
    init_graph(1, 2, g_dyck_self_loop, N_EDGES(g_dyck_self_loop));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0};
        check_reachable(expected, 1, "dyck_self_loop");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// mutual recursion S -> a A, A -> S b | b, start = 2 : expected {1, 3}
//------------------------------------------------------------------------------
void test_mutual_recursion(void)
{
    setup();
    init_rsm_mutual_recursion();
    init_graph(4, 2, g_cyclic, N_EDGES(g_cyclic));

    GrB_Index sources[] = {2};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {1, 3};
        check_reachable(expected, 2, "mutual_recursion");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// epsilon-only S -> epsilon, start = 0 : expected {0}
//------------------------------------------------------------------------------
void test_epsilon_only(void)
{
    setup();
    init_rsm_epsilon();
    init_graph(2, 1, g_epsilon, N_EDGES(g_epsilon));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0};
        check_reachable(expected, 1, "epsilon_only");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// even-palindrome S -> a S a | epsilon, start = 0 : expected {0}
//------------------------------------------------------------------------------
void test_start_equals_final_with_cycle(void)
{
    setup();
    init_rsm_even_palindrome();
    init_graph(3, 1, g_even, N_EDGES(g_even));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0};
        check_reachable(expected, 1, "even_palindrome");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// no match (dead-end b), start = 0 : expected {}
//------------------------------------------------------------------------------
void test_no_match(void)
{
    setup();
    init_rsm_anbn();
    init_graph(3, 2, g_no_match, N_EDGES(g_no_match));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        check_reachable(NULL, 0, "no_match");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// ambiguous S -> S S | a | epsilon, start = 0 : expected {0, 1, 2, 3}
//------------------------------------------------------------------------------
void test_ambiguous_grammar(void)
{
    setup();
    init_rsm_ambiguous();
    init_graph(4, 1, g_ambig, N_EDGES(g_ambig));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0, 1, 2, 3};
        check_reachable(expected, 4, "ambiguous_grammar");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// three NTs S -> A B, A -> a, B -> b, start = 0 : expected {2}
//------------------------------------------------------------------------------
void test_three_nonterminals(void)
{
    setup();
    init_rsm_three_nonterminals();
    init_graph(4, 2, g_three, N_EDGES(g_three));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {2};
        check_reachable(expected, 1, "three_nonterminals");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// cyclic Dyck S -> a S b | epsilon, start = 2 : expected {1, 2, 3}
//------------------------------------------------------------------------------
void test_cyclic_with_epsilon_base(void)
{
    setup();
    init_rsm_dyck();
    init_graph(4, 2, g_cyclic, N_EDGES(g_cyclic));

    GrB_Index sources[] = {2};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {1, 2, 3};
        check_reachable(expected, 3, "cyclic_with_epsilon_base");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// cyclic a^n b^n, 3(a)-5(b) loops, start = 0 : expected {1, 3, 4, 5, 6}
//------------------------------------------------------------------------------
void test_cyclic_3_5_anbn_start1(void)
{
    setup();
    init_rsm_anbn();
    init_graph(7, 2, g_cyclic_3a_5b, N_EDGES(g_cyclic_3a_5b));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {1, 3, 4, 5, 6};
        check_reachable(expected, 5, "cyclic_3_5_anbn");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// cyclic a^n b^n CNF, 3(a)-5(b) loops, start = 0 : expected {1, 3, 4, 5, 6}
//------------------------------------------------------------------------------
void test_cyclic_3_5_anbn_CNF_start1(void)
{
    setup();
    init_rsm_cnf();
    init_graph(7, 2, g_cyclic_3a_5b, N_EDGES(g_cyclic_3a_5b));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {1, 3, 4, 5, 6};
        check_reachable(expected, 5, "cyclic_3_5_anbn_CNF");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// cyclic regular a^n b^n CNF, start = 0 : expected {1}
//------------------------------------------------------------------------------
void test_cyclic_reg_anbn_CNF_start0(void)
{
    setup();
    init_rsm_cnf();
    init_graph(2, 2, g_reg_ab, N_EDGES(g_reg_ab));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {1};
        check_reachable(expected, 1, "cyclic_reg_anbn_CNF");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// left-recursive cycle S -> S a | epsilon, start = 0 : expected {0, 1, 2}
//------------------------------------------------------------------------------
void test_left_recursive_cycle(void)
{
    setup();
    init_rsm_left_recursive();
    init_graph(3, 1, g_left_cycle, N_EDGES(g_left_cycle));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0, 1, 2};
        check_reachable(expected, 3, "left_recursive_cycle");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// cyclic a^n b^n c^n d^n, start = 1 : expected {0, 5}
//------------------------------------------------------------------------------
void test_cyclic_2_3_anbncndn_start1(void)
{
    setup();
    init_rsm_abcd();
    init_graph(7, 4, g_abcd_start1, N_EDGES(g_abcd_start1));

    GrB_Index sources[] = {1};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0, 5};
        check_reachable(expected, 2, "cyclic_anbncndn_start1");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// cyclic a^n b^n c^n d^n, start = 6 : expected {0, 3, 4}
//------------------------------------------------------------------------------
void test_cyclic_2_3_anbncndn_start6(void)
{
    setup();
    init_rsm_abcd();
    init_graph(7, 4, g_abcd_start6, N_EDGES(g_abcd_start6));

    GrB_Index sources[] = {6};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0, 3, 4};
        check_reachable(expected, 3, "cyclic_anbncndn_start6");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// cyclic a^n b^n c^n d^n concat, start = 1 : expected {0, 3, 4, 5}
//------------------------------------------------------------------------------
void test_cyclic_2_3_anbncndn_concat_start1(void)
{
    setup();
    init_rsm_abcd_concat();
    init_graph(7, 4, g_abcd_start6, N_EDGES(g_abcd_start6));

    GrB_Index sources[] = {1};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0, 3, 4, 5};
        check_reachable(expected, 4, "cyclic_anbncndn_concat_start1");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// raw RSM a^n b^n, 3(a)-5(b) loops, start = 0 : expected {0, 1, 3, 4, 5, 6}
//------------------------------------------------------------------------------
void test_cyclic_3_5_anbn_raw_RSM_start1(void)
{
    setup();
    init_rsm_raw_anbn();
    init_graph(7, 2, g_cyclic_3a_5b, N_EDGES(g_cyclic_3a_5b));

    GrB_Index sources[] = {0};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0, 1, 3, 4, 5, 6};
        check_reachable(expected, 6, "cyclic_3_5_anbn_raw_RSM");
    }

    free_workspace();
    OK(info);
    teardown();
}

//------------------------------------------------------------------------------
// raw RSM a^n b^n c^n d^n concat, start = 1 : expected {0, 1, 3, 4, 5}
//------------------------------------------------------------------------------
void test_cyclic_2_3_anbncndn_concat_raw_rsm_start1(void)
{
    setup();
    init_rsm_raw_abcd();
    init_graph(7, 4, g_abcd_start6, N_EDGES(g_abcd_start6));

    GrB_Index sources[] = {1};
    GrB_Info info = run_algorithm(sources, 1);
    if (info == GrB_SUCCESS)
    {
        GrB_Index expected[] = {0, 1, 3, 4, 5};
        check_reachable(expected, 5, "cyclic_anbncndn_concat_raw_rsm");
    }

    free_workspace();
    OK(info);
    teardown();
}

TEST_LIST = {{"cyclic_anbn_start2", test_cyclic_anbn_start2},
             {"cyclic_anbn_start0", test_cyclic_anbn_start0},
             {"right_recursive", test_right_recursive},
             {"left_recursive", test_left_recursive},
             {"dyck", test_dyck},
             {"dyck_self_loop", test_dyck_self_loop},
             {"mutual_recursion", test_mutual_recursion},
             {"epsilon_only", test_epsilon_only},
             {"start_equals_final_with_cycle", test_start_equals_final_with_cycle},
             {"no_match", test_no_match},
             {"ambiguous_grammar", test_ambiguous_grammar},
             {"three_nonterminals", test_three_nonterminals},
             {"cyclic_with_epsilon_base", test_cyclic_with_epsilon_base},
             {"cyclic_3_5_anbn_start1", test_cyclic_3_5_anbn_start1},
             {"cyclic_3_5_anbn_CNF_start1", test_cyclic_3_5_anbn_CNF_start1},
             {"cyclic_reg_anbn_CNF_start0", test_cyclic_reg_anbn_CNF_start0},
             {"left_recursive_cycle", test_left_recursive_cycle},
             {"cyclic_2_3_anbncndn_start1", test_cyclic_2_3_anbncndn_start1},
             {"cyclic_2_3_anbncndn_start6", test_cyclic_2_3_anbncndn_start6},
             {"cyclic_2_3_anbncndn_concat_start1", test_cyclic_2_3_anbncndn_concat_start1},
             {"cyclic_3_5_anbn_raw_RSM_start1", test_cyclic_3_5_anbn_raw_RSM_start1},
             {"cyclic_2_3_anbncndn_concat_raw_rsm_start1",
              test_cyclic_2_3_anbncndn_concat_raw_rsm_start1},
             {NULL, NULL}};
