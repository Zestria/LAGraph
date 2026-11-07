# CFPQ RSM overnight summary

- Run root: `/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/build/runs/overnight-2026-11-02_03-36-52`
- Generated: 2026-11-02T08:40:41.737147+03:00
- Config: `/home/hollow/repo/LAGraph/experimental/algorithm/cfpq_rsm_lab/configs/lab.csv` (7 graph/grammar pairs)
- Expected rows: 3 variants x 3 source sizes x 7 workloads = 63
- `MATCH` requires all 30 chunks x 5 measured rounds for the workload, matching counts and hashes, an `ok` row in that comparison's `summary.csv`, and a zero run exit code.
- Times are milliseconds: median of the per-chunk median times. `ratio` is the median across chunks of `T_variant / T_first_baseline` (smaller is better).
- Memory is **process memory**: the maximum `max_memory_kb` over retained measured rows for the cfg_bench process, converted to MiB.
- No winner is selected or combined; failed/incomplete rows are excluded from the descriptive aggregates. Partial timings are kept but marked incomplete.

## Status overview

| variant | sources | MATCH | FAIL | NOT_RUN | median ratio (MATCH rows) |
| --- | --- | --- | --- | --- | --- |
| ds20 | 1 | 7 | 0 | 0 | 1.074 |
| ds20 | 10 | 7 | 0 | 0 | 1.115 |
| ds20 | 100 | 7 | 0 | 0 | 1.122 |
| ds21 | 1 | 7 | 0 | 0 | 1.036 |
| ds21 | 10 | 7 | 0 | 0 | 1.032 |
| ds21 | 100 | 7 | 0 | 0 | 1.034 |
| ds22 | 1 | 7 | 0 | 0 | 1.016 |
| ds22 | 10 | 7 | 0 | 0 | 1.035 |
| ds22 | 100 | 7 | 0 | 0 | 1.277 |

## Per-workload results

