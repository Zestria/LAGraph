# Standalone source derivation

The reviewed revisions `p1_review`–`p4_review` descend from the corresponding
original standalone snapshots; their `diff_parent` remains `baseline`. They
do not include each other's algorithm changes. Original snapshots and the
stack remain immutable. [PATCH_REVIEW.md](../PATCH_REVIEW.md) records the review.

Every active p1–p4 source has the same baseline, commit ba6e4308 / c7efb94f21bb. The full source hashes and reproducible baseline-relative diffs are in REGISTRY.csv. New snapshots were developed in scratch storage, validated, then published once with hash-suffixed filenames and mode 0444.

- **p1** is DeepSeek's first candidate unchanged (d586b953e263). It includes compact grouping and the ordinary terminal fallback.
- **p2** ports the cumulative compaction helper and its necessary tuple/allocation helpers onto the original baseline. Only the private RSM remapping/setup and cleanup are integrated; the baseline terminal and nonterminal evaluation loops remain. No grouped-transfer helper is enabled.
- **p3** ports destination-local waiting boxes, their route/shift/product/scatter helpers and cleanup onto the baseline. Terminal preprocessing stays the baseline's per-label path and there is no state trimming.
- **p4** ports fusion eligibility and fused scatter onto the baseline. Waiting matrices retain the original QV × V coordinates. Identity destination metadata carries each continuation's terminal/destination; a full-size return product is consumed immediately before reaching the persistent accumulator. It does not include p3's compact waiting domain or p1/p2.
- **stack_p1234** is the original final DeepSeek cumulative source unchanged (4e3a96853528). It remains an experimental comparison, not production. Intermediate cumulative p12/p123 sources are archived explicitly, not relabeled as standalone p2/p3.

The ports preserve the original behavior on the expanded deterministic regressions and 200 seeded oracle cases; loaded-candidate reduced checks provide additional practical evidence. These checks do not establish a speedup or correctness for all possible inputs. Fusion's state-dimension and per-continuation-product limitations are deliberately retained and documented rather than silently changed while rebuilding tooling. There was no new full sanitizer/allocation sweep for these ports.
