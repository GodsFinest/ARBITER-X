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

reactive (bots observe the live touch and react to strategy quotes; the measured effect includes market feedback — interference sensitivity is probed by re-running frozen)

| cell | allocation | MM PnL (ticks*qty) | MM filled lots | MM markout | Taker PnL | Taker filled lots |
|---|---|---|---|---|---|---|
| md0.000 | md+0,dec+4000,ord+4000 | -268855.0 | 34388.0 | -0.90 | 74220.8 | 1942.0 |
| md0.125 | md+1000,dec+3500,ord+3500 | -311466.6 | 35969.2 | -0.93 | 78807.1 | 2215.8 |
| md0.250 | md+2000,dec+3000,ord+3000 | -353521.4 | 34055.9 | -1.93 | 91658.0 | 2435.5 |
| md0.375 | md+3000,dec+2500,ord+2500 | -374278.1 | 41108.5 | -1.77 | -18916.1 | 2600.9 |
| md0.500 | md+4000,dec+2000,ord+2000 | -462092.4 | 34945.6 | -1.89 | 59361.5 | 2303.4 |
| md0.625 | md+5000,dec+1500,ord+1500 | -320810.0 | 25624.5 | -0.85 | 184335.4 | 1562.8 |
| md0.750 | md+6000,dec+1000,ord+1000 | -466860.1 | 35777.9 | -1.38 | 178810.8 | 2156.1 |
| md0.875 | md+7000,dec+500,ord+500 | -358750.9 | 31897.9 | -2.36 | 135344.5 | 2330.4 |
| md1.000 | md+8000,dec+0,ord+0 | -457905.5 | 31125.1 | -2.13 | 79433.5 | 2157.8 |

## Paired contrasts (MM PnL, b − a, per seed)

| contrast | mean diff | 95% CI (iid) | p (perm) | p (Holm) | per-lot | significant | material |
|---|---|---|---|---|---|---|---|
| md0.000 → md0.125 | -42611.6 | [-137608.3, +52385.1] | 0.4844 | 1.0000 | -1.21 | no | yes |
| md0.125 → md0.250 | -42054.8 | [-94801.0, +10691.5] | 0.1719 | 1.0000 | -1.20 | no | yes |
| md0.250 → md0.375 | -20756.8 | [-169704.2, +128190.7] | 0.8203 | 1.0000 | -0.55 | no | no |
| md0.375 → md0.500 | -87814.2 | [-246886.3, +71257.8] | 0.3125 | 1.0000 | -2.31 | no | yes |
| md0.500 → md0.625 | +141282.4 | [-34071.8, +316636.6] | 0.1797 | 1.0000 | +4.67 | no | yes |
| md0.625 → md0.750 | -146050.1 | [-344627.8, +52527.6] | 0.1484 | 1.0000 | -4.76 | no | yes |
| md0.750 → md0.875 | +108109.2 | [-86482.7, +302701.2] | 0.3516 | 1.0000 | +3.19 | no | yes |
| md0.875 → md1.000 | -99154.6 | [-337866.2, +139556.9] | 0.5234 | 1.0000 | -3.15 | no | yes |
| md0.000 → md1.000 | -189050.5 | [-355363.2, -22737.8] | 0.0391 | 0.3516 | -5.77 | no | yes |

- primary contrast (md0.000 -> md1.000): mean paired MM PnL diff -189050.5 ticks*qty (per-lot -5.77), Holm p=0.3516, not significant
- economic materiality (primary): MATERIAL (threshold 1.00 ticks*qty/fill)
- MM filled lots across the slice: 31125.1 (all-observation) vs 34388.0 (all-action): placement changes fill mechanics
- taker (momentum) PnL across the slice: 74220.8 -> 79433.5 (moves)
- taker primary-contrast paired PnL diff: +5212.8 (se 102694.5, perm p=0.9375) — not significant
- H0 at the primary contrast: NOT REJECTED — point estimate is economically large but cross-seed variance swamps it (low power; report as uncertain, do NOT interpret the sign as an effect)

## Falsification status

- Poisson null: run the same grid with `flow_model=poisson`. Interpretation scope: if an allocation effect existed in the null market it would indicate placement sensitivity WITHOUT clustered flow (mechanism contradiction); a null result here is necessary but NOT sufficient for the clustered-flow mechanism (the regimes also differ in intensity — see the regime-contrast scope above).
- Interference: re-run with `bots_observe_shadow=true`; a large gap between reactive and frozen effects means the interpretation is unstable.
- Kill conditions: no adjacent-pair materiality under clustered flow at the primary contrast; or materiality driven by one seed; or reference/optimized book disagreement (checked separately in the differential suite).

## Reproducibility

`arbiter flagship --config <file>` re-runs this design; `arbiter flagship --reproduce <artifact.json>` verifies every recorded hash bit-for-bit. definition_hash covers the full design (grid, seeds, regime, horizon, thresholds).
