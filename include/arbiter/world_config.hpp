#pragma once
// WorldConfig: the complete behavior-relevant configuration of one
// deterministic world. It lives in its own header so the snapshot schema
// can persist the FULL configuration (worldfork must reconstruct the exact
// parent configuration) without creating an include cycle with world.hpp.
//
// The configuration is serialized field-by-field in world snapshots (v13+)
// and validated by validate_world_config() before any World construction.

#include "arbiter/net.hpp"
#include "arbiter/risk.hpp"
#include "arbiter/sim.hpp"
#include "arbiter/strategy.hpp"
#include "arbiter/types.hpp"

#include <string>

namespace arbiter {

struct WorldConfig {
    // Deterministic identity of the configuration: covers every field that
    // can change execution behavior (tag ARBITERX/WORLDCFG/3, includes
    // risk.enabled, mm.enabled, momentum.enabled). Journal/instrumentation
    // fields (journal_events, journal_path, checkpoint/hash cadence) are
    // excluded: they never feed back into the deterministic world. Any new
    // behavior-changing field MUST be added to WorldConfig::hash and bump
    // the tag, see tests/test_quant.cpp (config_hash_covers_all_fields).
    u64 hash() const;

    // --- logical latency knobs (the counterfactual variables) ---
    i64 algo_latency_ns = 300;         // strategy decision -> order frame departs
    // Cancel-specific decision latency (message-type granularity, §27):
    // cancels are smaller/simpler messages than new orders, and their
    // economics (queue-exposure termination under adverse flow) differ
    // structurally. Defaults to algo_latency_ns so existing behavior is
    // bit-identical unless explicitly set.
    i64 cancel_latency_ns = -1;       // < 0 = "follow algo_latency_ns"
    i64 exchange_processing_ns = 150;  // gateway accept -> match
    i64 publish_latency_ns = 80;       // match -> execution frame departs
    NetProfile md_channel{200, 300, 0.0, 0.0, 0, 200, 0, 0.0};      // exchange -> strategy
    NetProfile order_channel{250, 200, 0.0, 0.0, 0, 200, 0, 0.0};   // strategy -> exchange
    NetProfile exec_channel{200, 250, 0.0, 0.0, 0, 200, 0, 0.0};    // exchange -> strategy

    // --- market ---
    SimConfig sim;
    LogicalNs horizon_ns = 200'000'000;   // 200 ms logical default
    u64 max_events = 2'000'000;
    // exchange fee schedule: flat fee per lot per fill (integer ticks*qty
    // units, cash accounting stays integer). A counterfactual variable:
    // fork the world with fee_per_lot changed and measure the PnL shift.
    i64 fee_per_lot = 0;
    // Interference control (docs/COUNTERFACTUALS.md "fixed vs reactive
    // world"): when true, bots observe a SHADOW touch computed WITHOUT
    // strategy orders, so their behavior is exogenous to the strategies,
    // the counterfactual then measures strategy-vs-frozen-market effects
    // (no SUTVA violation from bot reactions). When false (default), bots
    // see the live touch and may react to strategy quotes: the measured
    // effect includes market feedback. Comparing the two modes estimates
    // interference bias in this simulator.
    bool bots_observe_shadow = false;

    // --- risk ---
    RiskConfig risk;
    Price book_min_price = 1000;
    Price book_max_price = 2000;

    // effective decision->departure delay for a message kind (cancel falls
    // back to algo_latency_ns when cancel_latency_ns < 0)
    i64 decision_latency_for(bool is_cancel) const noexcept {
        return is_cancel && cancel_latency_ns >= 0 ? cancel_latency_ns : algo_latency_ns;
    }

    // Full config validation (see world.cpp). Returns "" when the
    // configuration is runnable; otherwise a human-readable reason. CLIs call
    // this BEFORE constructing a World so a bad config is a normal reported
    // error; the World constructor itself rejects invalid configs loudly as a
    // backstop.
    std::string validate() const;

    // --- strategies ---
    StrategyConfig mm;
    MomentumConfig momentum;

    // --- instrumentation ---
    bool journal_events = true;
    std::string journal_path;          // default: artifacts/world_<seed>.journal
    LogicalNs warmup_ns = 0;           // strategies idle until this logical time
    u64 checkpoint_every_events = 0;   // 0 = no Checkpoint events
    u64 hash_every_events = 256;
    // stage tracing (decision->fill decomposition). Pure instrumentation:
    // must never alter behavior; equivalence is test-enforced
    // (world_tracing_off_matches_tracing_on).
    bool stage_tracing = true;
};

// Full config validation (see world.cpp). Returns "" when the configuration
// is runnable; otherwise a human-readable reason.
std::string validate_world_config(const WorldConfig& c);

inline std::string WorldConfig::validate() const { return validate_world_config(*this); }

}  // namespace arbiter
