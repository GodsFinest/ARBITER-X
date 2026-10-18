#pragma once
// Execution-economics research: turn predictions into net P&L under explicit
// queue, latency, and cost assumptions.
//
// DESIGN: queue truth is unknowable from L2 alone, so this layer NEVER hides
// behind one fill model. Every backtest is run under three documented queue
// assumptions and the SPREAD across them is reported as model uncertainty:
//
//   Conservative: fill needs traded_qty >= 2*(ahead+ours)  (we are last, prints over-count)
//   Neutral:      fill needs traded_qty >= ahead+ours      (pro-rata-ish FIFO approximation)
//   Aggressive:   fill needs traded_qty >= ahead           (optimistic: our flow adds to prints)
//
// Costs are first-class: spread crossing, per-lot fees, slippage, and the
// opportunity cost of missed fills are tracked separately as gross vs net.
// Latency enters as a decision delay: predictions made at t execute against
// the book at t+latency (stale-quote effect), which is exactly what the
// World counterfactuals measure with real matching.
//
// Sharpe-like ratios over overlapping high-frequency trade samples are
// reported WITH the overlap warning (docs/RESEARCH.md): they are
// descriptive, never the significance test.

#include "arbiter/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace arbiter {
namespace execresearch {

enum class QueueModel : u8 { Conservative = 0, Neutral = 1, Aggressive = 2 };
const char* to_cstr(QueueModel m) noexcept;

// Fill decision for one resting buy/sell of `our_qty` with `ahead` lots
// ahead of us, given `traded_qty` lots printed through our level while we
// rested. Returns filled lots in [0, our_qty] (partial fills modeled).
Qty queue_fill_lots(QueueModel m, Qty ahead, Qty our_qty, Qty traded_qty);

struct CostModel {
    i64 fee_per_lot = 0;       // ticks*qty per lot filled (matches World fee_per_lot units)
    double slippage_ticks = 0.0;  // extra adverse ticks per fill (sensitivity analysis)
};

struct Leg {
    i64 t_ns = 0;
    Side side = Side::Buy;
    Price px = 0;      // execution price (ticks)
    Qty qty = 0;       // lots
    double score = 0;  // signal score that triggered it
};

// One prediction opportunity evaluated through execution.
struct Opportunity {
    i64 signal_t_ns = 0;
    i64 exec_t_ns = 0;   // signal_t + latency (stale execution)
    Side side = Side::Buy;
    Price ref_px = 0;    // decision-time reference (mid or touch)
    Price exec_px = 0;   // realized execution price under the simulation
    Qty qty = 0;
    Qty ahead = 0;         // queue ahead at exec time
    Qty traded_through = 0;  // prints through our level while resting
    double score = 0.0;
    bool filled = false;
    Qty filled_qty = 0;
};

struct ExecReport {
    QueueModel queue = QueueModel::Neutral;
    i64 latency_ns = 0;
    size_t opportunities = 0;
    size_t fills = 0;
    Qty filled_lots = 0;
    double fill_rate = 0.0;
    i64 gross_pnl = 0;   // ticks*lots marked at horizon mid, pre-cost
    i64 fees = 0;
    i64 net_pnl = 0;     // gross - fees - slippage
    double avg_trade = 0.0;
    double hit_rate = 0.0;
    double profit_factor = 0.0;
    double turnover_lots = 0.0;
    double sharpe_like = 0.0;  // descriptive only (see header warning)
    double max_drawdown = 0.0;
    // adverse selection: mean signed markout shortly after fill (ticks)
    double adverse_ticks = 0.0;
    std::string to_string() const;
};

// Evaluate a leg series (already latency-shifted by the caller) under one
// queue model + cost model. `horizon_mid[i]` is the markout mid for leg i;
// `markout_mid[i]` the short-horizon mid for adverse selection.
ExecReport evaluate(const std::vector<Opportunity>& opps, const std::vector<double>& horizon_mid,
                    const std::vector<double>& markout_mid, QueueModel q, CostModel c);

// Cost-sensitive decision gate: trade only if expected edge exceeds costs.
// expected_move_ticks: model-implied favorable move; spread_cost_ticks: half
// spread paid on entry; returns true when fill_prob*edge > costs.
bool should_trade(double expected_move_ticks, double spread_cost_ticks, double fee_ticks,
                  double fill_prob, double confidence = 1.0);

// Latency-budget decomposition (ns) for the information-decay analysis:
// theoretical edge half-life vs summed end-to-end latency.
struct LatencyBudget {
    i64 md_ingest_ns = 0, feature_ns = 0, inference_ns = 0, decision_ns = 0, serialize_ns = 0,
        network_ns = 0, gateway_ns = 0, risk_ns = 0, matching_ns = 0;
    i64 total() const {
        return md_ingest_ns + feature_ns + inference_ns + decision_ns + serialize_ns +
               network_ns + gateway_ns + risk_ns + matching_ns;
    }
    std::string to_string() const;
};

}  // namespace execresearch
}  // namespace arbiter