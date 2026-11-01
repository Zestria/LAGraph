# Variant patches

A patch is a unified diff against `../LAGraph_CFPQ_RSM.c`. It changes only the
lines needed by one optimization; the baseline file itself stays untouched.

## Add a variant

```bash
cp experimental/algorithm/LAGraph_CFPQ_RSM.c /tmp/A.c
$EDITOR /tmp/A.c
diff -u --label a/LAGraph_CFPQ_RSM.c --label b/LAGraph_CFPQ_RSM.c \
  experimental/algorithm/LAGraph_CFPQ_RSM.c /tmp/A.c \
  > experimental/algorithm/cfpq_rsm_lab/patches/A.patch
```

Then run:

```bash
./experimental/algorithm/cfpq_rsm_lab/run.py --variants baseline,A
```

The `a/LAGraph_CFPQ_RSM.c` / `b/LAGraph_CFPQ_RSM.c` labels are important: the
runner copies the baseline into a temporary directory and applies the patch
with `patch -p1`.

## Combine variants

`A+B` applies `A.patch` and then `B.patch`. If the patches conflict, the runner
stops and tells you which patch failed. In that case put a manually combined
full source file in `../variants/A+B.c`; the runner uses it as is.

## Example

`example_reshape_inplace.patch` replaces `GxB_Matrix_reshapeDup` + free with
`GxB_Matrix_reshape`. It exists only to exercise the build/run/compare path and
is not a claim that the in-place variant is faster on real inputs.
