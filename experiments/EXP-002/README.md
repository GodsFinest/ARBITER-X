# EXP-002: How much does latency destroy short-horizon execution alpha?

Run: `arbiter research run EXP-002 [--csv REAL.csv]`
Reproduce: `arbiter research reproduce EXP-002`

Two legs, same seed, controlled random streams:

1. Pipeline leg: FIXED predictions and market path, decision→execution delay
   swept 0µs…500µs; fills/queue value/P&L/markouts re-measured per point.
2. World leg: counterfactual latency pair (800ns vs 50000ns) with first
   causal divergence localized to the event, then traced to queue, fills,
   inventory and P&L.

Latency-value curve + causal attribution live in `report.md`; per-point net
P&L in `results.json` (`latency_sweep_net_*`).