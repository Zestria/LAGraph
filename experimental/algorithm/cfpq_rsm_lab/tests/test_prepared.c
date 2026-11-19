// Reuse the existing case builders, exact oracle and tracked allocator.
#define main ds00_main
#include "test_ds00.c"
#undef main
#include "LAGraph_CFPQ_RSM_prepared.h"

static void prepared_expect(const Case *c, const OracleView *ov,
                            const LAGraph_CFPQ_RSM_Plan *plan,
                            const GrB_Index *sources, size_t n)
{
    uint64_t src[8];
    for (size_t k = 0; k < n; ++k) src[k] = sources[k];
    bool expected[8] = {0};
    CHECK(cfpq_oracle_reachable(&ov->in, src, n, expected) == 0);
    GrB_Vector out = GrB_NULL;
    CHECK(LAGraph_CFPQ_RSM_Execute(&out, plan, sources, n, msg) == GrB_SUCCESS);
    if (out != GrB_NULL)
    {
        CHECK(GrB_Vector_wait(out, GrB_MATERIALIZE) == GrB_SUCCESS);
        CHECK(vector_matches(out, expected, c->V, true, "prepared query"));
    }
    GrB_free(&out);
}

static void reuse_case(Case *c)
{
    RSM rsm;
    case_rsm(c, &rsm);
    OracleView ov;
    CHECK(oracle_view_build(c, &ov));
    LAGraph_CFPQ_RSM_Plan *plan = NULL;
    CHECK(LAGraph_CFPQ_RSM_Prepare(&plan, &rsm, c->graph, c->V, msg) == GrB_SUCCESS);
    if (plan != NULL)
    {
        GrB_Index a = 0, b = c->V - 1, both[2] = {a,b};
        prepared_expect(c, &ov, plan, &a, 1);
        prepared_expect(c, &ov, plan, &b, 1);
        prepared_expect(c, &ov, plan, &a, 1); // A -> B -> A must not retain B
        prepared_expect(c, &ov, plan, NULL, 0);
        if (c->V >= 2) prepared_expect(c, &ov, plan, both, 2);
        GrB_Vector out = GrB_NULL;
        GrB_Index bad = c->V;
        CHECK(LAGraph_CFPQ_RSM_Execute(&out, plan, &bad, 1, msg) == GrB_INVALID_INDEX);
        CHECK(out == GrB_NULL);
        CHECK(LAGraph_CFPQ_RSM_Execute(&out, plan, NULL, 1, msg) == GrB_NULL_POINTER);
        CHECK(out == GrB_NULL);
        prepared_expect(c, &ov, plan, &a, 1); // failed calls leave the plan usable
    }
    LAGraph_CFPQ_RSM_FreePrepared(&plan);
    CHECK(plan == NULL);
    LAGraph_CFPQ_RSM_FreePrepared(&plan);
    LAGraph_CFPQ_RSM_FreePrepared(NULL);
    oracle_view_free(&ov);
}

static void allocation_failures(Case *c, bool fail_prepare)
{
    RSM rsm;
    case_rsm(c, &rsm);
    GrB_Index source = 0;
    bool succeeded = false;
    for (int budget = 0; budget < 2048; ++budget)
    {
        int64_t before = LG_nmalloc;
        LAGraph_CFPQ_RSM_Plan *plan = NULL;
        GrB_Vector out = GrB_NULL;
        if (!fail_prepare)
            CHECK(LAGraph_CFPQ_RSM_Prepare(&plan, &rsm, c->graph, c->V, msg) == GrB_SUCCESS);
        LG_brutal = budget;
        int info = fail_prepare
                 ? LAGraph_CFPQ_RSM_Prepare(&plan, &rsm, c->graph, c->V, msg)
                 : LAGraph_CFPQ_RSM_Execute(&out, plan, &source, 1, msg);
        LG_brutal = -1;
        if (info != GrB_SUCCESS)
        {
            CHECK(fail_prepare ? plan == NULL : out == GrB_NULL);
            if (!fail_prepare) // a failed execution cannot corrupt immutable support
                CHECK(LAGraph_CFPQ_RSM_Execute(&out, plan, &source, 1, msg) == GrB_SUCCESS);
        }
        GrB_free(&out);
        LAGraph_CFPQ_RSM_FreePrepared(&plan);
        CHECK(LG_nmalloc == before);
        if (info == GrB_SUCCESS)
        {
            printf("prepared %s: all budgets 0..%d tested; no retained allocations\n",
                   fail_prepare ? "construction" : "execution", budget);
            succeeded = true;
            break;
        }
    }
    CHECK(succeeded);
}

int main(void)
{
    CHECK(LG_brutal_setup(msg) == GrB_SUCCESS);
    freeze_graphblas_environment(); // JIT only; OpenMP settings are inherited
    GrB_Vector out = GrB_NULL;
    CHECK(LAGraph_CFPQ_RSM_Execute(&out, NULL, NULL, 0, msg) == GrB_NULL_POINTER);
    CHECK(out == GrB_NULL);
    LAGraph_CFPQ_RSM_Plan *bad = NULL;
    CHECK(LAGraph_CFPQ_RSM_Prepare(&bad, NULL, NULL, 0, msg) == GrB_NULL_POINTER);
    CHECK(bad == NULL);
    Case c;
    CHECK(build_alloc_case(&c)); // forward grouping, ordinary transfers, nullable recursion
    reuse_case(&c);
    allocation_failures(&c, true);
    allocation_failures(&c, false);
    case_free(&c);
    CHECK(case_new(&c, 4, 4, 3, 1, 0)); // dominant dual family: 0/1/2 -> 3
    for (GrB_Index t = 0; t < 3; ++t)
    {
        CHECK(case_term(&c, t, t, 3));
        CHECK(case_graph(&c, t, 0, t+1));
    }
    CHECK(case_final(&c, 0, 3));
    reuse_case(&c);
    case_free(&c);
    CHECK(case_new(&c, 1, 4, 0, 1, 0)); // no terminals/groups, nullable start box
    CHECK(case_final(&c, 0, 0));
    reuse_case(&c);
    case_free(&c);
    CHECK(LG_brutal_teardown(msg) == GrB_SUCCESS);
    printf("prepared checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures != 0;
}
