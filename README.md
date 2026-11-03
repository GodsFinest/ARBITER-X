# ARBITER-X

A zero-third-party-dependency C++23 deterministic trading execution laboratory.
It combines price-time matching, a faulted message pipeline, account risk,
recoverable event journals, complete world snapshots, deterministic forks, and
controlled latency experiments in one single-threaded discrete-event world.
This is a simulator for engineering and research, not a live trading system.

## Build and first run

Requires CMake 3.24+ and a C++23 compiler (tested CI configurations: GCC 13,
Clang 18, Apple Clang). LAB and Python packages are not required by the engine.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DARBITER_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/arbiter verify --seed 42 --ms 50
./build/arbiter simulate --seed 42 --ms 50 --out artifacts/example
./build/arbiter journal verify artifacts/example.journal
./build/arbiter replay --journal artifacts/example.journal --book ref
./build/arbiter replay --journal artifacts/example.journal --book opt
```

`verify` checks repeated-world determinism. Differential tests and the two replay
commands separately verify agreement between reference and optimized books.

## Execution path

Strategies observe delayed market data, schedule decisions, encode order frames,
traverse the faulted order channel, and enter gateway decode/deduplication/risk
checks before matching. Exchange state and risk positions update at match time.
Execution notifications traverse a separate channel before strategy-local state
updates. Local state can therefore lag exchange truth or miss notifications.

Bots submit colocated exchange flow. Scheduler ordering is logical time, event
rank, then insertion counter. Wall-clock measurements never steer execution.
Each network decision consumes seven draws regardless of its fault profile;
corruption uses separate randomness. Market-data recovery uses retransmission
and snapshot fallback. Order/execution loss has different recovery semantics;
it must not be described as a reliable transport.

Both matching engines implement the same price-time and TIF semantics. The
optimized ladder/pool/ID map is checked against the independent map/list oracle.
Risk reserves working-order exposure and releases it through exchange lifecycle
transitions, including lost-notification paths.

## Recovery and branching

Book checkpoints accelerate journal replay and support checkpoint-plus-tail
verification. Full-world snapshots additionally retain scheduler/RNG/strategy/
risk/ownership/in-flight state so execution can continue or fork.

```sh
./build/arbiter worldcheck --seed 4242 --events 20000 --out artifacts/world.wsnap
./build/arbiter worldjump --snap artifacts/world.wsnap --events 20000
./build/arbiter worldfork --snap artifacts/world.wsnap --variable half_spread=4 --events 20000
```

See docs/STORAGE.md, docs/REPRODUCIBILITY.md, and docs/COUNTERFACTUALS.md.
Full-world format 15 rejects format 14 explicitly; read old snapshots with
archive/monolith-65b9fa7 and regenerate them. Journal and book checkpoints
retain their existing formats.
A first divergent hash checkpoint bounds the divergence location; it does not
identify the exact causal event. State hashes are noncryptographic fingerprints.

## Fixed-total latency allocation

```sh
./build/arbiter flagship --config configs/flagship_clustered.axexp --out artifacts/allocation
./build/arbiter flagship --reproduce artifacts/allocation/results.json
./build/arbiter flagship --reproduce experiments/FLAGSHIP-clustered-reactive/results.json
```

The retained study varies placement of an 8us budget across observation,
decision and order transit, with nine allocations and eight paired seeds.
Poisson and shadow-observation controls remain under experiments/FLAGSHIP-*.
The clustered/reactive primary contrast is about -5.8 ticks/lot with Holm-adjusted
p=.35: material under its threshold, statistically inconclusive. Eight seeds use
256 exact sign assignments. The 5,000-repetition parameter applies to the larger
Monte Carlo path. Reported intervals are IID seed-level intervals; HAC utilities
are available but their intervals are not serialized by this study.

See docs/EXPERIMENTS/flagship_allocation.md and examples/flagship.sh.
Shadow mode changes observations; real matching interference remains.

## Verification and measurements

The suite checks matching, differential streams, minimization/corpus replay,
protocol mutation, risk reservations, transport faults, journal recovery,
continuous/restored worlds, forks and experimental stage placement.
Run scripts/reproduce_all.sh for the complete core workflow.

```sh
./build/arbiter_bench exchange
./build/arbiter_bench protocol --quick
./build/arbiter simstats --seed 42 --ms 50
```

See docs/PERFORMANCE.md for workload, hardware, sampled latency, throughput and
allocation evidence. No hardware-independent speed advantage is promised.
Preflight secures node/index resources for normal rejection paths; blanket
allocation-exception atomicity is not established because fill vectors can grow.
Measurement buffers are excluded from complete snapshot-history continuity.
Use a fresh world for restoration and discard it if restoration fails.

## Optional historical research

[ARBITER-X-LAB](https://github.com/GodsFinest/ARBITER-X-LAB) provides CSV ingestion,
features/labels/models, approximate historical execution and general World grids.
It consumes core 1.1.x/API 1 through the installed ArbiterX::core package or an
explicit ARBITER_X_SOURCE_DIR checkout. Core has no dependency on LAB.

```sh
cmake --install build --prefix /absolute/install/prefix
# In the LAB checkout:
cmake -S . -B build -DCMAKE_PREFIX_PATH=/absolute/install/prefix
```

Former `research` and `experiment` commands are `arbiter-lab research` and
`arbiter-lab experiment`. Use `replay --until` instead of `timetravel`, the
benchmark executable instead of `arbiter bench`, and examples instead of `demo`.

Original monolith: 65b9fa7803c5b888cdea777295683a7c06ef14f6, preserved in Git and
locally tagged archive/monolith-65b9fa7. See [validation](docs/VALIDATION.md)
for verification coverage and [architecture](docs/ARCHITECTURE.md) for the design.
