# Experiment threading policy

The user requires runtime-controlled multithreading. For real builds, tests,
timing and profiling, never set, unset or override `OMP_NUM_THREADS`,
`OMP_THREAD_LIMIT`, or other OpenMP environment variables. Never impose a
GraphBLAS thread count. Preserve the inherited environment and let OpenMP and
GraphBLAS choose parallelism. Record inherited settings and CPU affinity in
provenance; do not infer actual active threads from those settings.

Synthetic tests may use isolated fixture environments to verify that the
tooling preserves both inherited values and absent variables.
