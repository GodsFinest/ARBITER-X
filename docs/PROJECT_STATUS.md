# Project status

This file records the current, presentation-relevant state. Historical phase
logs and superseded audits are intentionally not kept in the repository.

## Implemented and verified

| Area | Evidence |
|---|---|
| Reference and optimized order books | unit, invariant, randomized differential, and corpus tests |
| Deterministic world execution | repeated-run hash/event equality tests and `arbiter verify` |
| Journal, replay, and recovery | CRC corruption, truncation, prefix recovery, and replay tests |
| Wire protocol and network faults | round-trip/fuzz tests plus MD gap-recovery convergence |
| Risk and strategy path | per-account reason-code tests and journal path traversal |
| Full-world snapshots and forks | serialize/restore/continue identity and divergent-fork tests |
| Counterfactual artifacts | exact reproduction and tamper-detection tests |
| Research pipeline | leakage, split, feature, model, execution, and manifest tests |

## Build gates

The default build enables `-Wall -Wextra -Wpedantic` on GCC/Clang. CI builds
with warnings as errors, runs all CTest targets, validates Python syntax, and
checks shell scripts. A separate CMake option enables AddressSanitizer and
UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DARBITER_WARNINGS_AS_ERRORS=ON \
  -DARBITER_ENABLE_SANITIZERS=ON
cmake --build build-sanitize -j 8
ctest --test-dir build-sanitize --output-on-failure
```

## Known boundaries

The simulator is stylized, not venue-calibrated. The deterministic core is
single-threaded. Order and execution channels do not implement recovery for a
dropped client message. Bitwise equality is scoped to the same architecture,
OS, compiler, and flags. The flagship economic result is currently uncertain.

These boundaries are deliberate and documented in [LIMITATIONS.md](LIMITATIONS.md).