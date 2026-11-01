//------------------------------------------------------------------------------
// cfpq_rsm_lab/tests/oracle.h: independent reference for CFPQ RSM regressions
//------------------------------------------------------------------------------
//
// DS-00 requires an oracle that is independent of the GraphBLAS kernel and of
// the blocked variant.  This reference is a plain explicit-set/worklist
// implementation: for each box it explores (state, current vertex) pairs from
// (start state, entry vertex, entry vertex) using terminal edges and the
// current callee summaries, records (entry, current) pairs at final states as
// summary edges, and repeats over all boxes until no summary relation grows.
// It performs no fixed path-length cutoff, so recursion terminates only at the
// least fixed point.  The GraphBLAS matrices are converted to plain edge lists
// by the test driver (true entries only), which is where the true-only input
// boundary is enforced.

#ifndef CFPQ_RSM_LAB_ORACLE_H
#define CFPQ_RSM_LAB_ORACLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// one RSM transition (from, to) or one graph edge (from, to)
typedef struct
{
    uint64_t from;
    uint64_t to;
} cfpq_oracle_edge;

// Compact, GraphBLAS-free description of one CFPQ RSM instance.
//
//   term_edges[t]     : RSM transitions labeled terminal t, [term_begin[t], term_begin[t+1])
//   nonterm_edges[i]  : RSM transitions labeled nonterminal i, [nonterm_begin[i], nonterm_begin[i+1])
//   graph_edges[t]    : graph edges labeled terminal t, [graph_begin[t], graph_begin[t+1])
//   final_states      : final_states[i*state_count + q] == true iff q is final in box i
//   start_states[i]   : entry state of box i
typedef struct
{
    uint64_t state_count;
    uint64_t vertex_count;
    uint64_t term_count;
    uint64_t nonterm_count;
    uint64_t start_nonterminal;
    const uint64_t *start_states;
    const bool *final_states;
    const cfpq_oracle_edge *term_edges;
    const uint64_t *term_begin;
    const cfpq_oracle_edge *nonterm_edges;
    const uint64_t *nonterm_begin;
    const cfpq_oracle_edge *graph_edges;
    const uint64_t *graph_begin;
} cfpq_oracle_input;

// Compute every box summary relation: out_summary[i*V*V + u*V + v] is true iff
// box i accepts the path from entry vertex u to vertex v.  out_summary must
// have nonterm_count * vertex_count * vertex_count entries.
// Returns 0 on success, -1 on allocation failure or inconsistent input.
int cfpq_oracle_summaries(const cfpq_oracle_input *in, bool *out_summary);

// Compute the accepted destination set from the requested sources:
// out_reachable[w] is true iff some source s has a start-box summary (s, w).
// out_reachable must have vertex_count entries.  Returns 0 on success.
int cfpq_oracle_reachable(const cfpq_oracle_input *in, const uint64_t *sources, size_t num_sources,
                          bool *out_reachable);

#endif
