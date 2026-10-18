# Architecture

ARBITER-X is a single-process deterministic discrete-event system. The engine
keeps simulation state on a logical clock; wall-clock time is restricted to
benchmark instrumentation. This separation makes repeated runs and paired
counterfactuals directly comparable.

## System layers

```text
CLI / benchmarks
        |
experiments / research pipeline
        |
world, replay, snapshots, strategies, risk, simulation
        |
wire protocol, journal, order books
        |
deterministic types, configuration, RNG, hashing, CRC
```

The `arbiter_core` static library contains every engine and research module.
The `arbiter` CLI and `arbiter_bench` executable are thin consumers of that
library. Tests link the same library used by the executables.

| Module | Responsibility |
|---|---|
| `core` | integer domain types, named RNG streams, hashing, CRC, configuration |
| `orderbook` | reference and optimized price-time-priority books |
| `protocol` | length/version/CRC-framed event and execution-report messages |
| `journal` | append-only events, checkpoints, integrity validation, recovery |
| `sim` | deterministic provider/taker flow and regime transitions |
| `strategy` | market maker, momentum taker, local view, queue inference |
| `risk` | per-account bounds, position, notional, rate, and duplicate checks |
| `exec` | scheduler, gateway, matching, feed handling, replay, snapshots |
| `counterfactual` | experiment grids, latency allocation, paired inference |
| `data`, `research` | normalized L2 input, features, labels, models, execution economics |
| `verification` | differential generation, diagnosis, and minimization |

## Event machine

Events are ordered by `(logical_time, rank, insertion_counter)`. Rank resolves
same-timestamp causal order: simulated flow, market-data delivery, strategy
timers, gateway/matching, execution reports, expiry, then recovery.

A strategy order follows one path:

```text
decision
  -> decision latency
  -> encoded order frame
  -> order-channel transit
  -> decode and risk checks
  -> exchange processing
  -> matching
  -> encoded execution report
  -> execution-channel transit
  -> strategy fill handling
```

Every exchange mutation publishes incremental market data. The strategy sees
only its local feed-handler view, which performs sequence validation, gap
detection, retransmission, and snapshot recovery.

## State, snapshots, and forks

`World::state_hash()` covers every value that can affect future execution,
including logical time, scheduled events, canonical book state and ownership,
strategies, per-account risk, simulator/RNG state, network state, feed buffers,
in-flight frames, and pending markouts. Measurement-only buffers are excluded
and cannot affect control flow.

A full-world snapshot serializes the same causal state and records its hash.
Restore rebuilds the world and rejects a hash or configuration mismatch. A fork
restores a common snapshot under one explicitly changed configuration value and
records branch genealogy, allowing the first causal divergence to be located.

## Deliberate boundaries

- No sockets, venue adapters, kernel bypass, or real order routing.
- The deterministic engine is single-threaded; the SPSC ring is a standalone
  concurrency benchmark, not part of execution.
- Bot flow is co-located with the simulated exchange and does not pass through
  the strategy-facing risk/network path.
- Cross-compiler and cross-architecture bitwise identity is not claimed.

See [the determinism contract](DETERMINISM.md), [execution model](EXECUTION_MODEL.md),
and [limitations](LIMITATIONS.md) for the exact guarantees.