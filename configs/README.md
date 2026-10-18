# configs/

File-driven experiment definitions (`.axexp`): the sectioned key=value
grammar documented in docs/COUNTERFACTUALS.md.

- `flagship_clustered.axexp` — the FLAGSHIP fixed-total latency-allocation
  study under clustered (self-exciting) flow. Run with
  `arbiter flagship --config configs/flagship_clustered.axexp`; the
  Poisson placebo is the same file with `flow_model = poisson`.
- `latency_grid.axexp` — the classic latency grid as a file (varies the
  TOTAL; infrastructure validation, not flagship evidence)
- `fee_grid.axexp` — fee counterfactual (does a per-lot fee flip wide-quoting
  profitability?)

Run: `arbiter experiment --config configs/fee_grid.axexp`
Reproduce: `arbiter experiment --reproduce artifacts/experiment_fee_grid.json`
Flagship: `arbiter flagship --config configs/flagship_clustered.axexp`