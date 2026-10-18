# Experiment registry

ARBITER-X keeps experiment definitions separate from generated runtime output.
Definitions live in `configs/`, reproducible result records live in
`experiments/`, and ad-hoc output belongs in ignored `artifacts/`.

## Flagship: fixed-total latency allocation

The flagship asks whether execution economics depend only on total latency or
also on where that latency occurs. Observation, decision, and order-transit
delays are varied while their sum remains fixed.

Controls include paired macro-seeds, memoryless and clustered flow, reactive
and frozen observation modes, engine-measured stage timing, permutation tests,
and Holm correction. The committed result is uncertain at the available
replication count; no positive claim is manufactured from the point estimates.

- Design and interpretation: [flagship_allocation.md](EXPERIMENTS/flagship_allocation.md)
- Definitions: `configs/flagship_clustered.axexp`
- Records: `experiments/FLAGSHIP-*`

## Supporting world experiments

`latency_grid`, `latency_sweep`, and `network_grid` validate divergence,
network recovery, and experiment plumbing while varying total latency or
transport conditions. `configs/fee_grid.axexp` checks the exact fee-to-filled-lot
accounting relationship. These are supporting tests, not evidence for the
fixed-total allocation hypothesis.

Run a file-defined grid and reproduce its output with:

```sh
./build/arbiter experiment --config configs/fee_grid.axexp
./build/arbiter experiment --reproduce artifacts/experiment_fee_grid.json
```

## Supporting research studies

- `EXP-001`: whether book imbalance predicts monetizable short-horizon moves.
- `EXP-002`: how execution latency erodes a fixed prediction stream.
- `EXP-003`: whether deeper-book features add value beyond top-of-book input.

Each tracked experiment directory contains a manifest, machine-readable result,
and generated report. The result can be rebuilt from the manifest:

```sh
./build/arbiter research reproduce EXP-001
./build/arbiter research reproduce EXP-002
./build/arbiter research reproduce EXP-003
```

See [RESEARCH.md](RESEARCH.md) for the evaluation contract and
[REPRODUCIBILITY.md](REPRODUCIBILITY.md) for artifact guarantees.