# Reproducibility

What "reproducible" means here, exactly.

## Determinism contract (same arch/build)

`(dataset_hash, config_hash, seed, code_version)` → bit-identical results.
Covered by: `arbiter verify` (IDENTICAL), `test_world` determinism,
`test_quant` pipeline determinism (`run_study` twice → same `result_hash`),
snapshot restore contracts, artifact reproduce commands. NOT claimed:
cross-architecture / cross-compiler bitwise equality (see DETERMINISM.md).

## State-hash guarantee (`ARBITERX/WORLD/2`)

FNV-1a 64 over a documented serialization (fixed order, sorted maps/sets).
Covers all EVENT-RELEVANT state: clock, progress counters, book ladder +
order-account ownership, full strategy state (view ladder, queue-ahead,
confirm_seq), per-account risk, sim dynamics + RNG progress, channel RNG +
burst + corruption stream, feed progress + pending buffer, retransmit-ring
logical contents, gateway ids, in-flight frames, markout probes, scheduler
contents. Excludes: journal bytes, RunStats histograms/timelines, wall-clock,
latency-tracing tables (`order_sent_t_`/`stages_`, histogram inputs only),
config identity (lives in `config_hash()`/`world_id()` so fork-divergence
assertions stay meaningful). Non-cryptographic: equality is evidence, and
differential tests compare full views, not just hashes.

## Config identity (`ARBITERX/WORLDCFG/3`)

Every behavior-changing field is hashed (explicit per-field test:
`config_hash_covers_all_behavior_fields`); instrumentation-only fields
(journal path/toggles, checkpoint/hash cadence) are excluded by design. Any
new behavior field MUST extend the hash and bump the tag.

## Network RNG streams (fixed-draw contract)

`Channel::next_delivery` consumes exactly 7 draws per message in fixed order
(loss, burst-arm, dup, reorder-decide, reorder-magnitude, corrupt, jitter),
including drops and mid-burst messages. Profiles gate interpretation, never
alignment, varying one network variable cannot shift unrelated randomness
(tested: loss patterns invariant to jitter/dup/corrupt settings).

## Research manifests (`ARBITERX/RESEARCH-MANIFEST/3`, `.../RESULT/1`)

Each `experiments/EXP-*/` holds `manifest.json` (run/dataset/config/seed/
feature/model/code identity plus Git/compiler/platform provenance),
`results.json` (metrics/counters/findings + `result_hash`), and `report.md`.
The deterministic manifest hash excludes informational build provenance, so a
documentation-only commit does not invalidate an otherwise identical result.
Doubles are hashed through a stable 17-digit decimal representation. `arbiter
research reproduce EXP-00X` uses exact `result_hash` equality when compiler and
platform provenance match. Across a different compiler or platform, bitwise
floating-point equality is not claimed: metric names and order must match,
integer counters remain exact, and floating metrics must satisfy
`|stored-rebuilt| <= 1e-9 * max(1, |stored|, |rebuilt|)`. The command prints a
distinct cross-platform result, reports the first numeric difference even when
it is tolerated, and names the first field outside that contract.

## What to do when reproduction fails

1. Compare `manifest_hash` first (input divergence) vs `result_hash` (same-build
   code drift) or the first field-level mismatch (cross-platform drift).
2. Compare `git rev-parse` with manifest `git_commit`, then compiler/platform
   provenance.
3. `experiment --reproduce` names the first mismatching variant for World-leg
   artifacts. Never "fix" an artifact by hand, regenerate from the pipeline.