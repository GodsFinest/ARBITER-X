# Flagship: latency-stage non-fungibility (fixed total budget)

**This is a paired counterfactual estimate under the simulator's structural assumptions — not a causal claim about real markets.**

## Research question

For a FIXED total trader-side latency budget L = 8000 ns, does the ALLOCATION of L across (observation md_transit, decision, order_transit) change execution economics?

H0: only the total matters (E[Y|allocation] = f(L)). H1: placement matters (stage marginal effects differ).

## Intervention

ONE-DIMENSIONAL SLICE of the 3-stage allocation simplex: the md-share axis {0..1} with the action stages (decision, order_transit) split evenly — NOT the full simplex (interior decision/order trade-offs are not explored). Identical total in every cell, enforced by the typed `LatencyAllocation` invariant; engine-measured realized stage latencies recorded per cell.

## Market process

flow_model=clustered (post-trade intensity boost + trade-sign persistence, k=0.90; a minimal self-exciting mechanism with a bit-exact Poisson null — NOT a Hawkes process)

**Regime-contrast scope (limitation):** the two flow regimes differ in the taker process's dependence structure AND consequently in realized intensity and trade-sign runs (measured: ~7x trade-count difference at k=0.9; the momentum archetype only participates when sign runs emerge, k>=~0.85). The placebo therefore shows "allocation has no effect under THIS null market"; it does NOT isolate which aspect of the clustered process (dependence vs intensity) would drive any clustered-regime effect.

## Replication

Seed is the unit of replication (independent worlds; within-world events are never treated as independent). Cells are PAIRED by seed (common random numbers): same master seed, same named streams.

## Interference mode

frozen (bots see a shadow touch excluding strategy orders; bot flow is exogenous to the strategies)

| cell | allocation | MM PnL (ticks*qty) | MM filled lots | MM markout | Taker PnL | Taker filled lots |
|---|---|---|---|---|---|---|
| md0.000 | md+0,dec+4000,ord+4000 | 31072.6 | 38004.2 | +0.94 | -59.6 | 38.0 |
| md0.125 | md+1000,dec+3500,ord+3500 | 29560.6 | 35871.5 | +0.93 | -100.2 | 63.6 |
| md0.250 | md+2000,dec+3000,ord+3000 | 31213.6 | 37958.9 | +0.94 | -54.9 | 53.8 |
| md0.375 | md+3000,dec+2500,ord+2500 | 32541.5 | 39176.0 | +0.94 | -38.5 | 82.1 |
| md0.500 | md+4000,dec+2000,ord+2000 | 31273.8 | 37677.6 | +0.96 | 2.2 | 48.9 |
| md0.625 | md+5000,dec+1500,ord+1500 | 31192.1 | 37137.9 | +0.95 | -70.8 | 105.4 |
| md0.750 | md+6000,dec+1000,ord+1000 | 31805.2 | 37253.4 | +0.93 | -127.6 | 121.4 |
| md0.875 | md+7000,dec+500,ord+500 | 32435.1 | 38361.6 | +0.94 | -105.6 | 94.1 |
| md1.000 | md+8000,dec+0,ord+0 | 33634.4 | 38998.5 | +0.96 | -119.0 | 151.1 |

## Paired contrasts (MM PnL, b − a, per seed)

| contrast | mean diff | 95% CI (iid) | p (perm) | p (Holm) | per-lot | significant | material |
|---|---|---|---|---|---|---|---|
| md0.000 → md0.125 | -1512.0 | [-4964.7, +1940.7] | 0.4609 | 1.0000 | -0.04 | no | no |
| md0.125 → md0.250 | +1653.0 | [-740.7, +4046.7] | 0.2344 | 1.0000 | +0.04 | no | no |
| md0.250 → md0.375 | +1327.9 | [-1179.0, +3834.7] | 0.3359 | 1.0000 | +0.03 | no | no |
| md0.375 → md0.500 | -1267.8 | [-6013.4, +3477.9] | 0.6484 | 1.0000 | -0.03 | no | no |
| md0.500 → md0.625 | -81.6 | [-5842.6, +5679.4] | 0.9844 | 1.0000 | -0.00 | no | no |
| md0.625 → md0.750 | +613.1 | [-2620.4, +3846.6] | 0.7266 | 1.0000 | +0.02 | no | no |
| md0.750 → md0.875 | +629.9 | [-3178.9, +4438.7] | 0.7656 | 1.0000 | +0.02 | no | no |
| md0.875 → md1.000 | +1199.2 | [-1347.7, +3746.2] | 0.3828 | 1.0000 | +0.03 | no | no |
| md0.000 → md1.000 | +2561.8 | [-953.7, +6077.2] | 0.2109 | 1.0000 | +0.07 | no | no |

- primary contrast (md0.000 -> md1.000): mean paired MM PnL diff +2561.8 ticks*qty (per-lot +0.07), Holm p=1.0000, not significant
- economic materiality (primary): not material (threshold 1.00 ticks*qty/fill)
- MM filled lots across the slice: 38998.5 (all-observation) vs 38004.2 (all-action): placement changes fill mechanics
- taker (momentum) PnL across the slice: -59.6 -> -119.0 (moves)
- taker primary-contrast paired PnL diff: -59.4 (se 52.8, perm p=0.2969) — not significant
- H0 at the primary contrast: NOT REJECTED (placement effect small and not significant at this budget/regime — a valid negative result)

## Falsification status

- Poisson null: run the same grid with `flow_model=poisson`. Interpretation scope: if an allocation effect existed in the null market it would indicate placement sensitivity WITHOUT clustered flow (mechanism contradiction); a null result here is necessary but NOT sufficient for the clustered-flow mechanism (the regimes also differ in intensity — see the regime-contrast scope above).
- Interference: re-run with `bots_observe_shadow=true`; a large gap between reactive and frozen effects means the interpretation is unstable.
- Kill conditions: no adjacent-pair materiality under clustered flow at the primary contrast; or materiality driven by one seed; or reference/optimized book disagreement (checked separately in the differential suite).

## Reproducibility

`arbiter flagship --config <file>` re-runs this design; `arbiter flagship --reproduce <artifact.json>` verifies every recorded hash bit-for-bit. definition_hash covers the full design (grid, seeds, regime, horizon, thresholds).