| variant | graph | grammar | sources | status | chunks | rounds | exit | baseline ms | variant ms | ratio | baseline mem MiB | variant mem MiB | timing | reason |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ds20 | avrora.g | java_points_to.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 30.637 | 30.711 | 1.004139 | 86.4 | 86.6 | yes |  |
| ds20 | eclass.g | nested_parentheses_subClassOf_type.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 0.717 | 0.756 | 1.059079 | 112.5 | 113.1 | yes |  |
| ds20 | go.g | nested_parentheses_subClassOf_type.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 7.551 | 7.757 | 1.027598 | 202.6 | 204.0 | yes |  |
| ds20 | go_hierarchy.g | nested_parentheses_subClassOf_type.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 0.609 | 0.655 | 1.075145 | 115.4 | 115.5 | yes |  |
| ds20 | mm.g | c_alias.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 1.529 | 1.634 | 1.074117 | 283.6 | 284.3 | yes |  |
| ds20 | init.g | c_alias.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 1.888 | 2.044 | 1.084292 | 275.9 | 276.1 | yes |  |
| ds20 | block.g | c_alias.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 1.881 | 2.045 | 1.085214 | 354.6 | 355.4 | yes |  |
| ds20 | avrora.g | java_points_to.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 761.073 | 764.102 | 1.003062 | 89.0 | 88.9 | yes |  |
| ds20 | eclass.g | nested_parentheses_subClassOf_type.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 1.219 | 1.308 | 1.073078 | 118.5 | 115.6 | yes |  |
| ds20 | go.g | nested_parentheses_subClassOf_type.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 9.643 | 9.815 | 1.028971 | 209.4 | 206.4 | yes |  |
| ds20 | go_hierarchy.g | nested_parentheses_subClassOf_type.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 1.794 | 2.328 | 1.277152 | 123.0 | 120.6 | yes |  |
| ds20 | mm.g | c_alias.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 9.020 | 9.975 | 1.114747 | 287.7 | 284.7 | yes |  |
| ds20 | init.g | c_alias.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 7.711 | 8.605 | 1.115329 | 280.5 | 276.4 | yes |  |
| ds20 | block.g | c_alias.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 7.611 | 8.555 | 1.119326 | 357.3 | 356.3 | yes |  |
| ds20 | avrora.g | java_points_to.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 5461.347 | 5560.146 | 1.011948 | 89.2 | 89.3 | yes |  |
| ds20 | eclass.g | nested_parentheses_subClassOf_type.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 2.489 | 2.732 | 1.098875 | 131.7 | 140.3 | yes |  |
| ds20 | go.g | nested_parentheses_subClassOf_type.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 22.939 | 37.038 | 1.629901 | 211.1 | 214.7 | yes |  |
| ds20 | go_hierarchy.g | nested_parentheses_subClassOf_type.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 19.937 | 26.837 | 1.328395 | 173.1 | 183.6 | yes |  |
| ds20 | mm.g | c_alias.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 25.939 | 32.512 | 1.123512 | 287.4 | 285.8 | yes |  |
| ds20 | init.g | c_alias.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 22.043 | 24.630 | 1.114632 | 280.3 | 278.1 | yes |  |
| ds20 | block.g | c_alias.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 23.274 | 25.968 | 1.121655 | 358.9 | 357.3 | yes |  |
| ds21 | avrora.g | java_points_to.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 30.631 | 30.898 | 1.002488 | 86.4 | 86.6 | yes |  |
| ds21 | eclass.g | nested_parentheses_subClassOf_type.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 0.714 | 0.725 | 1.022234 | 114.3 | 113.6 | yes |  |
| ds21 | go.g | nested_parentheses_subClassOf_type.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 7.709 | 7.773 | 1.013011 | 205.3 | 204.4 | yes |  |
| ds21 | go_hierarchy.g | nested_parentheses_subClassOf_type.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 0.604 | 0.625 | 1.035716 | 117.9 | 115.6 | yes |  |
| ds21 | mm.g | c_alias.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 1.511 | 1.608 | 1.052142 | 287.4 | 285.1 | yes |  |
| ds21 | init.g | c_alias.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 1.882 | 2.013 | 1.056947 | 280.2 | 277.9 | yes |  |
| ds21 | block.g | c_alias.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 1.865 | 2.004 | 1.055988 | 356.5 | 354.9 | yes |  |
| ds21 | avrora.g | java_points_to.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 762.353 | 760.161 | 0.999319 | 89.0 | 89.0 | yes |  |
| ds21 | eclass.g | nested_parentheses_subClassOf_type.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 1.227 | 1.269 | 1.032089 | 118.1 | 115.7 | yes |  |
| ds21 | go.g | nested_parentheses_subClassOf_type.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 9.648 | 9.616 | 0.996926 | 207.1 | 207.3 | yes |  |
| ds21 | go_hierarchy.g | nested_parentheses_subClassOf_type.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 1.736 | 1.758 | 1.017783 | 123.0 | 120.6 | yes |  |
| ds21 | mm.g | c_alias.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 8.991 | 9.422 | 1.047310 | 289.4 | 285.7 | yes |  |
| ds21 | init.g | c_alias.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 7.730 | 8.088 | 1.053514 | 279.0 | 278.4 | yes |  |
| ds21 | block.g | c_alias.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 7.574 | 7.967 | 1.048706 | 358.9 | 356.3 | yes |  |
| ds21 | avrora.g | java_points_to.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 5484.251 | 5480.904 | 1.002029 | 89.6 | 92.0 | yes |  |
| ds21 | eclass.g | nested_parentheses_subClassOf_type.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 2.479 | 2.576 | 1.038815 | 133.6 | 131.7 | yes |  |
| ds21 | go.g | nested_parentheses_subClassOf_type.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 23.010 | 23.369 | 1.016865 | 212.4 | 211.2 | yes |  |
| ds21 | go_hierarchy.g | nested_parentheses_subClassOf_type.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 20.142 | 20.159 | 1.006128 | 165.9 | 164.2 | yes |  |
| ds21 | mm.g | c_alias.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 25.871 | 26.887 | 1.036586 | 288.4 | 286.4 | yes |  |
| ds21 | init.g | c_alias.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 22.087 | 22.846 | 1.035262 | 280.2 | 279.7 | yes |  |
| ds21 | block.g | c_alias.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 23.340 | 24.238 | 1.034023 | 358.6 | 357.0 | yes |  |
| ds22 | avrora.g | java_points_to.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 30.699 | 30.626 | 0.996935 | 87.1 | 86.6 | yes |  |
| ds22 | eclass.g | nested_parentheses_subClassOf_type.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 0.711 | 0.765 | 1.075546 | 114.1 | 112.7 | yes |  |
| ds22 | go.g | nested_parentheses_subClassOf_type.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 7.731 | 7.677 | 0.993670 | 207.0 | 203.3 | yes |  |
| ds22 | go_hierarchy.g | nested_parentheses_subClassOf_type.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 0.604 | 0.607 | 1.006885 | 117.7 | 115.5 | yes |  |
| ds22 | mm.g | c_alias.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 1.497 | 1.550 | 1.029646 | 287.3 | 285.0 | yes |  |
| ds22 | init.g | c_alias.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 1.871 | 1.904 | 1.016050 | 280.1 | 277.8 | yes |  |
| ds22 | block.g | c_alias.cnf | 1 | MATCH | 30/30 | 150/150 | 0 | 1.855 | 1.898 | 1.019661 | 356.4 | 354.7 | yes |  |
| ds22 | avrora.g | java_points_to.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 758.346 | 775.769 | 1.001479 | 88.9 | 88.9 | yes |  |
| ds22 | eclass.g | nested_parentheses_subClassOf_type.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 1.223 | 1.255 | 1.030403 | 117.5 | 116.1 | yes |  |
| ds22 | go.g | nested_parentheses_subClassOf_type.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 9.553 | 9.703 | 1.017469 | 206.8 | 206.7 | yes |  |
| ds22 | go_hierarchy.g | nested_parentheses_subClassOf_type.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 1.738 | 1.805 | 1.035470 | 123.0 | 120.5 | yes |  |
| ds22 | mm.g | c_alias.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 8.952 | 9.344 | 1.050599 | 286.7 | 285.3 | yes |  |
| ds22 | init.g | c_alias.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 7.651 | 8.039 | 1.047610 | 279.0 | 277.8 | yes |  |
| ds22 | block.g | c_alias.cnf | 10 | MATCH | 30/30 | 150/150 | 0 | 7.627 | 7.968 | 1.045341 | 360.5 | 359.1 | yes |  |
| ds22 | avrora.g | java_points_to.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 5466.443 | 5507.914 | 1.003069 | 89.4 | 89.0 | yes |  |
| ds22 | eclass.g | nested_parentheses_subClassOf_type.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 2.478 | 3.025 | 1.222370 | 133.1 | 133.2 | yes |  |
| ds22 | go.g | nested_parentheses_subClassOf_type.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 22.981 | 30.180 | 1.277080 | 211.9 | 208.9 | yes |  |
| ds22 | go_hierarchy.g | nested_parentheses_subClassOf_type.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 20.087 | 20.486 | 1.023711 | 165.3 | 163.2 | yes |  |
| ds22 | mm.g | c_alias.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 25.996 | 41.930 | 1.608600 | 288.4 | 285.3 | yes |  |
| ds22 | init.g | c_alias.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 22.007 | 35.768 | 1.598785 | 281.7 | 279.1 | yes |  |
| ds22 | block.g | c_alias.cnf | 100 | MATCH | 30/30 | 150/150 | 0 | 23.387 | 36.579 | 1.559200 | 358.0 | 358.7 | yes |  |

## Duplicate-baseline noise context

Per-chunk ratio of the second baseline copy to the first (`T_baseline#2 / T_baseline`), using per-chunk medians. A ratio of 1.0 means identical timing; the spread is measurement noise, not a variant result.

| sources | chunks compared | min | median | max |
| --- | --- | --- | --- | --- |
| 1 | 630 | 0.9503 | 0.9994 | 1.0484 |
| 10 | 630 | 0.8609 | 0.9985 | 1.1865 |
| 100 | 630 | 0.9707 | 1.0001 | 1.0407 |
| all | 1890 | 0.8609 | 0.9995 | 1.1865 |

## Failures and missing data

None: all 63 rows are MATCH.

