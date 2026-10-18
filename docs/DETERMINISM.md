# Determinism contract

## What is guaranteed

Given `(initial state, configuration, master_seed)`, ARBITER-X produces
bitwise-identical results across repeated runs on the **same architecture,
OS, compiler, and build flags**:

* identical event streams and journal bytes
* identical book state at every point
* identical fills, positions, risk decisions
* identical canonical state hashes at every checkpoint

Verified by:
* `tests/test_world.cpp: world_deterministic_across_runs` (full pipeline,
  hash timeline equality)
* `arbiter verify` (two separate runs compared live)
* `arbiter demo replay`

The CLI distinguishes two hash domains. `simulate` prints a **world state
hash**, covering the complete causal simulation state. `replay` and
`timetravel` print a **book state hash**, covering the reconstructed exchange
book. They serve different verification scopes and are not expected to be
equal.

## How it is achieved

1. **One logical clock.** `LogicalNs` (int64) is the only time that affects
   behavior. `std::chrono::*::now()` appears only in benchmark/statistics
   code and never in the engine.
2. **Integers-only state.** Prices are int64 ticks, quantities uint64, money
   is int64 in ticks*qty. There is no floating-point state anywhere.
3. **Seeded randomness.** All stochastic behavior derives from SplitMix64
   streams addressed by `(master_seed, stream_id)`, market sim, each network
   channel, each strategy. No global RNG, no time-seeded RNG.
4. **Total event order.** The scheduler totally orders events by
   `(logical_ns, rank, insertion_counter)`; insertion counters are themselves
   a deterministic function of the run.
5. **No hidden state.** All event-relevant state participates in
   `state_hash()` (`ARBITERX/WORLD/2`, clock, book + account ownership,
   strategy incl. view/queue-ahead, per-account risk, sim + RNG progress,
   channel RNG/burst/corruption, feed buffers, in-flight frames, markouts,
   scheduler contents); unexpected divergence between two runs is therefore
   detectable, not just invisible. Pure instrumentation (journals,
   histograms, latency-tracing tables) is excluded by design, see
   docs/REPRODUCIBILITY.md for the exact guarantee.

## What is NOT guaranteed (say it plainly)

* **Cross-architecture / cross-compiler bitwise equality is NOT claimed.**
  The stochastic draws use IEEE double for exponential inter-arrivals and
  unit variates; integer division for skews is C++-defined but compiler
  flags could theoretically change codegen around floating point.
  Repeatability is guaranteed only within the same documented build and
  platform. Research-artifact reproduction therefore uses exact hashes only
  when compiler and platform provenance match; otherwise it compares integer
  counters exactly and floating metrics at a documented `1e-9` relative
  tolerance, failing at the first field outside tolerance.
* **Concurrency does not touch the deterministic core.** The SPSC ring is a
  measured experiment; the engine is single-threaded so thread scheduling
  cannot change results.
* **Filesystem durability** follows the journal contract: `flush()` puts
  bytes in the OS page cache, `sync()` fsyncs. A machine-level power loss
  loses everything after the last sync, a process crash loses everything
  after the last flush (tested at flush granularity).

## Canonical hashing

`Hasher` is FNV-1a 64 with explicit domain tags. Canonical book hashing
covers only behaviorally-relevant fields (level prices, totals, ordered
`(id, leaves, qty)` per level) so that two *implementations* with identical
behavior hash identically, the reference and optimized books pass the same
hash tests despite completely different data structures.

Collisions are possible (non-cryptographic 64-bit); the differential tests
therefore compare full ladder *views*, not just hashes, at every operation.