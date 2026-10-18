# Verification

Testing is a first-class subsystem with zero external dependencies and a
small in-repository test framework (`tests/testfw.hpp`).

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
  truncation, bad magic), recover-to-good-prefix, event decode fuzzing
  (40k mutations), replay equals original run
- `test_world.cpp`: queue-model unit scenarios, wire roundtrip + fuzz
  (60k mutations), channel determinism, risk reason codes, world
  determinism, seed divergence, full-path traversal via journal, counterfactual
  delta, MD gap recovery convergence, journal replay cross-check

### 2. Differential testing (the backbone)
`RefBook` (simple, obviously-correct) vs `OptBook` (fast) are driven by the
same seeded operation streams; after **every** operation we compare entry
status, produced fills, best prices, and the canonical ladder hash. First
divergence is reported with the op index and diagnosis.

- Property: 40 seeds × 3000 ops + a 50k-op stream, zero divergence
- Adversarial: `arbiter fuzz --target differential`: 100 seeds × ~5000 ops
  (200k ops total) — zero divergence

**This methodology already paid for itself**: when the FlatIdMap (open
addressing with backshift deletion) replaced `std::unordered_map` in the
optimized book, the differential harness caught an inverted backshift
condition at op 1093 of seed 26 within seconds. The fix was then locked in by
a dedicated property test.

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
  error. Both in unit tests (90k mutations) and via `arbiter fuzz --target
  protocol`
- Event encoding: 40k mutations of valid events + random garbage
- Journal: reader validated against corruption in unit tests; ASan build runs
  the whole suite

### 6. Sanitizers
The full test suite runs clean under ASan+UBSan (`build-asan`). TSan builds
verify the SPSC ring tests. The only sanitizer finding during development
(the empty-vector indexing in an early test) was fixed and is covered.

### 7. Invariants
`check_book_invariants` (price ordering, total conservation, leaves/qty
sanity, id uniqueness, no crossed book) is checked after every operation in
property tests, and the matching engine conserves quantity by construction
(taker fills == maker deltas, asserted by the replay cross-check).

## Regression policy

Every bug found becomes a test: the codec zero-fill bug, the 24-vs-20 byte
header, the replace-event id slot, the EOF conflation, the FlatIdMap
backshift, all are pinned by specific tests that fail if the bug returns.