// Experimental reusable P1 preparation API. The existing one-shot API is unchanged.
#ifndef LAGRAPH_CFPQ_RSM_PREPARED_H
#define LAGRAPH_CFPQ_RSM_PREPARED_H

#include "LAGraphX.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LAGraph_CFPQ_RSM_Plan LAGraph_CFPQ_RSM_Plan;

// Borrows the RSM, graph matrix arrays and their contents. They must remain
// alive and unchanged until FreePrepared. No sources/query results are cached.
// On failure *prepared is NULL; partially constructed state is released.
int LAGraph_CFPQ_RSM_Prepare(LAGraph_CFPQ_RSM_Plan **prepared, const RSM *rsm,
                            const GrB_Matrix *graph_term, GrB_Index V, char *msg);

// Each execution owns fresh scratch, summaries and waiting prefixes.
// On failure *reachable is NULL; the prepared plan remains usable.
// Callers materialize the returned vector before stopping an execution timer.
int LAGraph_CFPQ_RSM_Execute(GrB_Vector *reachable, const LAGraph_CFPQ_RSM_Plan *prepared,
                            const GrB_Index *sources, size_t num_sources, char *msg);

// NULL-safe, idempotent. Must run before releasing borrowed inputs/unloading code.
void LAGraph_CFPQ_RSM_FreePrepared(LAGraph_CFPQ_RSM_Plan **prepared);

#ifdef __cplusplus
}
#endif
#endif
