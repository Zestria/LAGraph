//------------------------------------------------------------------------------
// cfpq_profile.h: optional per-call profiling for the corrected RSM reference
//------------------------------------------------------------------------------
//
// DS-01-R2 instrumentation, compiled only when CFPQ_RSM_PROFILE is defined.
// Timing builds use the reference source directly and never see this header.
//
// Design notes required by the R2 review:
//   * separate start variables for whole phases, nested sub-operations,
//     representation conversion, summary extraction and diagnostic I/O;
//   * per-phase conversion attribution and exclusive phase times;
//   * explicit completion boundaries (GrB wait) after each phase, with their
//     cost recorded separately; phase API time + boundary wait is the completed
//     work estimate;
//   * total_s is the complete instrumented kernel call from entry to after
//     kernel cleanup; kernel_s subtracts diagnostic I/O; setup_s and cleanup_s
//     are separate buckets, and unclassified_s reconciles the remainder;
//   * sanity checks verify every subset fits inside its containing phase and
//     that the buckets reconcile within a declared tolerance;
//   * per-iteration records use the incoming frontier and visited sizes at a
//     documented boundary, and record the logical candidate union, its
//     intersection with visited state, and the next frontier separately;
//   * |X \ W_old| and |X intersect W_old| are measured per box and iteration;
//   * memory is reported by GxB_Matrix_memoryUsage / GxB_Vector_memoryUsage
//     (actual allocation bytes, as defined by the library) alongside process
//     peak RSS;
//   * every diagnostic row carries CFPQ_RSM_PROFILE_TAG so the runner can give
//     it a durable case/chunk/round/warm-up identity across library reloads.
//
// Output files (appended):
//   $CFPQ_RSM_PROFILE_FILE                one key=value line per kernel call
//   $CFPQ_RSM_PROFILE_FILE.detail.csv     per-iteration and per-box rows
//   $CFPQ_RSM_PROFILE_FILE.matrix.csv     persistent matrix metadata and bytes
//   $CFPQ_RSM_PROFILE_FILE.vector.csv     persistent vector metadata and bytes
// If CFPQ_RSM_PROFILE_FILE is unset, profiling is inactive.  All fixed-size
// per-box arrays are guarded; overflow sets detail_truncated.

#ifndef CFPQ_RSM_PROFILE_H
#define CFPQ_RSM_PROFILE_H

#include "LAGraph.h"
#include <GraphBLAS.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define CFPQ_PROF_MAX_ITER 65536
#define CFPQ_PROF_MAX_BOX 8192
#define CFPQ_PROF_TAG_LEN 512
// declared absolute tolerance for the subset and reconciliation checks
#define CFPQ_PROF_TOL_S 1e-4

