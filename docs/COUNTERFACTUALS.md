# Counterfactuals

## The mechanism

A counterfactual experiment runs N Worlds with the **same master seed** and
configurations that differ in exactly the variable(s) under study. Because
every random draw comes from `(master_seed, stream_id)` streams, variants
consume the same unit variates: changing a latency changes *delays*, not the
underlying randomness. Histories are therefore identical until the changed
variable reorders events, and the state-hash timeline localizes that
moment.

Common random numbers reduce variance by pairing runs. They do
not by themselves make the estimate causal; the causal reading is always
"paired counterfactual estimate under the simulator's structural
assumptions".

## Fixed-world vs reactive-world (interference)

The simulator supports two interference modes:

* **reactive** (`bots_observe_shadow = false`, default): bots observe the
  live touch, including strategy quotes, and their quoting reacts to the
  strategies. The measured effect of a strategy-side change includes market
  feedback. This is the flagship mode (it is the more realistic structure).
* **frozen** (`bots_observe_shadow = true`): bots observe a shadow touch
  computed without strategy orders, so their quoting input is exogenous
  to the strategies. In this mode a strategy-side counterfactual faces a
  flow process that cannot react to it (SUTVA holds by construction for the
  observation channel).

Frozen mode does not change matching. Bot takers still execute against
whatever rests at the touch, including strategy orders, a crossed market
is genuine interference, not an observation artifact. The fixed/frozen
comparison therefore estimates the observation-channel feedback part of
interference bias in this simulator, not real-market SUTBA violations.

## The flagship: fixed-total latency allocation

The flagship experiment (docs/EXPERIMENTS/flagship_allocation.md,
`arbiter flagship`) is not a latency sweep. Total latency is held fixed
(typed `LatencyAllocation`, invariant sum == total) while the allocation
across the trader-side stages (observation `md_transit`, `decision`,
`order_transit`) moves along the md-share slice of the simplex.

H0: E[Y | allocation] = f(total only) — placement is irrelevant.
H1: stage placement matters (∂E[Y]/∂l_md != ∂E[Y]/∂l_decision somewhere).

Controls:

* **flow regime** (`--flow poisson|clustered`): the clustered regime arms
  taker self-excitation k=0.9 (see docs/SIMULATION.md); the Poisson regime
  is the falsification placebo (k=0, exactly the historical bit-stream).
* **interference mode** (reactive default; `--frozen` re-run quantifies
  observation-channel feedback).
* **pairing**: cells are paired by macro-seed; the seed is the unit of
  replication (within-world events are not treated as independent).
* **statistics**: per-contrast paired differences across seeds, sign
  permutation p-values, Holm correction across the contrast family, HAC CI
  reported alongside iid CI, economic materiality threshold stated up front
  (ticks*qty per filled lot). Holm is the correction applied in the flagship;
  Bonferroni and BH-FDR are implemented and unit-tested, available for future
  multi-family studies.
* **engine-verified intervention**: per-cell realized stage latencies are
  recorded from the engine's own stage trace (placement provably took
  hold; test `stage_placement_is_realized_in_engine_timestamps`).

Artifacts (`experiments/FLAGSHIP-*/results.json`) record the complete
design: definition hash, seeds, regime, interference mode, allocations,
per-seed economics + state hashes, contrast family, findings. Reproduce
bit-for-bit when compiler and platform provenance match:
`arbiter flagship --reproduce <results.json>`.

## Prior latency experiments (superseded as evidence for H0-allocation)

The supporting latency grid/sweep (`arbiter experiment`, `configs/*.axexp`)
vary TOTAL latency. They answer "does
the total matter", which is a different question from the flagship's "does
placement matter at a fixed total". They remain valid infrastructure
validation and are kept as supporting records; they must NOT be cited as
evidence for or against allocation non-fungibility.

Classic measured results (seed 2026, re-baselined after the engine's
transport/risk-ledger repairs; regenerate with
`arbiter experiment --seed 2026 --ms 40`, never hand-edit):

| variant | events | trades | MM fills | MM PnL (ticks*qty) | first divergent checkpoint |
|---|---|---|---|---|---|
| baseline_800ns | 14183 | 1014 | 14 | 150 | identical |
| fast_300ns | 14182 | 1014 | 14 | 150 | @checkpoint(event 512) |
| slow_3000ns | 14178 | 1022 | 11 | 125 | @checkpoint(event 512) |
| jittery_md | 14354 | 1014 | 14 | 150 | @checkpoint(event 256) |
| lossy_md_1pct | 14227 | 1014 | 14 | 150 | @checkpoint(event 256) |

Reading: state divergence does not imply economic divergence. The reported
value is the first differing hash CHECKPOINT (an upper bound on where the
histories split), not the exact causal event.

## Queue-model accuracy

See docs/EXECUTION_MODEL.md "Queue-position model": the live metric reports
the point estimate AND the structural ambiguity bound
(`mm_ambiguous_fill_qty`). Real fills are authoritative for economics.

## Experiment configuration

Experiments are defined in two equivalent ways:

1. **File-driven** (preferred): a `.axexp` file using the sectioned
   `key = value` grammar, run with `arbiter experiment --config FILE`.
   Unknown keys are LOUD failures, never ignored. The same override-table
   mechanism covers the new research variables
   (`sim.taker_self_excitation`, `bots_observe_shadow`, `stage_tracing`,
   `momentum.id_base`).

2. **Built-in families**: `arbiter experiment --name
   latency_grid|latency_sweep|network_grid`.

Every artifact records the full definition (seed, horizon,
definition_hash, per-variant override sets) so `arbiter experiment
--reproduce ARTIFACT.json` re-runs the exact grid and verifies every hash
bit-for-bit when compiler and platform provenance match.
