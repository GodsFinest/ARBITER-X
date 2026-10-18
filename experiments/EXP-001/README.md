# EXP-001: Does order-book imbalance predict monetizable short-horizon movement?

Run: `arbiter research run EXP-001 [--csv REAL.csv]`
Reproduce: `arbiter research reproduce EXP-001`

Default dataset is the biased synthetic control (KNOWN alpha, seed 2026):
validates that the full loop, features → labels → OOS stats → queue-aware
execution → costs → net P&L → artifact, can find edge when it exists. The
unbiased control (`research evaluate --synthetic --seed 2026`) must read
INCONCLUSIVE with negative net P&L; if it ever reads SUPPORTED, the pipeline
— not the market, is broken.

Artifacts: `manifest.json` (dataset/config/seed/version hashes),
`results.json` (metrics, counters, findings, `result_hash`), `report.md`
(full Question→Reproducibility narrative).