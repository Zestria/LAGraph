# Registered kernel diffs

For the reviewed candidates use `active/p1_review.patch` through
`active/p4_review.patch`. These remain standalone, baseline-relative patches;
their exact sources and original parents are registered. The original p1–p4
and cumulative stack are preserved unchanged. See [PATCH_REVIEW.md](../PATCH_REVIEW.md)
for changes and validation. The stack does not include the review changes.

Use `../variants/REGISTRY.csv` to select a diff and its exact `diff_parent`; never guess the parent from the filename. `active/` contains baseline-relative standalone p1–p4 plus the cumulative stack. `archive/` contains every historical source diff. All registered diffs have repository-relative `a/experimental/algorithm/LAGraph_CFPQ_RSM.c` and `b/...` labels and apply with `patch -p1` in a scratch repository layout. Baseline has an empty identity diff.

Run `make verify` in the lab root to apply every diff and compare every resulting hash. Do not concatenate standalone patches blindly: they can share helpers and require a reviewed combination. Use the registered `stack_p1234` snapshot for the preserved cumulative implementation. Test changes live in the repository/backup separately from the kernel-only runner patches. The old absolute-path DeepSeek diffs in legacy build output are superseded evidence; they are not experiment inputs.