typedef struct
{
    bool enabled;
    bool detail_truncated;
    bool sanity_ok;
    int sanity_violations;
    FILE *summary;
    FILE *detail;
    FILE *matrix;
    FILE *vector;
    char tag[CFPQ_PROF_TAG_LEN];
    uint64_t call_id;

    // wall-clock attribution (seconds)
    double setup_s;
    double activity_s;
    double phase1_s;
    double phase2_3_s;
    double phase4_s;
    double phase5_s;
    double final_s;
    double materialize_s;
    double cleanup_s;
    double boundary_p1_s;
    double boundary_p2_3_s;
    double boundary_p4_s;
    double boundary_p5_s;
    double boundary_s;
    double diag_io_s;
    double diag_x_s;       // diagnostic X\W / X&W probes (inside phase 2+3)
    double diag_union_s;   // diagnostic candidate-union probes (inside phase 5)
    double total_s;        // entry to after kernel cleanup (instrumented call)
    double kernel_s;       // total_s - diag_io_s
    double unclassified_s;
    double tolerance_s;

    // conversion time, total and attributed per phase
    double conversion_s;
    double phase1_conv_s;
    double phase2_3_conv_s;
    double phase4_conv_s;
    double phase5_conv_s;

    // nested work timings (subsets of the containing phase)
    double callconstr_s;
    double summary_s;
    double wupdate_s;
    double retprod_s;

    // Nested operation timings; these overlap phase/setup times.
    double grouped_s; uint64_t grouped_calls;
    double group_setup_s; uint64_t group_setup_calls;
    double compaction_s; uint64_t compaction_calls;
    double routing_setup_s; uint64_t routing_setup_calls;
    double routing_s; uint64_t routing_calls;
    double fusion_setup_s; uint64_t fusion_setup_calls;
    double fusion_s; uint64_t fusion_calls;
    uint64_t compact_q, waiting_blocks, fused_blocks, group_count, group_columns;

    // call shape
    uint64_t q, v, nterm, nnonterm, nboxes_called, nboxes_final, nsources, seed_entries;

    // iteration counters
    uint64_t iters;
    uint64_t sum_m, max_m, sum_p, max_p;
    uint64_t sum_cand, max_cand;           // logical union acc|M_call, before subtraction
    uint64_t sum_cand_in_p, max_cand_in_p; // same union intersect visited P
    uint64_t sum_next, max_next;
    uint64_t sum_x, sum_x_fresh, sum_x_overlap;

    // operation counters
    uint64_t term_static_skip, term_pred_skip, term_shift, term_product, term_skip_product;
    uint64_t nt_shift, nt_call, nt_skip_product, ret_extract, ret_dg, ret_product;
    uint64_t clears, reshapes, w_updates, summary_unions;

    // intermediate peaks
    uint64_t peak_m, peak_p, peak_acc, peak_scratch, peak_s_temp, peak_new_edges, peak_union;

    // per-iteration detail
    uint64_t iter_in_m[CFPQ_PROF_MAX_ITER];
    uint64_t iter_p_before[CFPQ_PROF_MAX_ITER];
    uint64_t iter_cand[CFPQ_PROF_MAX_ITER];
    uint64_t iter_cand_in_p[CFPQ_PROF_MAX_ITER];
    uint64_t iter_next[CFPQ_PROF_MAX_ITER];
    uint64_t iter_p_after[CFPQ_PROF_MAX_ITER];

    // per-box detail
    uint64_t box_dg[CFPQ_PROF_MAX_BOX];
    uint64_t box_x[CFPQ_PROF_MAX_BOX];
    uint64_t box_fresh[CFPQ_PROF_MAX_BOX];
    uint64_t box_overlap[CFPQ_PROF_MAX_BOX];
    uint64_t box_calls[CFPQ_PROF_MAX_BOX];
    uint64_t box_wmax[CFPQ_PROF_MAX_BOX];
    uint64_t box_gntmax[CFPQ_PROF_MAX_BOX];
} cfpq_prof;

static inline double cfpq_prof_now(void)
{
    return LAGraph_WallClockTime();
}

static inline void cfpq_prof_abort(cfpq_prof *p)
{
    if (p->summary != NULL)
        fflush(p->summary);
    if (p->detail != NULL)
        fflush(p->detail);
    if (p->matrix != NULL)
        fflush(p->matrix);
    if (p->vector != NULL)
        fflush(p->vector);
    if (p->summary != NULL)
        fclose(p->summary);
    if (p->detail != NULL)
        fclose(p->detail);
    if (p->matrix != NULL)
        fclose(p->matrix);
    if (p->vector != NULL)
        fclose(p->vector);
    p->summary = NULL;
    p->detail = NULL;
    p->matrix = NULL;
    p->vector = NULL;
    p->enabled = false;
}

static inline void cfpq_prof_init(cfpq_prof *p)
{
    memset(p, 0, sizeof(*p));
    const char *path = getenv("CFPQ_RSM_PROFILE_FILE");
    if (path == NULL || path[0] == '\0')
        return;
    const char *tag = getenv("CFPQ_RSM_PROFILE_TAG");
    if (tag != NULL)
    {
        snprintf(p->tag, sizeof(p->tag), "%s", tag);
        // keep the key/value summary line and the CSV files parseable
        for (char *c = p->tag; *c != '\0'; ++c)
            if (*c == ';' || *c == '\n' || *c == '\r' || *c == ',')
                *c = '|';
    }
    p->summary = fopen(path, "a");
    if (p->summary == NULL)
        return;
    char name[4096];
    snprintf(name, sizeof(name), "%s.detail.csv", path);
    p->detail = fopen(name, "a");
    if (p->detail != NULL && ftell(p->detail) == 0)
        fprintf(p->detail,
                "tag,kind,call,iter,box,incoming_m,p_before,candidates,cand_in_p,next,p_after,"
                "dg_sum,x_sum,x_fresh_sum,x_overlap_sum,w_final,gnt_final,box_calls,w_max,gnt_max,"
                "w_bytes,gnt_bytes\n");
    snprintf(name, sizeof(name), "%s.matrix.csv", path);
    p->matrix = fopen(name, "a");
    if (p->matrix != NULL && ftell(p->matrix) == 0)
        fprintf(p->matrix,
                "tag,call,name,nrows,ncols,nvals,sparsity,iso,rowbits,colbits,offsetbits,"
                "orientation,bytes,bytes_status\n");
    snprintf(name, sizeof(name), "%s.vector.csv", path);
    p->vector = fopen(name, "a");
    if (p->vector != NULL && ftell(p->vector) == 0)
        fprintf(p->vector, "tag,call,name,size,nvals,bytes,bytes_status\n");
    p->enabled = true;
    static uint64_t next_call_id = 0;
    p->call_id = next_call_id++;
}

