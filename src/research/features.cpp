// Feature engine feat_v1: book shape, microprice, OFI, flow, volatility.
#include "arbiter/features.hpp"

#include <algorithm>
#include <cmath>

namespace arbiter {
namespace features {

double mid_price(const LadderView& b) {
    if (b.bids.empty() || b.asks.empty()) return 0.0;
    return (static_cast<double>(b.bids.front().price) +
            static_cast<double>(b.asks.front().price)) *
           0.5;
}

double spread_ticks(const LadderView& b) {
    if (b.bids.empty() || b.asks.empty()) return -1.0;
    return static_cast<double>(b.asks.front().price - b.bids.front().price);
}

double relative_spread(const LadderView& b) {
    const double m = mid_price(b);
    if (m <= 0.0) return -1.0;
    return spread_ticks(b) / m;
}

Qty level_bid_qty(const LadderView& b, int levels) {
    Qty q = 0;
    for (size_t i = 0; i < b.bids.size() && i < static_cast<size_t>(levels); ++i)
        q += b.bids[i].total_qty;
    return q;
}

Qty level_ask_qty(const LadderView& b, int levels) {
    Qty q = 0;
    for (size_t i = 0; i < b.asks.size() && i < static_cast<size_t>(levels); ++i)
        q += b.asks[i].total_qty;
    return q;
}

double depth_imbalance(const LadderView& b, int levels) {
    const double bb = static_cast<double>(level_bid_qty(b, levels));
    const double ba = static_cast<double>(level_ask_qty(b, levels));
    const double denom = bb + ba;
    if (denom <= 0.0) return 0.0;  // explicit empty-book convention
    return (bb - ba) / denom;
}

double microprice(const LadderView& b) {
    if (b.bids.empty() || b.asks.empty()) return 0.0;
    const double bid = static_cast<double>(b.bids.front().price);
    const double ask = static_cast<double>(b.asks.front().price);
    const double bq = static_cast<double>(b.bids.front().total_qty);
    const double aq = static_cast<double>(b.asks.front().total_qty);
    const double denom = bq + aq;
    if (denom <= 0.0) return (bid + ask) * 0.5;
    return (ask * bq + bid * aq) / denom;
}

double microprice_deviation_ticks(const LadderView& b) {
    const double m = mid_price(b);
    if (m <= 0.0) return 0.0;
    return microprice(b) - m;
}

// ---- OFI ----

void OfiAccumulator::observe(i64 ts_ns, const BookDelta& d) {
    if (!have_prev_) {
        prev_ = d;
        have_prev_ = true;
        ticks_.push_back({ts_ns, 0, total_});
        return;
    }
    i64 e = 0;
    // Cont et al. (2014) top-level OFI event contribution.
    if (d.bid_px > prev_.bid_px)
        e += static_cast<i64>(d.bid_qty);
    else if (d.bid_px == prev_.bid_px)
        e += static_cast<i64>(d.bid_qty) - static_cast<i64>(prev_.bid_qty);
    else
        e -= static_cast<i64>(prev_.bid_qty);
    if (d.ask_px < prev_.ask_px)
        e -= static_cast<i64>(d.ask_qty);
    else if (d.ask_px == prev_.ask_px)
        e -= static_cast<i64>(d.ask_qty) - static_cast<i64>(prev_.ask_qty);
    else
        e += static_cast<i64>(prev_.ask_qty);
    // Empty-side guards: a 0 price means the side is empty; treat moves
    // to/from empty as full depletion/replenishment of the known depth.
    total_ += e;
    ticks_.push_back({ts_ns, e, total_});
    prev_ = d;
}

double OfiAccumulator::windowed(i64 as_of_ns, i64 window_ns) const {
    if (ticks_.empty()) return 0.0;
    const i64 lo = as_of_ns - window_ns;
    // prefix(hi) - prefix(lo): last tick with ts <= bound
    auto le = [&](i64 bound) -> i64 {
        size_t l = 0, r = ticks_.size();
        i64 acc = 0;
        while (l < r) {
            const size_t m = l + (r - l) / 2;
            if (ticks_[m].ts <= bound) {
                acc = ticks_[m].prefix;
                l = m + 1;
            } else {
                r = m;
            }
        }
        return acc;
    };
    return static_cast<double>(le(as_of_ns) - le(lo));
}

void OfiAccumulator::clear() {
    ticks_.clear();
    have_prev_ = false;
    total_ = 0;
}

// ---- flow ----

void FlowWindows::add_trade(i64 ts_ns, Side aggressor, Qty qty) {
    evs_.push_back({ts_ns, is_buy(aggressor) ? static_cast<i64>(qty) : -static_cast<i64>(qty), false});
    Ev e;
    e.ts = ts_ns;
    const double q = static_cast<double>(qty);
    const double ds = is_buy(aggressor) ? q : -q;
    const double db = is_buy(aggressor) ? q : 0.0;
    const double dss = is_buy(aggressor) ? 0.0 : q;
    const Ev* p = idx_.empty() ? nullptr : &idx_.back();
    e.psigned_trade = (p ? p->psigned_trade : 0.0) + ds;
    e.pbuy = (p ? p->pbuy : 0.0) + db;
    e.psell = (p ? p->psell : 0.0) + dss;
    e.pcancel_buy = p ? p->pcancel_buy : 0.0;
    e.pcancel_sell = p ? p->pcancel_sell : 0.0;
    idx_.push_back(e);
}

void FlowWindows::add_cancel(i64 ts_ns, Side side, Qty qty) {
    evs_.push_back({ts_ns, is_buy(side) ? static_cast<i64>(qty) : -static_cast<i64>(qty), true});
    Ev e;
    e.ts = ts_ns;
    const double q = static_cast<double>(qty);
    const double dcb = is_buy(side) ? q : 0.0;
    const double dcs = is_buy(side) ? 0.0 : q;
    e.psigned_trade = idx_.empty() ? 0.0 : idx_.back().psigned_trade;
    e.pbuy = idx_.empty() ? 0.0 : idx_.back().pbuy;
    e.psell = idx_.empty() ? 0.0 : idx_.back().psell;
    e.pcancel_buy = (idx_.empty() ? 0.0 : idx_.back().pcancel_buy) + dcb;
    e.pcancel_sell = (idx_.empty() ? 0.0 : idx_.back().pcancel_sell) + dcs;
    idx_.push_back(e);
}

std::pair<size_t, size_t> FlowWindows::span(i64 lo, i64 hi) const {
    // [first index with ts > lo, first index with ts > hi)
    size_t a = 0, b = idx_.size();
    size_t l = 0, r = idx_.size();
    while (l < r) {
        const size_t m = l + (r - l) / 2;
        if (idx_[m].ts <= lo) l = m + 1;
        else r = m;
    }
    a = l;
    l = 0;
    r = idx_.size();
    while (l < r) {
        const size_t m = l + (r - l) / 2;
        if (idx_[m].ts <= hi) l = m + 1;
        else r = m;
    }
    b = l;
    return {a, b};
}

double FlowWindows::range(const std::vector<Ev>& v, size_t lo, size_t hi, double Ev::*field) {
    if (hi <= lo || hi > v.size()) return 0.0;
    const double h = v[hi - 1].*field;
    const double l = lo > 0 ? v[lo - 1].*field : 0.0;
    return h - l;
}

double FlowWindows::signed_volume(i64 as_of_ns, i64 window_ns) const {
    const auto [a, b] = span(as_of_ns - window_ns, as_of_ns);
    return range(idx_, a, b, &Ev::psigned_trade);
}

double FlowWindows::buy_volume(i64 as_of_ns, i64 window_ns) const {
    const auto [a, b] = span(as_of_ns - window_ns, as_of_ns);
    return range(idx_, a, b, &Ev::pbuy);
}

double FlowWindows::sell_volume(i64 as_of_ns, i64 window_ns) const {
    const auto [a, b] = span(as_of_ns - window_ns, as_of_ns);
    return range(idx_, a, b, &Ev::psell);
}

double FlowWindows::trade_imbalance(i64 as_of_ns, i64 window_ns) const {
    const double b = buy_volume(as_of_ns, window_ns);
    const double s = sell_volume(as_of_ns, window_ns);
    if (b + s <= 0.0) return 0.0;
    return (b - s) / (b + s);
}

double FlowWindows::cancel_imbalance(i64 as_of_ns, i64 window_ns) const {
    const auto [a, b] = span(as_of_ns - window_ns, as_of_ns);
    const double cb = range(idx_, a, b, &Ev::pcancel_buy);
    const double cs = range(idx_, a, b, &Ev::pcancel_sell);
    if (cb + cs <= 0.0) return 0.0;
    return (cb - cs) / (cb + cs);
}

void FlowWindows::clear() {
    evs_.clear();
    idx_.clear();
}

// ---- mids / vol ----

void MidSeries::observe(i64 ts_ns, double mid) {
    if (mid <= 0.0) return;
    Pt p;
    p.ts = ts_ns;
    p.mid = mid;
    if (!mids_.empty() && mids_.back().mid > 0.0) {
        const double r = std::log(mid / mids_.back().mid);
        p.pr = mids_.back().pr + r;
        p.pr2 = mids_.back().pr2 + r * r;
    } else if (!mids_.empty()) {
        p.pr = mids_.back().pr;
        p.pr2 = mids_.back().pr2;
    }
    mids_.push_back(p);
}

double MidSeries::realized_vol(i64 as_of_ns, i64 window_ns) const {
    if (mids_.size() < 2) return 0.0;
    const i64 lo = as_of_ns - window_ns;
    // first index with ts > lo
    size_t a = 0;
    {
        size_t l = 0, r = mids_.size();
        while (l < r) {
            const size_t m = l + (r - l) / 2;
            if (mids_[m].ts <= lo) l = m + 1;
            else r = m;
        }
        a = l;
    }
    // last index with ts <= as_of
    size_t b = 0;
    {
        size_t l = 0, r = mids_.size();
        while (l < r) {
            const size_t m = l + (r - l) / 2;
            if (mids_[m].ts <= as_of_ns) l = m + 1;
            else r = m;
        }
        if (l == 0) return 0.0;
        b = l - 1;
    }
    if (b <= a) return 0.0;
    // returns strictly inside (lo, as_of]: pairs (a..b); the return AT a uses
    // the pre-window mid, excluded (as-of purity: it needs out-of-window data)
    const size_t n = b - a;
    if (n < 2) return 0.0;
    const double sum = mids_[b].pr - mids_[a].pr;
    const double sum2 = mids_[b].pr2 - mids_[a].pr2;
    const double nn = static_cast<double>(n);
    const double mean = sum / nn;
    double var = sum2 / nn - mean * mean;
    if (var < 0.0) var = 0.0;
    return std::sqrt(var);
}

// ---- assembly ----

std::vector<std::string> feature_names_v1() {
    std::vector<std::string> n = {"mid", "spread", "rel_spread", "micro_dev", "ofi_10ms",
                                  "ofi_100ms", "signed_vol_10ms", "signed_vol_100ms",
                                  "trade_imb_100ms", "cancel_imb_100ms", "realvol_100ms",
                                  "realvol_1s", "depth_tot"};
    for (int k : kImbalanceDepths) n.push_back("imb_" + std::to_string(k));
    return n;
}

FeatureVector compute_v1(const LadderView& ladder, const OfiAccumulator& ofi,
                         const FlowWindows& flow, const MidSeries& mids, i64 as_of_ns) {
    FeatureVector f;
    const double mid = mid_price(ladder);
    f.values.push_back({"mid", mid});
    f.values.push_back({"spread", spread_ticks(ladder)});
    f.values.push_back({"rel_spread", relative_spread(ladder)});
    f.values.push_back({"micro_dev", microprice_deviation_ticks(ladder)});
    f.values.push_back({"ofi_10ms", ofi.windowed(as_of_ns, 10'000'000)});
    f.values.push_back({"ofi_100ms", ofi.windowed(as_of_ns, 100'000'000)});
    f.values.push_back({"signed_vol_10ms", flow.signed_volume(as_of_ns, 10'000'000)});
    f.values.push_back({"signed_vol_100ms", flow.signed_volume(as_of_ns, 100'000'000)});
    f.values.push_back({"trade_imb_100ms", flow.trade_imbalance(as_of_ns, 100'000'000)});
    f.values.push_back({"cancel_imb_100ms", flow.cancel_imbalance(as_of_ns, 100'000'000)});
    f.values.push_back({"realvol_100ms", mids.realized_vol(as_of_ns, 100'000'000)});
    f.values.push_back({"realvol_1s", mids.realized_vol(as_of_ns, 1'000'000'000)});
    f.values.push_back({"depth_tot", static_cast<double>(level_bid_qty(ladder, 20) +
                                                          level_ask_qty(ladder, 20))});
    for (int k : kImbalanceDepths)
        f.values.push_back({"imb_" + std::to_string(k), depth_imbalance(ladder, k)});
    return f;
}

double FeatureVector::get(const std::string& name) const {
    for (const auto& [k, v] : values)
        if (k == name) return v;
    return 0.0;
}

bool FeatureVector::has(const std::string& name) const {
    for (const auto& [k, v] : values)
        if (k == name) return true;
    return false;
}

}  // namespace features
}  // namespace arbiter