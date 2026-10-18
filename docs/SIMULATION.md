# Simulation

## What the market generator IS

The `MarketSim` bot population is the exogenous flow process:

* **Provider bots** (`provider_arrival_per_ms`, 3 accounts): Poisson
  arrivals; join inside the spread at the observed touch (sometimes
  deeper); random cancels (`cancel_fraction`). Both sides stay alive.
* **Taker bots** (`taker_arrival_per_ms`): Poisson arrivals of aggressive
  IOC orders with a momentum bias from recent trade directions
  (`0.5 + 0.08 * bias`, bias clamped ±4).
* **Regime chain** (`regime_dwell_ns`): a two-state calm/volatile Markov
  chain scaling arrival rates (×3) and sizes (×2). Dwell 4ms default.
* **Taker self-excitation** (`taker_self_excitation`, k in [0,1), default
  0 = OFF): THE CLUSTERING KNOB of the flagship research program.
  Two coupled mechanisms, both armed by the same k:
  1. *intensity memory*: each executed trade shortens the NEXT taker
     inter-arrival by factor 1/(1-k) (one-shot boost; no extra RNG draw);
  2. *sign persistence*: a taker follows the previous taker's side with
     probability k.
  k = 0 recovers the historical memoryless process BIT-EXACTLY (verified:
  `flow_poisson_null_is_bit_identical_to_historical_default`). We do NOT
  call this a Hawkes process: it is a minimal, seedable, snapshot-safe
  self-excitation with a clean Poisson null, chosen deliberately over a
  full Hawkes calibration (which the research question does not need).

## Measured regime properties (seed 99, 300ms, simstats)

| quantity | k=0 (Poisson null) | k=0.9 (clustered) |
|---|---|---|
| trades | 6,834 (22.8k/s) | 40,661 (135.5k/s) |
| trade-sign persistence (repeat rate) | 0.000 (perfect alternation) | 0.297 (runs emerge) |
| trade inter-arrival burstiness (cv) | 1.72 (regime chain alone) | 3.17 |
| mid lag-1 autocorr @ trade times | ~0.0 | ~1.0 |

Calibration facts (simulator properties, NOT claims of
realism):

* At k=0 the market alternates tick-for-tick: provider replenishment
  (60/ms) dominates taker pressure (8/ms), so sign runs never emerge and
  the momentum archetype never trades. This was the historical behavior.
* Sign runs long enough to trigger the momentum strategy
  (|sum of 8 signs| >= 3) only emerge for k >= ~0.85 (measured).
* k=0 still shows inter-arrival cv > 1, the calm/volatile regime chain is
  itself a clustering mechanism (rate persistence over 4ms dwell times).
  The Poisson-null claim is about the TAKER PROCESS given the regime, not
  about the absence of any clustering anywhere.

## Bots and the strategies

Bots are co-located with the exchange: their orders skip the network and
the risk gateway (documented asymmetry). They interact with the strategies
through the real book, and, in the default reactive mode, they OBSERVE the
live touch (including strategy quotes). The frozen mode
(`bots_observe_shadow`) removes the observation channel's strategy
dependence (docs/COUNTERFACTUALS.md).

## Not claimed

No fidelity to any specific venue. Spread is 1 tick at p50 by construction
(half_spread=1 market maker inside a 2-tick band); depth dynamics are
stylized; sizes are homogeneous per bot class. `simstats` reports these as
model choices.