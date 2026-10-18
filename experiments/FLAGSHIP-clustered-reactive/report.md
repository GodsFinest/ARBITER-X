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
| md0.125 | md+1000,dec+3500,ord+3500 | -286822.9 | 32336.2 | -0.99 | 140391.9 | 1976.4 |
| md0.250 | md+2000,dec+3000,ord+3000 | -327214.0 | 34464.6 | -2.05 | 133643.5 | 2402.1 |
| md0.375 | md+3000,dec+2500,ord+2500 | -460600.5 | 36756.1 | -1.49 | 110709.4 | 2240.2 |
| md0.500 | md+4000,dec+2000,ord+2000 | -383499.6 | 33375.6 | -1.62 | 134983.1 | 2335.5 |
| md0.625 | md+5000,dec+1500,ord+1500 | -409183.1 | 36706.6 | -1.14 | 15409.8 | 2081.4 |
| md0.750 | md+6000,dec+1000,ord+1000 | -435194.1 | 31363.2 | -1.62 | 38054.0 | 2157.8 |
| md0.875 | md+7000,dec+500,ord+500 | -345676.4 | 35723.2 | -2.14 | 13251.4 | 2268.2 |
| md1.000 | md+8000,dec+0,ord+0 | -377930.8 | 34498.2 | -1.70 | 37661.8 | 2152.1 |

## Paired contrasts (MM PnL, b − a, per seed)

| contrast | mean diff | 95% CI (iid) | p (perm) | p (Holm) | per-lot | significant | material |
|---|---|---|---|---|---|---|---|
| md0.000 → md0.125 | -17967.9 | [-107040.4, +71104.6] | 0.7422 | 1.0000 | -0.54 | no | no |
| md0.125 → md0.250 | -40391.1 | [-119503.2, +38721.0] | 0.3516 | 1.0000 | -1.21 | no | yes |
| md0.250 → md0.375 | -133386.5 | [-250825.0, -15948.0] | 0.0703 | 0.6328 | -3.75 | no | yes |
| md0.375 → md0.500 | +77100.9 | [-83448.7, +237650.5] | 0.3750 | 1.0000 | +2.20 | no | yes |
| md0.500 → md0.625 | -25683.5 | [-123595.4, +72228.4] | 0.6172 | 1.0000 | -0.73 | no | no |
| md0.625 → md0.750 | -26011.0 | [-200932.4, +148910.4] | 0.8203 | 1.0000 | -0.76 | no | no |
| md0.750 → md0.875 | +89517.8 | [-14823.2, +193858.7] | 0.1172 | 0.9375 | +2.67 | no | yes |
| md0.875 → md1.000 | -32254.4 | [-128270.1, +63761.4] | 0.7422 | 1.0000 | -0.92 | no | no |
| md0.000 → md1.000 | -109075.8 | [-263318.7, +45167.2] | 0.2422 | 1.0000 | -3.17 | no | yes |

- primary contrast (md0.000 -> md1.000): mean paired MM PnL diff -109075.8 ticks*qty (per-lot -3.17), Holm p=1.0000, not significant
- economic materiality (primary): MATERIAL (threshold 1.00 ticks*qty/fill)
- MM filled lots across the slice: 34498.2 (all-observation) vs 34388.0 (all-action): placement changes fill mechanics
- taker (momentum) PnL across the slice: 74220.8 -> 37661.8 (moves)
- taker primary-contrast paired PnL diff: -36559.0 (se 80432.7, perm p=0.6875) — not significant
- H0 at the primary contrast: NOT REJECTED — point estimate is economically large but cross-seed variance swamps it (low power; report as uncertain, do NOT interpret the sign as an effect)

## Falsification status

- Poisson null: run the same grid with `flow_model=poisson`. Interpretation scope: if an allocation effect existed in the null market it would indicate placement sensitivity WITHOUT clustered flow (mechanism contradiction); a null result here is necessary but NOT sufficient for the clustered-flow mechanism (the regimes also differ in intensity — see the regime-contrast scope above).
- Interference: re-run with `bots_observe_shadow=true`; a large gap between reactive and frozen effects means the interpretation is unstable.
- Kill conditions: no adjacent-pair materiality under clustered flow at the primary contrast; or materiality driven by one seed; or reference/optimized book disagreement (checked separately in the differential suite).

## Reproducibility

`arbiter flagship --config <file>` re-runs this design; `arbiter flagship --reproduce <artifact.json>` verifies every recorded hash bit-for-bit. definition_hash covers the full design (grid, seeds, regime, horizon, thresholds).