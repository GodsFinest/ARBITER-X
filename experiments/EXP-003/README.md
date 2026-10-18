# EXP-003: Does deeper-book information add value beyond top-of-book imbalance?

Run: `arbiter research run EXP-003 [--csv REAL.csv]`
Reproduce: `arbiter research reproduce EXP-003`

Compares imb1-only rules against multi-level imbalance + OFI + microprice
models (logistic, stump) on identical TEST folds: predictive gap (AUC/acc/IC),
stability (walk-forward), execution P&L gap, and inference-cost budget.
Ablation verdict: the full-vs-imb1 gap is the incremental value; the stump
row separates "depth helped" from "nonlinearity helped". If logistic cannot
beat imb1 after costs, that, not a leaderboard — is the recorded result.