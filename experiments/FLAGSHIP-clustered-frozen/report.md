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
| md0.125 | md+1000,dec+3500,ord+3500 | 30071.2 | 36474.6 | +0.93 | -98.5 | 69.4 |
| md0.250 | md+2000,dec+3000,ord+3000 | 31424.4 | 38283.9 | +0.94 | -83.4 | 71.1 |
| md0.375 | md+3000,dec+2500,ord+2500 | 31668.8 | 38251.1 | +0.94 | -78.0 | 56.6 |
| md0.500 | md+4000,dec+2000,ord+2000 | 30456.6 | 36422.8 | +0.97 | -58.1 | 42.8 |
| md0.625 | md+5000,dec+1500,ord+1500 | 33369.8 | 39537.9 | +0.95 | -71.5 | 70.9 |
| md0.750 | md+6000,dec+1000,ord+1000 | 33942.0 | 40101.6 | +0.96 | -86.4 | 76.0 |
| md0.875 | md+7000,dec+500,ord+500 | 36488.1 | 43071.2 | +0.94 | -120.5 | 129.2 |
| md1.000 | md+8000,dec+0,ord+0 | 36148.2 | 41663.8 | +0.98 | -164.9 | 147.0 |

## Paired contrasts (MM PnL, b − a, per seed)

| contrast | mean diff | 95% CI (iid) | p (perm) | p (Holm) | per-lot | significant | material |
|---|---|---|---|---|---|---|---|
| md0.000 → md0.125 | -1001.4 | [-4182.4, +2179.7] | 0.5234 | 1.0000 | -0.03 | no | no |
| md0.125 → md0.250 | +1353.1 | [-2581.1, +5287.3] | 0.5859 | 1.0000 | +0.04 | no | no |
| md0.250 → md0.375 | +244.4 | [-3506.4, +3995.1] | 0.8984 | 1.0000 | +0.01 | no | no |
| md0.375 → md0.500 | -1212.1 | [-6385.5, +3961.3] | 0.6562 | 1.0000 | -0.03 | no | no |
| md0.500 → md0.625 | +2913.1 | [-1161.4, +6987.6] | 0.2031 | 1.0000 | +0.08 | no | no |
| md0.625 → md0.750 | +572.2 | [-3055.6, +4200.1] | 0.8125 | 1.0000 | +0.01 | no | no |
| md0.750 → md0.875 | +2546.1 | [+31.2, +5061.0] | 0.0859 | 0.6875 | +0.06 | no | no |
| md0.875 → md1.000 | -339.9 | [-4760.1, +4080.4] | 0.9141 | 1.0000 | -0.01 | no | no |
| md0.000 → md1.000 | +5075.6 | [+1572.9, +8578.4] | 0.0312 | 0.2812 | +0.13 | no | no |

- primary contrast (md0.000 -> md1.000): mean paired MM PnL diff +5075.6 ticks*qty (per-lot +0.13), Holm p=0.2812, not significant
- economic materiality (primary): not material (threshold 1.00 ticks*qty/fill)
- MM filled lots across the slice: 41663.8 (all-observation) vs 38004.2 (all-action): placement changes fill mechanics
- taker (momentum) PnL across the slice: -59.6 -> -164.9 (moves)
- taker primary-contrast paired PnL diff: -105.2 (se 75.6, perm p=0.2188) — not significant
- H0 at the primary contrast: NOT REJECTED (placement effect small and not significant at this budget/regime — a valid negative result)

## Falsification status

- Poisson null: run the same grid with `flow_model=poisson`. Interpretation scope: if an allocation effect existed in the null market it would indicate placement sensitivity WITHOUT clustered flow (mechanism contradiction); a null result here is necessary but NOT sufficient for the clustered-flow mechanism (the regimes also differ in intensity — see the regime-contrast scope above).
- Interference: re-run with `bots_observe_shadow=true`; a large gap between reactive and frozen effects means the interpretation is unstable.
- Kill conditions: no adjacent-pair materiality under clustered flow at the primary contrast; or materiality driven by one seed; or reference/optimized book disagreement (checked separately in the differential suite).

## Reproducibility

`arbiter flagship --config <file>` re-runs this design; `arbiter flagship --reproduce <artifact.json>` verifies every recorded hash bit-for-bit. definition_hash covers the full design (grid, seeds, regime, horizon, thresholds).