static inline void cfpq_prof_matrix(FILE *f, const char *tag, uint64_t call, const char *name,
                                    GrB_Matrix A)
{
    (void)call;
    if (f == NULL)
        return;
    if (A == GrB_NULL)
    {
        fprintf(f, "%s,%llu,%s,0,0,0,-1,-1,-1,-1,-1,-1,-1,no_matrix\n", tag,
                (unsigned long long)call, name);
        return;
    }
    GrB_Index nr = 0, nc = 0, nv = 0;
    GrB_Matrix_nrows(&nr, A);
    GrB_Matrix_ncols(&nc, A);
    GrB_Matrix_nvals(&nv, A);
    int32_t status = -1, iso = -1, pbits = -1, ibits = -1, obits = -1, orient = -1;
    GrB_get(A, &status, GxB_SPARSITY_STATUS);
    GrB_get(A, &iso, GxB_ISO);
    GrB_get(A, &pbits, GxB_ROWINDEX_INTEGER_BITS);
    GrB_get(A, &ibits, GxB_COLINDEX_INTEGER_BITS);
    GrB_get(A, &obits, GxB_OFFSET_INTEGER_BITS);
    GrB_get(A, &orient, GrB_STORAGE_ORIENTATION_HINT);
    size_t bytes = 0;
    GrB_Info b = GxB_Matrix_memoryUsage(&bytes, A);
    fprintf(f, "%s,%llu,%s,%llu,%llu,%llu,%d,%d,%d,%d,%d,%d,%llu,%s\n", tag,
            (unsigned long long)call, name, (unsigned long long)nr, (unsigned long long)nc,
            (unsigned long long)nv, (int)status,
            (int)iso, (int)pbits, (int)ibits, (int)obits, (int)orient,
            (unsigned long long)(b == GrB_SUCCESS ? bytes : 0),
            b == GrB_SUCCESS ? "ok" : "unavailable");
}

static inline void cfpq_prof_vector(FILE *f, const char *tag, uint64_t call, const char *name,
                                    GrB_Vector v)
{
    if (f == NULL)
        return;
    if (v == GrB_NULL)
    {
        fprintf(f, "%s,%llu,%s,0,0,-1,no_vector\n", tag, (unsigned long long)call, name);
        return;
    }
    GrB_Index n = 0, nv = 0;
    GrB_Vector_size(&n, v);
    GrB_Vector_nvals(&nv, v);
    size_t bytes = 0;
    GrB_Info b = GxB_Vector_memoryUsage(&bytes, v);
    fprintf(f, "%s,%llu,%s,%llu,%llu,%llu,%s\n", tag, (unsigned long long)call, name,
            (unsigned long long)n, (unsigned long long)nv,
            (unsigned long long)(b == GrB_SUCCESS ? bytes : 0),
            b == GrB_SUCCESS ? "ok" : "unavailable");
}

