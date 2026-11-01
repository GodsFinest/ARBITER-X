# ARBITER-X

[![CI](https://github.com/GodsFinest/ARBITER-X/actions/workflows/ci.yml/badge.svg)](https://github.com/GodsFinest/ARBITER-X/actions/workflows/ci.yml)

ARBITER-X is a deterministic C++23 laboratory for market-microstructure and
execution research. It combines a price-time-priority matching engine, a
fault-injecting network model, journal/replay, full-world snapshots, queue-aware
strategies, and paired counterfactual experiments in one reproducible system.

This is simulation software. It has no venue connectivity and must not be used
for real-money trading.

## Results at a glance

- The Release exchange benchmark median on the Apple M1 is [10,222,719 ops/s
  for the optimized book versus 9,189,719 for the reference](docs/PERFORMANCE.md);
  the sampled optimized latency is 42 ns p50 and 792 ns p99.
- The separate [200,000-operation allocation probe](docs/PERFORMANCE.md)
  records 1 steady-state allocation for the optimized book versus 188,905 for
  the `std::map`/list reference.
- The [flagship result](docs/EXPERIMENTS/flagship_allocation.md) is uncertain:
  the point estimate is economically large, but cross-seed variance swamps it
  at the committed replication budget.
- [Verification](docs/VERIFICATION.md) covers 168 AX_TEST cases in 9 test
  binaries and 17 CTest entries.

## What it demonstrates

- A simple reference order book and an optimized ladder/slab implementation
  are checked after every operation by a differential test harness.
- The complete strategy path uses CRC-protected wire frames, pre-trade risk,
  matching, market-data delivery, and execution reports.
- Runs are exactly reproducible from configuration and seed on the same
  architecture, OS, compiler, and build flags. Research artifacts use exact
  counters plus a documented floating-point tolerance when compiler or
  platform provenance differs.
- Full-world snapshots preserve scheduler, exchange, strategy, risk, simulator,
  network, and in-flight-message state, enabling deterministic forks.
- Research reports separate prediction from execution economics and preserve
  negative or inconclusive results.

## Quick start

Requirements: CMake 3.24+, a C++23 compiler (Clang 17+ or GCC 13+), and no
third-party runtime dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
ctest --test-dir build --output-on-failure
./build/arbiter --help
```

Useful entry points:

```sh
./build/arbiter verify --seed 42 --ms 50
./build/arbiter simulate --seed 42 --ms 100 --out artifacts/sim42
./build/arbiter replay --journal artifacts/sim42.journal
./build/arbiter flagship --seed 2026 --seeds 8 --total-ns 8000 --flow clustered
./build/arbiter research run EXP-001
./build/arbiter_bench all
```

Run the complete build, verification, research-reproduction, and benchmark
workflow with:

```sh
./scripts/reproduce_all.sh
```

Generated journals, snapshots, benchmark logs, and ad-hoc experiment output
belong under `artifacts/`, which is intentionally ignored. Versioned research
records under `experiments/` are inputs to reproducibility checks and remain
tracked.

## Research question

The flagship experiment asks whether allocating a fixed total latency budget
across observation, decision, and order-transit stages changes execution
economics. The total delay is held constant, cells share macro-seeds, the engine
records realized stage timings, and the analysis includes a memoryless-flow
placebo and an interference control.

The instrument works, while the current economic conclusion is uncertain at
the available replication count.
See [the experiment record](docs/EXPERIMENTS/flagship_allocation.md) and
[limitations](docs/LIMITATIONS.md).

## Known limitations

- The simulator is stylized, not venue-calibrated; its one-tick spread and
  flow process are model choices ([simulation](docs/SIMULATION.md),
  [limitations](docs/LIMITATIONS.md)).
- Co-located bot flow bypasses the strategy-facing network and risk path
  ([architecture](docs/ARCHITECTURE.md)).
- Only the market-data channel has gap recovery; dropped or corrupted order
  and execution-report frames are not recovered in v1
  ([execution model](docs/EXECUTION_MODEL.md)).
- Bitwise equality is guaranteed only on the same architecture, OS, compiler,
  and build flags ([determinism contract](docs/DETERMINISM.md)).
- The flagship is a paired simulator counterfactual, not causal evidence about
  real markets, and its current economic result is uncertain
  ([experiment record](docs/EXPERIMENTS/flagship_allocation.md)).

## Repository map

| Path | Responsibility |
|---|---|
| `include/arbiter`, `src/core` | deterministic types, RNG streams, hashing, CRC, configuration |
| `src/orderbook` | reference and optimized matching engines |
| `src/exec`, `src/protocol`, `src/journal` | event machine, wire format, replay, checkpoints |
| `src/sim`, `src/strategy`, `src/risk` | simulated flow, strategies, queue model, gateway risk |
| `src/counterfactual`, `src/research`, `src/data` | experiments, statistics, data pipeline |
| `tests` | unit, differential, recovery, snapshot, and research tests |
| `benchmarks` | reproducible wall-clock and allocation probes |
| `configs`, `experiments` | versioned experiment definitions and result records |
| `tools/research` | independent Python analysis and feature checks |

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Execution model](docs/EXECUTION_MODEL.md)
- [Determinism contract](docs/DETERMINISM.md)
- [Verification strategy](docs/VERIFICATION.md)
- [Counterfactual methodology](docs/COUNTERFACTUALS.md)
- [Research pipeline](docs/RESEARCH.md)
- [Reproducibility](docs/REPRODUCIBILITY.md)
- [Performance methodology](docs/PERFORMANCE.md)
- [Limitations](docs/LIMITATIONS.md)

## License

MIT. See [LICENSE](LICENSE).
