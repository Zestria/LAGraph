//------------------------------------------------------------------------------
// cfpq_rsm_lab/tests/oracle.c: independent fixed-point reference (DS-00)
//------------------------------------------------------------------------------
//
// See oracle.h.  Deliberately plain C: no GraphBLAS, no LAGraph, no kernel
// code.  Bounds are small (the test driver only uses it for small instances),
// so per-box DFS over (state, vertex) configurations with an explicit stack is
// sufficient and easy to audit.

#include "oracle.h"

#include <stdlib.h>
#include <string.h>

int cfpq_oracle_summaries(const cfpq_oracle_input *in, bool *out_summary)
{
    if (in == NULL || out_summary == NULL)
        return -1;

    const uint64_t Q = in->state_count;
    const uint64_t V = in->vertex_count;
    const uint64_t T = in->term_count;
    const uint64_t N = in->nonterm_count;
    const uint64_t S = V * V;

    if (N > 0 && in->start_states == NULL)
        return -1;
    if (N > 0 && in->final_states == NULL)
        return -1;

    memset(out_summary, 0, (size_t)N * (size_t)S * sizeof(bool));

    // Visited, discovered and DFS stack are reused across (box, entry)
    // traversals.  A configuration is marked discovered when it is pushed and
    // visited when it is popped; every distinct configuration is therefore
    // pushed at most once and the fixed Q*V stack cannot overflow because of
    // duplicate pending entries (R2 review finding P2).
    bool *visited = NULL;
    bool *discovered = NULL;
    uint64_t *stack = NULL;
    if (Q > 0 && V > 0)
    {
        visited = (bool *)calloc((size_t)Q * (size_t)V, sizeof(bool));
        discovered = (bool *)calloc((size_t)Q * (size_t)V, sizeof(bool));
        stack = (uint64_t *)malloc((size_t)Q * (size_t)V * sizeof(uint64_t));
        if (visited == NULL || discovered == NULL || stack == NULL)
        {
            free(visited);
            free(discovered);
            free(stack);
            return -1;
        }
    }

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (uint64_t box = 0; box < N; ++box)
        {
            // a box with no final state contributes no summary pair
            bool has_final = false;
            for (uint64_t q = 0; q < Q; ++q)
            {
                if (in->final_states[box * Q + q])
                {
                    has_final = true;
                    break;
                }
            }
            if (!has_final)
                continue;

            for (uint64_t u = 0; u < V; ++u)
            {
                size_t sp = 0;
                memset(visited, 0, (size_t)Q * (size_t)V * sizeof(bool));
                memset(discovered, 0, (size_t)Q * (size_t)V * sizeof(bool));
                stack[sp++] = in->start_states[box] * V + u;
                discovered[stack[0]] = true;
                while (sp > 0)
                {
                    uint64_t code = stack[--sp];
                    uint64_t q = code / V;
                    uint64_t v = code % V;
                    if (visited[q * V + v])
                        continue;
                    visited[q * V + v] = true;

                    if (in->final_states[box * Q + q])
                    {
                        if (!out_summary[box * S + u * V + v])
                        {
                            out_summary[box * S + u * V + v] = true;
                            changed = true;
                        }
                    }

                    // terminal transitions: follow one RSM edge and one graph edge
                    for (uint64_t t = 0; t < T; ++t)
                    {
                        for (uint64_t k = in->term_begin[t]; k < in->term_begin[t + 1]; ++k)
                        {
                            if (in->term_edges[k].from != q)
                                continue;
                            for (uint64_t g = in->graph_begin[t]; g < in->graph_begin[t + 1]; ++g)
                            {
                                if (in->graph_edges[g].from != v)
                                    continue;
                                uint64_t next = in->term_edges[k].to * V + in->graph_edges[g].to;
                                if (!discovered[next] && !visited[next])
                                {
                                    if (sp >= (size_t)Q * (size_t)V)
                                    {
                                        free(visited);
                                        free(discovered);
                                        free(stack);
                                        return -1;
                                    }
                                    discovered[next] = true;
                                    stack[sp++] = next;
                                }
                            }
                        }
                    }

                    // nonterminal transitions: follow one RSM edge and one
                    // current summary edge of the callee
                    for (uint64_t j = 0; j < N; ++j)
                    {
                        for (uint64_t k = in->nonterm_begin[j]; k < in->nonterm_begin[j + 1]; ++k)
                        {
                            if (in->nonterm_edges[k].from != q)
                                continue;
                            for (uint64_t w = 0; w < V; ++w)
                            {
                                if (!out_summary[j * S + v * V + w])
                                    continue;
                                uint64_t next = in->nonterm_edges[k].to * V + w;
                                if (!discovered[next] && !visited[next])
                                {
                                    if (sp >= (size_t)Q * (size_t)V)
                                    {
                                        free(visited);
                                        free(discovered);
                                        free(stack);
                                        return -1;
                                    }
                                    discovered[next] = true;
                                    stack[sp++] = next;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    free(visited);
    free(discovered);
    free(stack);
    return 0;
}

int cfpq_oracle_reachable(const cfpq_oracle_input *in, const uint64_t *sources, size_t num_sources,
                          bool *out_reachable)
{
    if (in == NULL || out_reachable == NULL)
        return -1;
    if (num_sources > 0 && sources == NULL)
        return -1;

    const uint64_t V = in->vertex_count;
    const uint64_t N = in->nonterm_count;
    if (N == 0)
        return -1;

    bool *summary = (bool *)calloc((size_t)N * (size_t)V * (size_t)V, sizeof(bool));
    if (summary == NULL)
        return -1;

    int status = cfpq_oracle_summaries(in, summary);
    if (status == 0)
    {
        memset(out_reachable, 0, (size_t)V * sizeof(bool));
        const uint64_t start = in->start_nonterminal;
        for (size_t k = 0; k < num_sources; ++k)
        {
            uint64_t s = sources[k];
            if (s >= V)
            {
                status = -1;
                break;
            }
            for (uint64_t w = 0; w < V; ++w)
            {
                if (summary[start * V * V + s * V + w])
                    out_reachable[w] = true;
            }
        }
    }

    free(summary);
    return status;
}