// Write per-iteration, per-box, matrix and vector records.  Must be called
// before kernel cleanup because it queries the live handles.  Matrix/vector
// metadata is written here, not in cfpq_prof_emit, so cleanup can be timed.
static inline void cfpq_prof_snapshot(cfpq_prof *p, GrB_Matrix M, GrB_Matrix P, GrB_Matrix acc,
                                      GrB_Matrix scratch, GrB_Matrix s_temp, GrB_Matrix new_edges,
                                      GrB_Matrix M_union, GrB_Matrix X_fresh, GrB_Matrix X_keep,
                                      GrB_Vector entry_vec, GrB_Vector source_mask,
                                      GrB_Vector rowvec, GrB_Matrix *W, GrB_Matrix *graph_nt,
                                      size_t nn)
{
    if (!p->enabled)
        return;
    double io0 = cfpq_prof_now();
    if (p->detail != NULL)
    {
        uint64_t iters = p->iters < CFPQ_PROF_MAX_ITER ? p->iters : CFPQ_PROF_MAX_ITER;
        for (uint64_t k = 0; k < iters; ++k)
            fprintf(p->detail, "%s,iter,%llu,%llu,,%llu,%llu,%llu,%llu,%llu,%llu,,,,,,,,,,,,\n",
                    p->tag, (unsigned long long)p->call_id, (unsigned long long)k,
                    (unsigned long long)p->iter_in_m[k], (unsigned long long)p->iter_p_before[k],
                    (unsigned long long)p->iter_cand[k], (unsigned long long)p->iter_cand_in_p[k],
                    (unsigned long long)p->iter_next[k], (unsigned long long)p->iter_p_after[k]);
        size_t boxes = nn < CFPQ_PROF_MAX_BOX ? nn : CFPQ_PROF_MAX_BOX;
        for (size_t i = 0; i < boxes; ++i)
        {
            GrB_Index wn = 0, gn = 0;
            size_t wbytes = 0, gbytes = 0;
            if (W != NULL && W[i] != GrB_NULL)
            {
                GrB_Matrix_nvals(&wn, W[i]);
                GxB_Matrix_memoryUsage(&wbytes, W[i]);
            }
            if (graph_nt != NULL && graph_nt[i] != GrB_NULL)
            {
                GrB_Matrix_nvals(&gn, graph_nt[i]);
                GxB_Matrix_memoryUsage(&gbytes, graph_nt[i]);
            }
            fprintf(p->detail,
                    "%s,box,%llu,,%zu,,,,,,,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                    p->tag, (unsigned long long)p->call_id, i, (unsigned long long)p->box_dg[i],
                    (unsigned long long)p->box_x[i], (unsigned long long)p->box_fresh[i],
                    (unsigned long long)p->box_overlap[i], (unsigned long long)wn,
                    (unsigned long long)gn, (unsigned long long)p->box_calls[i],
                    (unsigned long long)p->box_wmax[i], (unsigned long long)p->box_gntmax[i],
                    (unsigned long long)wbytes, (unsigned long long)gbytes);
        }
        fflush(p->detail);
    }
    if (p->matrix != NULL)
    {
        cfpq_prof_matrix(p->matrix, p->tag, p->call_id, "M", M);
        cfpq_prof_matrix(p->matrix, p->tag, p->call_id, "P", P);
        cfpq_prof_matrix(p->matrix, p->tag, p->call_id, "acc", acc);
        cfpq_prof_matrix(p->matrix, p->tag, p->call_id, "scratch", scratch);
        cfpq_prof_matrix(p->matrix, p->tag, p->call_id, "s_temp", s_temp);
        cfpq_prof_matrix(p->matrix, p->tag, p->call_id, "new_edges", new_edges);
        cfpq_prof_matrix(p->matrix, p->tag, p->call_id, "M_union", M_union);
        cfpq_prof_matrix(p->matrix, p->tag, p->call_id, "X_fresh", X_fresh);
        cfpq_prof_matrix(p->matrix, p->tag, p->call_id, "X_keep", X_keep);
        size_t boxes = nn < CFPQ_PROF_MAX_BOX ? nn : CFPQ_PROF_MAX_BOX;
        char name[64];
        for (size_t i = 0; i < boxes; ++i)
        {
            snprintf(name, sizeof(name), "W[%zu]", i);
            cfpq_prof_matrix(p->matrix, p->tag, p->call_id, name,
                             (W != NULL ? W[i] : GrB_NULL));
            snprintf(name, sizeof(name), "graph_nt[%zu]", i);
            cfpq_prof_matrix(p->matrix, p->tag, p->call_id, name,
                             (graph_nt != NULL ? graph_nt[i] : GrB_NULL));
        }
        fflush(p->matrix);
    }
    if (p->vector != NULL)
    {
        cfpq_prof_vector(p->vector, p->tag, p->call_id, "entry_vec", entry_vec);
        cfpq_prof_vector(p->vector, p->tag, p->call_id, "source_mask", source_mask);
        cfpq_prof_vector(p->vector, p->tag, p->call_id, "rowvec", rowvec);
        fflush(p->vector);
    }
    p->diag_io_s += cfpq_prof_now() - io0;
}

