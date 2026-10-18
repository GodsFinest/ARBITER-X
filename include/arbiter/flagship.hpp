#pragma once
// Flagship research experiment: latency-stage non-fungibility.
//
// RESEARCH QUESTION. For a FIXED total execution-latency budget L, does
// redistributing latency among the stages of a trader's own pipeline
// (observation md_transit / decision / order_transit) change execution
// economics? H0: E[Y | allocation] = f(total only), placement is
// irrelevant given the total. H1: placement matters
// (∂E[Y]/∂l_md != ∂E[Y]/∂l_decision != ∂E[Y]/∂l_order somewhere).
//
// DESIGN (see docs/EXPERIMENTS/flagship_allocation.md for the full record):
//   * Grid: allocations on the md-share slice of the simplex at fixed L:
//     x in {0, .125, .25, .375, .5, .625, .75, .875, 1} plus a balanced
//     interior point. Every cell has the SAME total (exact invariant).
//   * Regimes: flow_model in {poisson, clustered}, the clustered regime
//     arms taker self-excitation (the proposed mechanism); the Poisson
//     regime is the falsification placebo.
//   * Interference: bots_observe_shadow in {false (reactive), true
//     (frozen exogenous flow)}, the flagship measures the reactive world;
//     the frozen mode quantifies interference sensitivity.
//   * Replication: independent macro-seeds (whole simulated worlds). The
//     seed is the unit of replication: NEVER within-world events.
//   * Statistics: for every pair of allocation cells, the PAIRED difference
//     per seed (common random numbers across cells: same master seed,
//     same draw streams by construction) is summarized with mean, HAC CI,
//     and a sign/permutation test across seeds. Multiple testing across the
//     cell-pair family: Holm-Bonferroni. Statistical significance is
//     reported strictly alongside economic effect size (ticks*qty per fill,
//     per 1000 opportunities) and a predefined economic threshold.
//
// HONESTY RULES BAKED IN:
//   * The engine-verified stage trace confirms every cell realized the
//     intended allocation (mean realized per-stage latencies recorded).
//   * Kill conditions (documented in the report): effect absent under
//     clustered flow / present under Poisson / below the economic
//     threshold / unstable across interference modes.
//   * "Paired counterfactual estimate under the simulator's structural
//     assumptions", never "causal proof about real markets".

#include "arbiter/errors.hpp"
#include "arbiter/latency.hpp"
#include "arbiter/metrics.hpp"
#include "arbiter/world.hpp"

#include <string>
#include <vector>

