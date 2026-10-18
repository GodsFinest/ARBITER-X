#pragma once
// Deterministic gateway-side pre-trade risk engine.
// Every reject reason is explicit and testable; rejects are journalled as
// RiskDecision events and echoed back to the strategy.

#include "arbiter/order.hpp"
#include "arbiter/types.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace arbiter {

enum RiskReason : u8 {
    kRiskOk = 0,
    kRiskKillSwitch = 1,
    kRiskPriceBounds = 2,
    kRiskMaxOrderQty = 3,
    kRiskMaxPosition = 4,
    kRiskMaxNotional = 5,
    kRiskRateLimit = 6,
    kRiskDuplicateId = 7,
};

const char* to_cstr(RiskReason r) noexcept;

struct RiskConfig {
    bool enabled = true;
    bool kill_switch = false;  // when set, every order is rejected (emergency halt)
    Price min_price = 1;
    Price max_price = 1'000'000;
    Qty max_order_qty = 100'000;
    i64 max_position = 1'000'000;      // abs(inventory + signed qty) cap
    i64 max_notional_per_order = 1'000'000'000'000;  // ticks*qty
    u32 max_msgs_per_window = 1'000'000;
    LogicalNs rate_window_ns = 1'000'000;
};

struct RiskState {
    i64 position = 0;                  // strategy inventory known to risk
    u64 accepted = 0;
    u64 rejected = 0;
    u64 rejects_by_reason[kRiskDuplicateId + 1] = {};
    // sliding-window rate limiter: timestamps of recent admits
    std::vector<LogicalNs> window;
};

struct RiskVerdict {
    bool accept = true;
    RiskReason reason = kRiskOk;
};

// `now` drives the rate limiter; `known_ids` lets the gateway reject
// duplicate order ids (in-flight or already used by this account).
//
// Per-account isolation: the engine keeps an INDEPENDENT RiskState per
// account (position, counters, rate window), one strategy's inventory never
// consumes another's position cap or message budget. `state()` returns the
// summed aggregate (reporting/hashing); `state_of(account)` exposes one
// account's state for tests.
class RiskEngine {
public:
    explicit RiskEngine(RiskConfig cfg) : cfg_(cfg) {}

    RiskVerdict check(const Order& o, LogicalNs now, bool duplicate_id) {
        RiskState& st = st_[o.account];
        RiskVerdict v;
        if (!cfg_.enabled) {
            ++st.accepted;
            return v;
        }
        auto reject = [&](RiskReason r) {
            v.accept = false;
            v.reason = r;
            ++st.rejected;
            ++st.rejects_by_reason[r];
            return v;
        };
        if (cfg_.kill_switch) return reject(kRiskKillSwitch);
        // MARKET orders carry price=0 by definition: the price-bounds and
        // notional checks are limit-order constraints (the execution price is
        // determined by the book). Quantity and position caps still apply.
        if (o.type == OrderType::Limit && (o.price < cfg_.min_price || o.price > cfg_.max_price))
            return reject(kRiskPriceBounds);
        if (o.qty == 0 || o.qty > cfg_.max_order_qty) return reject(kRiskMaxOrderQty);
        if (duplicate_id) return reject(kRiskDuplicateId);
        // projected position (this account only)
        const i64 signed_qty = is_buy(o.side) ? static_cast<i64>(o.qty) : -static_cast<i64>(o.qty);
        if (st.position + signed_qty > cfg_.max_position ||
            st.position + signed_qty < -cfg_.max_position)
            return reject(kRiskMaxPosition);
        if (o.type == OrderType::Limit) {
            const i64 notional = static_cast<i64>(o.qty) * o.price;
            if (notional > cfg_.max_notional_per_order) return reject(kRiskMaxNotional);
        }
        // rate limit: drop timestamps outside the window, then count
        // (per account: each strategy gets its own message budget)
        while (!st.window.empty() && st.window.front() <= now - cfg_.rate_window_ns)
            st.window.erase(st.window.begin());
        if (st.window.size() >= cfg_.max_msgs_per_window) return reject(kRiskRateLimit);
        st.window.push_back(now);
        ++st.accepted;
        return v;
    }

    void note_fill(i64 signed_qty, u32 account) { st_[account].position += signed_qty; }

    // Aggregate view (sums across accounts) for reporting and hashing.
    struct Aggregates {
        i64 position = 0;
        u64 accepted = 0;
        u64 rejected = 0;
        u64 rejects_by_reason[kRiskDuplicateId + 1] = {};
        std::vector<LogicalNs> window;
    };
    Aggregates state() const {
        Aggregates a;
        for (const auto& [acct, st] : st_) {
            (void)acct;
            a.position += st.position;
            a.accepted += st.accepted;
            a.rejected += st.rejected;
            for (size_t i = 0; i <= kRiskDuplicateId; ++i)
                a.rejects_by_reason[i] += st.rejects_by_reason[i];
            a.window.insert(a.window.end(), st.window.begin(), st.window.end());
        }
        std::sort(a.window.begin(), a.window.end());
        return a;
    }
    const RiskState& state_of(u32 account) const {
        auto it = st_.find(account);
        return it == st_.end() ? kEmptyState() : it->second;
    }
    // snapshot support: visit every account's state (sorted by account)
    template <typename F>
    void for_each_state(F&& f) const {
        for (const auto& [acct, st] : st_) f(acct, st);
    }
    // restore support: set one account's state outright
    void restore_state(u32 account, const RiskState& st) { st_[account] = st; }
    // restore support: drop all account states (fresh-world restore)
    void clear_states() { st_.clear(); }
    const RiskConfig& config() const noexcept { return cfg_; }

private:
    static const RiskState& kEmptyState() {
        static const RiskState e{};
        return e;
    }
    RiskConfig cfg_;
    std::map<u32, RiskState> st_;
};

}  // namespace arbiter