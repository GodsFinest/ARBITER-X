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
replication count.

- Design and interpretation: [flagship_allocation.md](EXPERIMENTS/flagship_allocation.md)
- Definitions: `configs/flagship_clustered.axexp`
- Records: `experiments/FLAGSHIP-*`

## Optional research extension

General World grids and EXP-001/002/003 records moved unchanged to
[ARBITER-X-LAB](https://github.com/GodsFinest/ARBITER-X-LAB).
They are not required to build or reproduce the core flagship.