namespace arbiter {

enum class FlowModel : u8 { Poisson = 0, Clustered = 1 };

struct FlagshipConfig {
    // experiment identity
    std::string name = "flagship_allocation";
    // latency budget (trader-side stages; exchange-side held fixed)
    i64 total_latency_ns = 8'000;
    // allocation grid on the md-share slice (fractions of L on observation)
    std::vector<double> md_shares = {0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0};
    // market process
    FlowModel flow_model = FlowModel::Clustered;
    // clustered regime intensity: k in [0,1). Each executed trade shortens
    // the next taker inter-arrival by 1/(1-k) and takers follow the prior
    // taker's side with prob k. CALIBRATION NOTE (measured, seed 2026,
    // 100ms): sign runs long enough to trigger the momentum archetype
    // (|sum signs| >= 3) only emerge for k >= ~0.85, below that, provider
    // replenishment (60/ms) dominates taker pressure (8/ms) and trade
    // prices alternate. Default 0.9 is where the mechanism is active; the
    // report states this is a SIMULATOR PROPERTY, not a calibrated fact.
    double taker_self_excitation = 0.9;
    // interference mode
    bool bots_observe_shadow = false;  // false = reactive world (flagship)
    // replication
    std::vector<u64> seeds = {2026, 2027, 2028, 2029, 2030, 2031, 2032, 2033};
    // horizon / market
    LogicalNs horizon_ns = 100'000'000;
    LogicalNs warmup_ns = 2'000'000;
    // strategy archetype under study (mm = maker-style, momentum = taker)
    // both run; the report separates their economics.
    // economic significance threshold: |Δticks*qty per lot| below this is
    // reported as "not economically material" regardless of p-values.
    double economic_threshold_ticks_qty_per_lot = 1.0;
    // stats
    size_t hac_lag = 2;  // small: seed-level series, mild dependence
    u64 permutation_reps = 5000;
};

// Per-cell, per-seed economic outcome vector.
struct CellSeedOutcome {
    // economics (integer ticks*qty)
    i64 mm_marked_pnl = 0;
    i64 mom_marked_pnl = 0;
    i64 mm_max_drawdown = 0;
    // execution
    u64 mm_fills = 0;        // fill EVENTS
    u64 mm_filled_qty = 0;  // fill LOTS
    u64 mom_fills = 0;
    u64 mom_filled_qty = 0;
    u64 mm_orders_sent = 0;
    u64 mm_cancels_sent = 0;
    u64 mm_orders_rejected = 0;
    // adverse selection (mean markout, ticks)
    double mm_markout_100us = 0.0;
    double mom_markout_100us = 0.0;
    // realized stage latency means (ns), verifies the allocation took hold
    double mean_md_transit_ns = 0.0;
    double mean_decision_ns = 0.0;
    double mean_order_transit_ns = 0.0;
    // engine state
    u64 final_state_hash = 0;
    u64 events = 0;
};

struct CellResult {
    std::string name;             // e.g. "md0.500"
    LatencyAllocation alloc;
    std::vector<CellSeedOutcome> per_seed;  // aligned with cfg.seeds
    // cross-seed summary (mean over seeds)
    double mean_mm_pnl = 0.0;
    double mean_mom_pnl = 0.0;
    double mean_mm_fills = 0.0;
    double mean_mm_filled_qty = 0.0;
    double mean_mom_filled_qty = 0.0;
    double mean_mm_markout = 0.0;
    double mean_mom_markout = 0.0;
};

// A paired cell-pair comparison (primary contrasts defined by the design).
struct PairComparison {
    std::string cell_a, cell_b;      // names
    LatencyAllocation alloc_a, alloc_b;
    // per-seed paired differences (b - a) of the endpoint under study
    std::vector<double> mm_pnl_diff;
    std::vector<double> mom_pnl_diff;
    std::vector<double> mm_fills_diff;
    std::vector<double> mm_markout_diff;
    // inference on mm_pnl_diff (primary endpoint)
    double mean_diff = 0.0;
    double se_iid = 0.0;
    double ci95_lo = 0.0, ci95_hi = 0.0;
    double p_permutation = 1.0;      // two-sided sign-permutation across seeds
    double p_holm = 1.0;            // Holm-adjusted across the pair family
    // economic interpretation inputs
    double pnl_diff_per_lot = 0.0;  // mean PnL diff per filled lot (ticks*qty/lot)
    bool statistically_significant = false;
    bool economically_material = false;
};

struct FlagshipResult {
    FlagshipConfig cfg;                       // exact definition (echoed)
    u64 definition_hash = 0;                   // identity of the whole design
    std::string flow_model_name;              // "poisson" | "clustered"
    std::vector<CellResult> cells;
    std::vector<PairComparison> pairs;         // contrast family
    // primary-contrast verdict text (falsification logic lives here)
    std::vector<std::string> findings;
    // environment
    std::string compiler, platform, created_utc;
};

FlagshipResult run_flagship(const FlagshipConfig& cfg);

// Artifact: complete reproducibility record (JSON) + human report (Markdown).
std::string flagship_to_json(const FlagshipResult& r);
std::string flagship_report_md(const FlagshipResult& r);
Status write_flagship_artifacts(const std::string& dir, const FlagshipResult& r);

// Reproduce: re-run from a written flagship artifact JSON and verify every
// recorded per-seed state hash + economics bit-for-bit.
struct FlagshipReproReport {
    bool ok = false;
    u64 cells_checked = 0;
    u64 seeds_checked = 0;
    u64 mismatches = 0;
    std::string detail;
};
FlagshipReproReport flagship_reproduce(const std::string& artifact_path);

}  // namespace arbiter