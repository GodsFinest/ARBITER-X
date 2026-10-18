# Performance methodology

Performance results are produced by `arbiter_bench`; generated logs are not
committed because wall-clock measurements depend on machine, toolchain, build
type, and system load.

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