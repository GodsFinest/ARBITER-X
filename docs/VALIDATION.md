# Validation

Build and run the suite with CMake and CTest as shown in the README.
`scripts/reproduce_all.sh` exercises the complete core reproduction workflow.
GitHub Actions checks GCC 13, Clang 18, Apple Clang, and ASan/UBSan.

The core suite covers matching semantics, randomized differential streams,
protocol mutation, risk reservations, transport faults, journal recovery,
snapshot continuation, forks, and latency allocation. Regression tests reject
out-of-range artifact durations before conversion to signed integers.

Local Release verification passed all 22 core CTest entries and all 11 LAB
entries. Combined core/LAB ASan/UBSan verification passed 32 entries. A fresh
standalone core checkout passed without LAB; LAB also passed against a relocated
installed core package with the source checkout unavailable.

All six historical studies reproduced after extraction. The default seed-42,
50-ms journal remained byte-identical to the original monolith; reference and
optimized replay both produced book hash 2898256232837298603. World hash was
12505037182163188822. Historical experiment records retain their original bytes.

Full-world snapshots use format 15 and explicitly reject format 14. Journal,
wire, and book-checkpoint formats retain their existing formats. State hashes
are noncryptographic fingerprints; failed restoration is not guaranteed to be
transactional. Historical L2 fills remain approximate.

See [performance](PERFORMANCE.md) for hardware, workloads, and benchmark evidence.
