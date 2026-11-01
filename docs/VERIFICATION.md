# Verification

Tests use the in-repository framework in `tests/testfw.hpp` and have no
external runtime dependencies.

## Current test inventory

The current suite contains 168 `AX_TEST` cases across 9 test binaries:

| binary | cases |
|---|---:|
| `test_book` | 29 |
| `test_core` | 9 |
| `test_diff` | 7 |
| `test_flagship` | 7 |
| `test_journal` | 9 |
| `test_quant` | 26 |
| `test_spsc` | 3 |
| `test_world` | 57 |
| `test_worldsnap` | 21 |
| **total** | **168** |

This count is re-auditable with `grep -c "^AX_TEST(" tests/*.cpp`. CTest
registers the same 9 binaries, 4 CLI checks (including 2 `WILL_FAIL` cases),
and 4 benchmark smoke checks: 17 entries in total (`ctest --test-dir build
-N`).

## Layers

### 1. Unit tests (per component)
- `test_core.cpp`: RNG determinism, FNV-1a known vectors, CRC32 known
  vectors, histogram percentiles, config parsing, FlatIdMap vs
  `std::unordered_map` property test (200k random ops incl. rehash growth
  and backshift-heavy deletion)
- `test_book.cpp`: price-time priority, partial fills, IOC/FOK/GTD, cancel
  semantics, replace-loses-priority, validation errors, empty-book best
  tracking, invariants after random op streams
- `test_journal.cpp`: roundtrip, corruption detection (bit flip → CRC error,
  truncation, bad magic), recover-to-good-prefix, 20,000 mutated-event
  iterations with 1–4 byte mutations each plus 20,000 random-garbage
  iterations, replay equals original run
- `test_world.cpp`: queue-model unit scenarios, wire roundtrip + fuzz
  (30,000 mutated-frame iterations with 1–6 byte mutations each plus 30,000
  random-garbage iterations), channel determinism, risk reason codes, world
  determinism, seed divergence, full-path traversal via journal, counterfactual
  delta, MD gap recovery convergence, journal replay cross-check

### 2. Differential testing
`RefBook` (simple reference implementation) and `OptBook` (optimized) are
driven by the same seeded operation streams; after **every** operation we
compare entry status, produced fills, best prices, and the canonical ladder
hash. First divergence is reported with the op index and diagnosis.

- Property: 40 seeds × 3000 ops + a 50k-op stream, zero divergence
- Adversarial: `arbiter fuzz --target differential`: 100 seeds × 2,000 ops
  (200,000 ops total with the committed reproduction command) — zero
  divergence

The differential harness previously caught an inverted FlatIdMap backshift
condition at operation 1093 of seed 26. A dedicated property test covers the
regression.

### 3. Trace minimization
`diff::minimize_ops` implements delta debugging over failing operation
streams (chunk removal + granularity reduction). Tested against synthetic
monotone predicates (1000-op stream reduced to the 2 load-bearing ops).

### 4. Recovery / crash testing
- Snapshot + journal-tail == full replay at **every** checkpoint (tested
  exhaustively over sampled points, and over all 1200 seqs in one test)
- Journal truncation at arbitrary byte offsets → `journal_recover` restores
  the last verifiable frame boundary → replay reproduces exactly the
  state at that event count (per-event hashes recorded during the run)
- World-level: `arbiter demo recovery` truncates a live journal mid-flight
  and recovers

### 5. Fuzzing
- Wire frames: bit flips, truncations, bad magic, random garbage, decoders
  never crash (ASan-verified), every malformed frame rejected with a precise
  error. Unit tests cover 30,000 mutated frames plus 30,000 random-garbage
  iterations; `arbiter fuzz --target protocol` runs 200,000 mutation
  iterations via `scripts/reproduce_all.sh`.
- Event encoding: 20,000 mutated-event iterations plus 20,000 random-garbage
  iterations
- Snapshot restore: randomized checkpoint placement and restore across 8 seeds
- Journal: reader validated against corruption in unit tests; the ASan+UBSan
  build runs the whole suite

### 6. Sanitizers
The full test suite runs clean under ASan+UBSan (`build-sanitize`). TSan is
outside the current CMake sanitizer configuration. The only sanitizer finding
during development (the empty-vector indexing in an early test) was fixed and
is covered.

### 7. Invariants
`check_book_invariants` (price ordering, total conservation, leaves/qty
sanity, id uniqueness, no crossed book) is checked after every operation in
property tests, and the matching engine conserves quantity by construction
(taker fills == maker deltas, asserted by the replay cross-check).

## Regression policy

Every bug found becomes a test: the codec zero-fill bug, the 24-vs-20 byte
header, the replace-event id slot, the EOF conflation, the FlatIdMap
backshift, all are pinned by specific tests that fail if the bug returns.
