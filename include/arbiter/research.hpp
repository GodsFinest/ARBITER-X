#pragma once
// Reproducible research runs: manifests, artifact hashing, report generation.
//
// A ResearchRun binds (dataset_hash, config_hash, seed, code_version,
// feature_version, model_version) into one manifest with one hash. Reproduce
// = recompute and compare every hash; any divergence is reported LOUDLY with
// the first mismatching component named. Mirrors the Experiment artifact
// discipline (definition_hash + reproduce) for the data-driven leg.

#include "arbiter/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace arbiter {
namespace research {

inline constexpr const char* kResearchCodeVersion = "quant_v1";

// Compiler/platform fingerprint (informational; determinism is claimed
// same-arch/build only, docs/REPRODUCIBILITY.md).
std::string compiler_id();
std::string platform_id();
// Wall-clock timestamp string (informational only, never hashed).
std::string utc_now();

struct ResearchManifest {
    std::string run_id;          // e.g. "EXP-001/seed2026"
    std::string experiment_id;   // e.g. "EXP-001"
    std::string git_commit;      // informational provenance, not identity
    u64 dataset_hash = 0;
    std::string dataset_name;
    // Dataset provenance for exact rebuild (manifest v2): synthetic streams
    // are regenerated from (synth_seed, synth_events, synth_bias); CSV runs
    // reload dataset_name as a path (must still exist).
    std::string dataset_kind = "synthetic";  // "synthetic" | "csv"
    u64 synth_seed = 2026;
    size_t synth_events = 6000;
    double synth_bias = 0.0;
    // Study parameters (manifest v2): every knob that changes numbers.
    double imbalance_thresh = 0.2;
    i64 exec_latency_ns = 10'000;
    i64 fee_per_lot = 0;
    size_t sample_every = 1;
    size_t max_samples = 20000;
    u64 config_hash = 0;
    u64 seed = 0;
    std::string feature_version;
    std::string model_version;
    std::string code_version = kResearchCodeVersion;
    std::string compiler;        // informational provenance, not identity
    std::string platform;        // informational provenance, not identity
    std::string created_utc;     // informational only (never hashed)
    // FNV over deterministic inputs above. Build provenance and the stored
    // hash itself are excluded so unchanged results survive a docs-only commit.
    u64 manifest_hash = 0;
    u64 compute_hash() const;
    std::string to_json() const;
};

// Result bundle for one experiment run (JSON-serializable metrics + hashes).
struct ResearchResult {
    ResearchManifest manifest;
    // metrics: ordered (name, value) pairs, fixed order is part of the artifact.
    std::vector<std::pair<std::string, double>> metrics;
    // integer counters (fills, samples, divergences...).
    std::vector<std::pair<std::string, long long>> counters;
    // short textual findings (hypothesis verdicts incl. FAILED ones).
    std::vector<std::string> findings;
    u64 result_hash = 0;  // FNV over manifest_hash + metrics + counters
    u64 compute_result_hash() const;
    std::string metrics_json() const;
};

// Human-readable Markdown report (Question/Hypothesis/Dataset/Method/
// Results incl. robust CIs/Execution/Costs/Robustness/Failure analysis/
// Conclusion/Reproducibility manifest).
std::string render_report(const std::string& title, const std::string& question,
                          const std::string& hypothesis, const ResearchResult& r,
                          const std::vector<std::string>& sections);

// Write experiments/<EXP>/{manifest.json,results.json,report.md}.
// Creates directories. Returns false with `err` set on I/O failure.
bool write_artifact(const std::string& exp_dir, const ResearchResult& r, const std::string& report_md,
                    std::string& err);

// Reproduce check: recompute manifest_hash + result_hash and compare against
// stored JSON files. Returns {ok, detail}; ok=false names the mismatch.
// Parse a written manifest.json back into a ResearchManifest (for end-to-end
// re-runs and integrity checks). Returns false with `err` set when the file
// is missing, corrupt, tampered, or a superseded v1 manifest.
// `result_hash_out` receives the stored results.json result_hash.
bool read_manifest(const std::string& exp_dir, ResearchManifest& m, u64& result_hash_out,
                   std::string& err);

// Read the metric and counter values from results.json in writer order.
// The standalone manifest remains authoritative and is integrity-checked by
// read_manifest().
bool read_result(const std::string& exp_dir, ResearchResult& result, std::string& err);

struct ResultComparison {
    bool ok = false;
    std::string first_difference;
    std::string first_mismatch;
};

// Compare named values in writer order. Counters and names are always exact.
// Floating metrics use |a-b| <= relative_tolerance * max(1, |a|, |b|);
// pass zero for an exact comparison.
ResultComparison compare_result_values(const ResearchResult& stored,
                                       const ResearchResult& rebuilt,
                                       double relative_tolerance);

}  // namespace research
}  // namespace arbiter