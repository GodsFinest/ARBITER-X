# Flagship: latency-stage non-fungibility (fixed total budget)

**This is a paired counterfactual estimate under the simulator's structural assumptions — not a causal claim about real markets.**

## Research question

For a FIXED total trader-side latency budget L = 8000 ns, does the ALLOCATION of L across (observation md_transit, decision, order_transit) change execution economics?

H0: only the total matters (E[Y|allocation] = f(L)). H1: placement matters (stage marginal effects differ).

## Intervention

ONE-DIMENSIONAL SLICE of the 3-stage allocation simplex: the md-share axis {0..1} with the action stages (decision, order_transit) split evenly — NOT the full simplex (interior decision/order trade-offs are not explored). Identical total in every cell, enforced by the typed `LatencyAllocation` invariant; engine-measured realized stage latencies recorded per cell.

## Market process

flow_model=poisson (k=0: the memoryless null process, bit-identical to the historical simulator behavior)

**Regime-contrast scope (limitation):** the two flow regimes differ in the taker process's dependence structure AND consequently in realized intensity and trade-sign runs (measured: ~7x trade-count difference at k=0.9; the momentum archetype only participates when sign runs emerge, k>=~0.85). The placebo therefore shows "allocation has no effect under THIS null market"; it does NOT isolate which aspect of the clustered process (dependence vs intensity) would drive any clustered-regime effect.

## Replication

Seed is the unit of replication (independent worlds; within-world events are never treated as independent). Cells are PAIRED by seed (common random numbers): same master seed, same named streams.

## Interference mode

reactive (bots observe the live touch and react to strategy quotes; the measured effect includes market feedback — interference sensitivity is probed by re-running frozen)

| cell | allocation | MM PnL (ticks*qty) | MM filled lots | MM markout | Taker PnL | Taker filled lots |
|---|---|---|---|---|---|---|
| md0.000 | md+0,dec+4000,ord+4000 | 168.8 | 171.9 | +0.98 | 0.0 | 0.0 |
| md0.125 | md+1000,dec+3500,ord+3500 | 168.8 | 171.9 | +0.98 | 0.0 | 0.0 |
| md0.250 | md+2000,dec+3000,ord+3000 | 168.8 | 171.9 | +0.98 | 0.0 | 0.0 |
| md0.375 | md+3000,dec+2500,ord+2500 | 168.8 | 171.9 | +0.98 | 0.0 | 0.0 |
| md0.500 | md+4000,dec+2000,ord+2000 | 168.8 | 171.9 | +0.98 | 0.0 | 0.0 |
| md0.625 | md+5000,dec+1500,ord+1500 | 168.8 | 171.9 | +0.98 | 0.0 | 0.0 |
| md0.750 | md+6000,dec+1000,ord+1000 | 168.8 | 171.9 | +0.98 | 0.0 | 0.0 |
| md0.875 | md+7000,dec+500,ord+500 | 168.8 | 171.9 | +0.98 | 0.0 | 0.0 |
| md1.000 | md+8000,dec+0,ord+0 | 168.8 | 171.9 | +0.98 | 0.0 | 0.0 |

## Paired contrasts (MM PnL, b − a, per seed)

| contrast | mean diff | 95% CI (iid) | p (perm) | p (Holm) | per-lot | significant | material |
|---|---|---|---|---|---|---|---|
| md0.000 → md0.125 | +0.0 | [+0.0, +0.0] | 1.0000 | 1.0000 | +0.00 | no | no |
| md0.125 → md0.250 | +0.0 | [+0.0, +0.0] | 1.0000 | 1.0000 | +0.00 | no | no |
| md0.250 → md0.375 | +0.0 | [+0.0, +0.0] | 1.0000 | 1.0000 | +0.00 | no | no |
| md0.375 → md0.500 | +0.0 | [+0.0, +0.0] | 1.0000 | 1.0000 | +0.00 | no | no |
| md0.500 → md0.625 | +0.0 | [+0.0, +0.0] | 1.0000 | 1.0000 | +0.00 | no | no |
| md0.625 → md0.750 | +0.0 | [+0.0, +0.0] | 1.0000 | 1.0000 | +0.00 | no | no |
| md0.750 → md0.875 | +0.0 | [+0.0, +0.0] | 1.0000 | 1.0000 | +0.00 | no | no |
| md0.875 → md1.000 | +0.0 | [+0.0, +0.0] | 1.0000 | 1.0000 | +0.00 | no | no |
| md0.000 → md1.000 | +0.0 | [+0.0, +0.0] | 1.0000 | 1.0000 | +0.00 | no | no |

- primary contrast (md0.000 -> md1.000): mean paired MM PnL diff +0.0 ticks*qty (per-lot +0.00), Holm p=1.0000, not significant
- economic materiality (primary): not material (threshold 1.00 ticks*qty/fill)
- MM filled lots across the slice: 171.9 (all-observation) vs 171.9 (all-action): fill mechanics invariant to placement
- taker (momentum) PnL across the slice: 0.0 -> 0.0 (invariant)
- taker primary-contrast paired PnL diff: +0.0 (se 0.0, perm p=1.0000) — not significant
- H0 at the primary contrast: NOT REJECTED (placement effect small and not significant at this budget/regime — a valid negative result)

## Falsification status

- Poisson null: run the same grid with `flow_model=poisson`. Interpretation scope: if an allocation effect existed in the null market it would indicate placement sensitivity WITHOUT clustered flow (mechanism contradiction); a null result here is necessary but NOT sufficient for the clustered-flow mechanism (the regimes also differ in intensity — see the regime-contrast scope above).
- Interference: re-run with `bots_observe_shadow=true`; a large gap between reactive and frozen effects means the interpretation is unstable.
- Kill conditions: no adjacent-pair materiality under clustered flow at the primary contrast; or materiality driven by one seed; or reference/optimized book disagreement (checked separately in the differential suite).

## Reproducibility

`arbiter flagship --config <file>` re-runs this design; `arbiter flagship --reproduce <artifact.json>` verifies every recorded hash bit-for-bit. definition_hash covers the full design (grid, seeds, regime, horizon, thresholds).