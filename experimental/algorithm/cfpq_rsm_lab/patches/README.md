# Registered kernel diffs

`active/` contains the empty baseline identity patch, `p1_full.patch`, and
`p1_full_clean.patch`. The `p1_full` patch applies to the archived `p1_review`
source. The `p1_full_clean` patch applies to `p1_full`. Previous
patches live in `archive/` alongside their recorded ancestry in
[the registry](../variants/REGISTRY.csv).

Use the registry's `diff_parent` to select the exact input source. All registered
diffs use repository-relative `a/experimental/algorithm/LAGraph_CFPQ_RSM.c`
and `b/...` labels and apply with `patch -p1` in a scratch repository layout.

Run `make verify` in the lab root to apply every diff and compare every resulting hash. Do not concatenate standalone patches blindly: they can share helpers and require a reviewed combination. Use the registered `stack_p1234` snapshot for the preserved cumulative implementation. Test changes live in the repository/backup separately from the kernel-only runner patches. The old absolute-path DeepSeek diffs in legacy build output are superseded evidence; they are not experiment inputs.
