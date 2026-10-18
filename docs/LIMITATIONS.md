# Limitations

Credibility requires stating what is NOT proven. This document is normative:
claims contradicting it are bugs.

## Simulator

- Single instrument, integer ticks; stylized bots (Poisson arrivals, Markov
  regimes, OPTIONAL one-shot taker self-excitation k; see
  docs/SIMULATION.md). Not calibrated to any venue. Measured: at k=0 the
  market alternates tick-for-tick (no sign runs); clustering is a MODEL
  CHOICE controlled by k, not an empirical feature.
- Queue inference reports a point estimate AND its structural ambiguity
  bound (`mm_ambiguous_fill_qty`); real fills authoritative. Inference is
  never used for accounting.
- Bots bypass network/risk (documented asymmetry). In reactive mode they
  observe the live touch INCLUDING strategy quotes; frozen mode isolates
  the observation channel but NOT matching interference.

## Flagship research program

- The fixed-total allocation experiment is a paired counterfactual under
  the simulator's structural assumptions, not identification about real
  markets, and not "causal proof" (shared randomness is variance control,
  not causal identification).
- The clustered regime's measured effects are heavy-tailed across seeds;
  the flagship result at L=8µs is UNCERTAIN (point estimates material,
  Holm-corrected significance not reached at 8 seeds, see
  docs/EXPERIMENTS/flagship_allocation.md).
- Frozen-vs-reactive differences show the reactive estimate includes
  observation-channel feedback; the split does NOT estimate real-market
  interference in general.
- Latency budgets at the flagship default (8µs) are sub-quote-lifetime in
  this market (25µs requote cadence). Observed insensitivity at smaller
  delays bounds what the experiment can express without larger budgets.

## Execution / queue

- No L3 queue truth from L2 data: fill reconstruction is a 3-model spread,
  not a measurement. Do not claim "queue-aware" precision beyond the
  documented assumptions.
- Market impact: size experiments vary assumed consumption; there is no
  production impact model. Labeled as such.
- Fees are flat per-lot; exchange fee schedules/rebates are NOT modeled.
- Hidden liquidity, iceberg orders, odd-lots: absent.

## Data

- No proprietary data bundled; public captures need license review.
- L2 level-set reconstruction approximates; sequence gaps/session boundaries
  must be preserved, never interpolated.
- Synthetic streams are stylized controls with KNOWN injected alpha, they
  validate the pipeline, not the market.

## Statistics

- HAC/Newey-West intervals and non-overlapping subsamples mitigate, but do
  not eliminate, dependence bias. Sharpe-like ratios are descriptive.
- Walk-forward covers 4 folds; regime-shift coverage is partial.
- Seed-level replication is the unit of inference in the flagship;
  within-world event counts are never treated as independent samples.

## Determinism / portability

- Same-architecture/build only. FNV-1a 64 is non-cryptographic (collisions
  are possible; differential tests compare full views). `__int128` has a
  tested portable fallback, and CI covers current GCC and Clang toolchains.
- `created_utc`/wall-clock/benches are informational, never hashed.

## Scope (never claimed)

Production HFT, exchange-grade matching, institutional execution, profitable
alpha, realistic fills/impact, universal determinism. This is a research
laboratory for discovering WHEN ideas fail, under WHAT assumptions.