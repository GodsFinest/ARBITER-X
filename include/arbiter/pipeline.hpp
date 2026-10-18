#pragma once
// End-to-end quantitative research pipeline (deterministic, reproducible).
//
// One entry point: `run_study`, executes the full loop on a MarketEvent
// stream from ANY source (real CSV via data::parse_csv, or the deterministic
// synthetic generator below):
//
//   events -> ladder/mid series -> as-of features -> horizon labels ->
//   chronological split -> baselines + logistic + stump (train) ->
//   walk-forward + held-out OOS (evaluate w/ robust stats) ->
//   decay curves + conditional buckets + ablation + permutation null ->
//   queue/latency/cost execution economics -> ResearchResult + report.
//
// Sampling: event-time (every depth/trade event is one sample) by default;
// `SAMPLE_EVERY` throttles for long streams. Fixed-time bars are supported by
// pre-bucketing events (see docs/RESEARCH.md).

#include "arbiter/data.hpp"
#include "arbiter/execution_research.hpp"
#include "arbiter/features.hpp"
#include "arbiter/labels.hpp"
#include "arbiter/research.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace arbiter {
namespace pipeline {

// ---- event -> book/mid series (L2 level-set semantics, docs/DATA.md) ----

struct SeriesSample {
    i64 ts_ns = 0;
    LadderView ladder;   // top-20 per side retained
    double mid = 0.0;
    double spread_rel = 0.0;
};

// Replay a MarketEvent stream into per-event ladder snapshots. Deterministic.
// Semantics: depth_l2/quote_l1 rows are ABSOLUTE level snapshots (price->qty
// is set); add rows are incremental (+qty, with L3 id tracking); cancel
// removes (L3: by order id; L2: subtracts qty floored at 0); trade consumes
// qty at the traded price (floored at 0); quote_l1 sets the touched level.
// This is an L2 APPROXIMATION (no per-order queues), queue uncertainty is
// carried explicitly into execution_research's three queue models.
std::vector<SeriesSample> build_series(const std::vector<data::MarketEvent>& events);

// ---- deterministic synthetic L2 stream (no external data needed) ----

struct SyntheticConfig {
    u64 seed = 2026;
    size_t n_events = 5000;
    Price start_mid = 1500;
    i64 dt_ns = 500'000;      // 0.5ms between events
    double imbalance_bias = 0.0;  // [+/-]: injected static imbalance -> drift coupling
    double drift_per_event = 0.0;  // mid drift per event in ticks (0 = martingale-ish)
};

// Deterministic (Rng streams only). When imbalance_bias != 0, bid depth is
// systematically larger and the mid drifts up with imbalance, a KNOWN
// synthetic alpha for pipeline validation (must be REPORTED as synthetic).
std::vector<data::MarketEvent> synthesize(SyntheticConfig cfg);

// ---- study configuration + outputs ----

struct StudyConfig {
    std::string experiment_id = "EXP-001";
    std::string run_id;
    u64 seed = 2026;
    std::string dataset_name = "synthetic";
    u64 dataset_hash = 0;
    u64 config_hash = 0;         // World config when execution leg uses the World
    double train_frac = 0.6, valid_frac = 0.2, held_out_frac = 0.1;
    double imbalance_thresh = 0.2;
    i64 exec_latency_ns = 10'000;  // 10us decision->execution delay
    i64 fee_per_lot = 0;
    size_t sample_every = 1;       // event-time thinning
    size_t max_samples = 20000;    // compute guard
};

struct DecayPoint {
    i64 horizon_ns = 0;
    double cond_ret_pos = 0.0;  // mean future ret | score > 0  (HAC CI in report)
    double hac_lo = 0.0, hac_hi = 0.0;
    double nover_lo = 0.0, nover_hi = 0.0;  // IID CI on non-overlapping subsample
    double ic = 0.0;
    size_t n = 0;
    size_t n_nonover = 0;
};

struct ConditionalBucket {
    std::string dim;  // e.g. "spread", "imb_decile"
    std::string bucket;
    double mean_ret = 0.0, hac_lo = 0.0, hac_hi = 0.0;
    double hit = 0.0;
    size_t n = 0;
};

struct ModelRow {
    std::string model;   // zero | prev | spread | imb1 | ofi | logistic | stump
    std::string split;   // train | valid | test | heldout | walkforward-mean
    double accuracy = 0.0, auc = 0.0, ic = 0.0, hit = 0.0, coverage = 0.0;
    double cond_ret = 0.0, cond_lo = 0.0, cond_hi = 0.0;
    size_t n = 0;
};

struct StudyResult {
    research::ResearchResult result;          // hashes + metrics + findings
    std::vector<DecayPoint> decay_imb1;       // flagship decay curve (top-book imbalance)
    std::vector<DecayPoint> decay_ofi;
    std::vector<ModelRow> model_rows;
    std::vector<ConditionalBucket> conditional;
    std::vector<std::pair<std::string, double>> queue_spread;  // net PnL per queue model
    std::string report_md;
};

// Full deterministic study. No I/O except through `cfg` (pure compute).
StudyResult run_study(const std::vector<SeriesSample>& series, const StudyConfig& cfg);

}  // namespace pipeline
}  // namespace arbiter