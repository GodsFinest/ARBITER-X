#pragma once
// Market-microstructure feature engine (versioned definitions).
//
// FEATURE_VERSION pins exact semantics: any formula/window change MUST bump
// it and keep the old version available for reproduction. Research artifacts
// record feature_version alongside dataset/config hashes.
//
// Windows are DOCUMENTED constants (not magic): WINDOWS_NS covers 1ms..1s,
// HORIZONS_NS covers 1ms..1s. All features are as-of: computed from state at
// or before `as_of_ns` only (leakage tests in tests/test_quant.cpp prove
// that perturbing post-as-of events cannot change earlier features).
//
// Formulas:
//   imbalance_k = (bid_k - ask_k) / (bid_k + ask_k), 0 denominator -> 0.0
//   microprice  = (ask*bbid_qty + bid*ask_qty... precisely:
//                 (ask_price*bid_qty + bid_price*ask_qty)/(bid_qty+ask_qty),
//                 0 denominator -> mid.
//   OFI (Cont et al. 2014, top level): over each book update,
//     e = dq_bid - dq_ask where dq = signed size change at the price level
//         (up-move in bid price contributes +depth, down-move -depth, etc.)
//   realized_vol(window) = stdev of log mid-returns sampled at event times
//         within the window (0 if <2 samples).

#include "arbiter/book_common.hpp"
#include "arbiter/types.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace arbiter {
namespace features {

inline constexpr const char* kFeatureVersion = "feat_v1";

// Documented sampling/lookback windows (ns). Rationale: 1ms..1s spans the
// simulator's quote cadence (25us) up to regime-relevant scales; horizons
// match LABEL horizons so decay curves are directly comparable.
inline constexpr i64 kWindowsNs[] = {1'000'000, 5'000'000, 10'000'000, 50'000'000,
                                     100'000'000, 500'000'000, 1'000'000'000};
inline constexpr size_t kNumWindows = 7;
inline constexpr int kImbalanceDepths[] = {1, 2, 5, 10, 20};
inline constexpr size_t kNumImbalanceDepths = 5;

// ---- pure book-shape features (from a canonical ladder snapshot) ----

double mid_price(const LadderView& b);            // (bb+ba)/2, 0 if either side empty
double spread_ticks(const LadderView& b);         // ba-bb, -1 if either side empty
double relative_spread(const LadderView& b);      // spread/mid, -1 if undefined
double depth_imbalance(const LadderView& b, int levels);  // [-1,1], 0 on empty denominator
double microprice(const LadderView& b);           // qty-weighted, in [bb,ba] when defined
// microprice deviation from mid, in ticks (signed; 0 when undefined)
double microprice_deviation_ticks(const LadderView& b);
Qty level_bid_qty(const LadderView& b, int levels);
Qty level_ask_qty(const LadderView& b, int levels);

// ---- order-flow imbalance (event-driven, top-of-book + multi-level) ----

// One book-delta observation (call per L1/L2 update, as-of ordered).
struct BookDelta {
    Price bid_px = 0, ask_px = 0;
    Qty bid_qty = 0, ask_qty = 0;  // top-level sizes
};

// Cont-style OFI accumulator. Feed strictly as-of ordered deltas; query sums
// over trailing windows in O(log n) via prefix sums. Deterministic (integer
// lots internally).
class OfiAccumulator {
public:
    void observe(i64 ts_ns, const BookDelta& d);
    // Signed OFI lots summed over (as_of - window_ns, as_of]. 0 with no data.
    double windowed(i64 as_of_ns, i64 window_ns) const;
    // Total lifetime OFI (diagnostic).
    i64 total() const { return total_; }
    void clear();

private:
    struct Tick {
        i64 ts = 0;
        i64 e = 0;
        i64 prefix = 0;  // sum of e up to and including this tick
    };
    std::vector<Tick> ticks_;
    BookDelta prev_{};
    bool have_prev_ = false;
    i64 total_ = 0;
};

// ---- windowed signed trade / cancel flow ----

struct FlowEvent {
    i64 ts = 0;
    i64 signed_qty = 0;  // +buy / -sell lots
    bool is_cancel = false;
};

class FlowWindows {
public:
    void add_trade(i64 ts_ns, Side aggressor, Qty qty);
    void add_cancel(i64 ts_ns, Side side, Qty qty);
    // Signed trade lots over the trailing window (as-of). O(log n).
    double signed_volume(i64 as_of_ns, i64 window_ns) const;
    double buy_volume(i64 as_of_ns, i64 window_ns) const;
    double sell_volume(i64 as_of_ns, i64 window_ns) const;
    // (buys - sells)/(buys + sells) in lots; 0 on empty.
    double trade_imbalance(i64 as_of_ns, i64 window_ns) const;
    double cancel_imbalance(i64 as_of_ns, i64 window_ns) const;  // signed cancel lots
    void clear();

private:
    struct Ev {
        i64 ts = 0;
        // prefix sums up to and including this event
        double psigned_trade = 0.0, pbuy = 0.0, psell = 0.0, pcancel_buy = 0.0, pcancel_sell = 0.0;
    };
    // range sum of one prefix field over (lo, hi]
    static double range(const std::vector<Ev>& v, size_t lo, size_t hi, double Ev::*field);
    // index range of events with ts in (lo, hi]
    std::pair<size_t, size_t> span(i64 lo, i64 hi) const;
    std::vector<FlowEvent> evs_;  // raw log (diagnostic/audit)
    std::vector<Ev> idx_;         // prefix index (queries)
};

// ---- realized volatility from an as-of mid series ----

class MidSeries {
public:
    void observe(i64 ts_ns, double mid);
    // Sample stdev of log-returns of mids in (as_of-window, as_of]; 0 if <2 samples.
    // O(log n) via prefix sums of r and r^2.
    double realized_vol(i64 as_of_ns, i64 window_ns) const;
    double last_mid() const { return mids_.empty() ? 0.0 : mids_.back().mid; }
    void clear();

private:
    struct Pt {
        i64 ts = 0;
        double mid = 0.0;
        double pr = 0.0;   // prefix sum of log-returns up to this point
        double pr2 = 0.0;  // prefix sum of squared log-returns
    };
    std::vector<Pt> mids_;
};

// ---- assembled feature vector ----

struct FeatureVector {
    std::string version = kFeatureVersion;
    // Fixed-order named values (order is part of the version contract).
    std::vector<std::pair<std::string, double>> values;
    double get(const std::string& name) const;  // 0.0 + missing flag via has()
    bool has(const std::string& name) const;
};

// Assemble the full v1 vector from as-of state. `as_of_ns` gates every
// windowed query; ladder/ofi/flow/mids must contain only data <= as_of_ns
// (the caller enforces this; leakage tests verify the property end-to-end).
FeatureVector compute_v1(const LadderView& ladder, const OfiAccumulator& ofi,
                         const FlowWindows& flow, const MidSeries& mids, i64 as_of_ns);

// Ordered feature names of feat_v1 (model column contract).
std::vector<std::string> feature_names_v1();

}  // namespace features
}  // namespace arbiter