# CFPQ RSM optimization history

Canonical snapshot: [2026-11-03 experiment archive](experimental/algorithm/cfpq_rsm_lab/archive/2026-11-03/README.md).

The archive preserves all available experiment records: 16 recent optimization variants plus the corrected baseline, historical I/S experiments and combinations, exact sources/parent patches, raw results, logs, commands, correctness reports, profile evidence and checksums. It is outside the ignored build tree; original working files and runs remain untouched.

Recent status: DS-20/21/22 completed 1/10/100-source comparisons and were slower overall. R9/R10 completed all 1/10-source comparisons for 13 candidates with matching counts/hashes; 100-source measurements are partial. config_block and empty/first-contribution handling are promising; joint filtering is Java-specific. No optimization has been promoted or combined based on these results. Inspect the archived workload results rather than treating small timing differences as universal speedups.

Historical experiments used different baselines/harnesses; their recorded statuses must not be reinterpreted as acceptance under the corrected reference. D4/D5 were discarded before implementation. Do not repeat accepted checks or continue 100-source runs merely to fill the archive.

Before a new optimization, consult the catalogue and retain a new immutable snapshot when results change. Full checksum verification and recovery instructions are in the archive README. This is a local archive, not a remote backup or Git commit.
