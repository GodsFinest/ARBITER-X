# Performance methodology

Performance results are produced by `arbiter_bench`; generated logs are not
committed because wall-clock measurements depend on machine, toolchain, build
type, and system load. The citable exchange snapshot below records three runs
with their provenance; new claims should include equivalent raw output and
environment details.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
./build/arbiter_bench all | tee artifacts/benchmarks.log
```

The benchmark prints compiler/build information, host architecture, iteration
counts, sampled latency percentiles, aggregate throughput, and allocation
counts. Run it on an otherwise idle machine and retain the raw output whenever
citing a number.

## Suites

- `exchange`: identical mixed add/cancel/taker workloads against the reference
  and optimized books, followed by a steady-state allocation probe.
- `protocol`: CRC-framed wire encode/decode throughput and latency.
- `journal`: append and verified-read costs over a generated event stream.
- `pipeline`: complete world throughput, instrumentation overhead, research
  feature/study cost, and SPSC round-trip latency.

The optimized book uses a contiguous price ladder, slab-allocated order nodes,
and an open-addressing ID map. The benchmark intercepts allocation calls to
verify its steady-state hot path independently of elapsed time.

## Citable exchange snapshot

Three sequential runs of `./build/arbiter_bench exchange` were collected on
2026-11-01 from commit `de3c716` (`fix: keep market takers on the ladder
scan`). The machine was a MacBookAir10,1 with an Apple M1, 8 GB RAM, arm64,
using Apple clang 17.0.0 (`clang-1700.6.3.2`), C++23
(`__cplusplus=202302L`), and a Release build. The host was otherwise idle;
the benchmark itself reports wall-clock measurements only.

Latency columns are sampled per-operation values from the reference and
optimized passes. Allocation counts are from the separate 200,000-operation
steady-state probe, not the 500,000-operation throughput workload.

| run | reference aggregate (ops/s) | reference p50 / p99 / p99.9 (ns) | optimized aggregate (ops/s) | optimized p50 / p99 / p99.9 (ns) | steady-state allocations, 200,000-op probe (opt / ref) |
|---:|---:|---:|---:|---:|---:|
| 1 | 9,189,719 | 125 / 458 / 583 | 9,944,106 | 42 / 792 / 1,563 | 1 / 188,905 |
| 2 | 9,062,301 | 125 / 458 / 917 | 10,395,614 | 42 / 750 / 1,500 | 1 / 188,905 |
| 3 | 9,229,264 | 125 / 417 / 584 | 10,222,719 | 42 / 792 / 1,542 | 1 / 188,905 |
| median | 9,189,719 | 125 / 458 / 584 | 10,222,719 | 42 / 792 / 1,542 | 1 / 188,905 |

The aggregate spread was 9,062,301–9,229,264 ops/s for the reference book
and 9,944,106–10,395,614 ops/s for the optimized book. The allocation result
is one steady-state allocation for the optimized book versus 188,905 for the
`std::map`/list reference over the 200,000-operation probe.

## Interpretation rules

1. Compare aggregate throughput for sub-clock-resolution operations; sampled
   per-operation medians can be dominated by timer-call overhead.
2. Treat absolute wall-clock values as host-specific. Architecture and
   compiler changes invalidate direct comparisons.
3. Benchmark instrumentation is not simulation time. It never feeds into
   engine behavior or reproducibility hashes.
4. A performance change is accepted only after correctness, differential, and
   recovery tests remain green.
5. Profile before attributing end-to-end cost; component microbenchmarks alone
   do not identify the world loop's bottleneck.