static inline void cfpq_prof_emit(cfpq_prof *p, double total_s, double cleanup_s)
{
    if (!p->enabled || p->summary == NULL)
        return;
    double io0 = cfpq_prof_now();
    p->cleanup_s = cleanup_s;
    p->total_s = total_s;
    p->kernel_s = p->total_s - p->diag_io_s;
    p->boundary_s = p->boundary_p1_s + p->boundary_p2_3_s + p->boundary_p4_s + p->boundary_p5_s;
    double accounted = p->setup_s + p->activity_s + p->phase1_s + p->phase2_3_s + p->phase4_s +
                       p->phase5_s + p->final_s + p->materialize_s + p->cleanup_s + p->boundary_s;
    p->unclassified_s = p->kernel_s - accounted;
    p->tolerance_s = CFPQ_PROF_TOL_S + 0.02 * (p->kernel_s > 0.0 ? p->kernel_s : 0.0);

    // subset / reconciliation sanity checks (R2 requirement)
    p->sanity_violations = 0;
    double tol = p->tolerance_s;
    if (p->phase1_conv_s > p->phase1_s + tol)
        p->sanity_violations++;
    if (p->phase2_3_conv_s > p->phase2_3_s + tol)
        p->sanity_violations++;
    if (p->phase4_conv_s > p->phase4_s + tol)
        p->sanity_violations++;
    if (p->phase5_conv_s > p->phase5_s + tol)
        p->sanity_violations++;
    if (p->callconstr_s > p->phase2_3_s + tol)
        p->sanity_violations++;
    if (p->wupdate_s > p->phase2_3_s + tol)
        p->sanity_violations++;
    if (p->summary_s > p->phase4_s + tol)
        p->sanity_violations++;
    if (p->retprod_s > p->phase4_s + tol)
        p->sanity_violations++;
    if (p->unclassified_s < -tol || p->unclassified_s > tol)
        p->sanity_violations++;
    p->sanity_ok = (p->sanity_violations == 0);

    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    getrusage(RUSAGE_SELF, &ru);
    fprintf(p->summary,
        "grouped_s=%.9f;grouped_calls=%llu;group_setup_s=%.9f;group_setup_calls=%llu;compaction_s=%.9f;compaction_calls=%llu;routing_setup_s=%.9f;routing_setup_calls=%llu;routing_s=%.9f;routing_calls=%llu;fusion_setup_s=%.9f;fusion_setup_calls=%llu;fusion_s=%.9f;fusion_calls=%llu;compact_q=%llu;waiting_blocks=%llu;fused_blocks=%llu;group_count=%llu;group_columns=%llu;probe_coverage=phase-and-operation;candidate_counts_available=0;prefix_overlap_available=0;legacy_operation_counts_available=0;",
        p->grouped_s, (unsigned long long)p->grouped_calls, p->group_setup_s, (unsigned long long)p->group_setup_calls, p->compaction_s, (unsigned long long)p->compaction_calls, p->routing_setup_s, (unsigned long long)p->routing_setup_calls, p->routing_s, (unsigned long long)p->routing_calls, p->fusion_setup_s, (unsigned long long)p->fusion_setup_calls, p->fusion_s, (unsigned long long)p->fusion_calls,
        (unsigned long long)p->compact_q, (unsigned long long)p->waiting_blocks, (unsigned long long)p->fused_blocks, (unsigned long long)p->group_count, (unsigned long long)p->group_columns);
    fprintf(p->summary,
            "tag=%s;call=%llu;q=%llu;v=%llu;nterm=%llu;nnonterm=%llu;nboxes_called=%llu;"
            "nboxes_final=%llu;nsources=%llu;seed_entries=%llu;iters=%llu;"
            "setup_s=%.9f;activity_s=%.9f;phase1_s=%.9f;phase2_3_s=%.9f;phase4_s=%.9f;"
            "phase5_s=%.9f;final_s=%.9f;materialize_s=%.9f;cleanup_s=%.9f;"
            "boundary_p1_s=%.9f;boundary_p2_3_s=%.9f;boundary_p4_s=%.9f;boundary_p5_s=%.9f;"
            "boundary_s=%.9f;diag_io_s=%.9f;diag_x_s=%.9f;diag_union_s=%.9f;"
            "total_s=%.9f;kernel_s=%.9f;unclassified_s=%.9f;tolerance_s=%.9f;"
            "conversion_s=%.9f;phase1_conv_s=%.9f;phase2_3_conv_s=%.9f;phase4_conv_s=%.9f;"
            "phase5_conv_s=%.9f;callconstr_s=%.9f;summary_s=%.9f;wupdate_s=%.9f;retprod_s=%.9f;"
            "sum_m=%llu;max_m=%llu;sum_p=%llu;max_p=%llu;sum_cand=%llu;max_cand=%llu;"
            "sum_cand_in_p=%llu;max_cand_in_p=%llu;sum_next=%llu;max_next=%llu;"
            "sum_x=%llu;sum_x_fresh=%llu;sum_x_overlap=%llu;"
            "term_static_skip=%llu;term_pred_skip=%llu;term_shift=%llu;term_product=%llu;"
            "term_skip_product=%llu;nt_shift=%llu;nt_call=%llu;nt_skip_product=%llu;"
            "ret_extract=%llu;ret_dg=%llu;ret_product=%llu;clears=%llu;reshapes=%llu;"
            "w_updates=%llu;summary_unions=%llu;peak_m=%llu;peak_p=%llu;peak_acc=%llu;"
            "peak_scratch=%llu;peak_s_temp=%llu;peak_new_edges=%llu;peak_union=%llu;"
            "detail_truncated=%d;sanity_violations=%d;sanity_ok=%d;peak_rss_kb=%ld\n",
            p->tag, (unsigned long long)p->call_id, (unsigned long long)p->q,
            (unsigned long long)p->v, (unsigned long long)p->nterm,
            (unsigned long long)p->nnonterm, (unsigned long long)p->nboxes_called,
            (unsigned long long)p->nboxes_final, (unsigned long long)p->nsources,
            (unsigned long long)p->seed_entries, (unsigned long long)p->iters, p->setup_s,
            p->activity_s, p->phase1_s, p->phase2_3_s, p->phase4_s, p->phase5_s, p->final_s,
            p->materialize_s, p->cleanup_s, p->boundary_p1_s, p->boundary_p2_3_s,
            p->boundary_p4_s, p->boundary_p5_s, p->boundary_s, p->diag_io_s, p->diag_x_s,
            p->diag_union_s, p->total_s, p->kernel_s, p->unclassified_s, p->tolerance_s,
            p->conversion_s, p->phase1_conv_s, p->phase2_3_conv_s, p->phase4_conv_s,
            p->phase5_conv_s, p->callconstr_s, p->summary_s, p->wupdate_s, p->retprod_s,
            (unsigned long long)p->sum_m, (unsigned long long)p->max_m,
            (unsigned long long)p->sum_p, (unsigned long long)p->max_p,
            (unsigned long long)p->sum_cand, (unsigned long long)p->max_cand,
            (unsigned long long)p->sum_cand_in_p, (unsigned long long)p->max_cand_in_p,
            (unsigned long long)p->sum_next, (unsigned long long)p->max_next,
            (unsigned long long)p->sum_x, (unsigned long long)p->sum_x_fresh,
            (unsigned long long)p->sum_x_overlap, (unsigned long long)p->term_static_skip,
            (unsigned long long)p->term_pred_skip, (unsigned long long)p->term_shift,
            (unsigned long long)p->term_product, (unsigned long long)p->term_skip_product,
            (unsigned long long)p->nt_shift, (unsigned long long)p->nt_call,
            (unsigned long long)p->nt_skip_product, (unsigned long long)p->ret_extract,
            (unsigned long long)p->ret_dg, (unsigned long long)p->ret_product,
            (unsigned long long)p->clears, (unsigned long long)p->reshapes,
            (unsigned long long)p->w_updates, (unsigned long long)p->summary_unions,
            (unsigned long long)p->peak_m, (unsigned long long)p->peak_p,
            (unsigned long long)p->peak_acc, (unsigned long long)p->peak_scratch,
            (unsigned long long)p->peak_s_temp, (unsigned long long)p->peak_new_edges,
            (unsigned long long)p->peak_union, (int)p->detail_truncated,
            p->sanity_violations, p->sanity_ok ? 1 : 0, (long)ru.ru_maxrss);
    fflush(p->summary);
    p->diag_io_s += cfpq_prof_now() - io0;
    cfpq_prof_abort(p);
}

#endif
