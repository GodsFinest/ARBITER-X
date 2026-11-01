#pragma once
// Deterministic gateway-side pre-trade risk engine.
// Every reject reason is explicit and testable; rejects are journalled as
// RiskDecision events and echoed back to the strategy.

#include "arbiter/order.hpp"
#include "arbiter/types.hpp"

#include <algorithm>
#include <limits>
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

// Working-order exposure reservation: the full quantity of every
// gateway-accepted, not-yet-terminal order is reserved against its account's
// position cap until the exchange resolves the order (fill reduces it, IOC/FOK
// remainder / cancel / expiry / book-admission reject release it). All
// reservation lifecycle transitions happen at EXCHANGE time, never on
// execution-report delivery, so a lost or corrupted report cannot leave
// stale reserved exposure.
struct RiskReservation {
    u32 account = 0;
    i64 signed_qty = 0;  // remaining reserved exposure: > 0 buy, < 0 sell
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
        auto reject = [&](RiskReason r) {
            v.accept = false;
            v.reason = r;
            ++st.rejected;
            ++st.rejects_by_reason[r];
            return v;
        };
        // Order identity is a protocol invariant, not an optional financial
        // control. It remains enforced when the risk limits are disabled.
        if (duplicate_id) return reject(kRiskDuplicateId);
        if (o.qty > static_cast<Qty>(std::numeric_limits<i64>::max()))
            return reject(kRiskMaxOrderQty);
        if (!cfg_.enabled) {
            ++st.accepted;
            return v;
        }
        if (cfg_.kill_switch) return reject(kRiskKillSwitch);
        // MARKET orders carry price=0 by definition: the price-bounds and
        // notional checks are limit-order constraints (the execution price is
        // determined by the book). Quantity and position caps still apply.
        if (o.type == OrderType::Limit && (o.price < cfg_.min_price || o.price > cfg_.max_price))
            return reject(kRiskPriceBounds);
        if (o.qty == 0 || o.qty > cfg_.max_order_qty) return reject(kRiskMaxOrderQty);
        // projected position (this account only): the position itself PLUS
        // the outstanding working-order exposure already reserved for this
        // account on the SAME side, PLUS this order's quantity. Without the
        // reservation term, two buys could each pass the check and jointly
        // breach the cap. Opposite-side reservations do NOT net (no
        // assumptions about fill sequencing).
        const i64 signed_qty = is_buy(o.side) ? static_cast<i64>(o.qty) : -static_cast<i64>(o.qty);
        const auto safe_add = [](i64 a, i64 b) {
            if (b > 0 && a > std::numeric_limits<i64>::max() - b)
                return std::numeric_limits<i64>::max();
            if (b < 0 &&
                (b == std::numeric_limits<i64>::min() ||
                 a < std::numeric_limits<i64>::min() - b))
                return std::numeric_limits<i64>::min();
            return a + b;
        };
        i64 outstanding_same_side = 0;
        for (const auto& [id, r] : reservations_) {
            (void)id;
            if (r.account == o.account &&
                ((r.signed_qty > 0) == is_buy(o.side))) {
                outstanding_same_side = safe_add(outstanding_same_side, r.signed_qty);
            }
        }
        const i64 projected = safe_add(safe_add(st.position, outstanding_same_side), signed_qty);
        if (projected > cfg_.max_position || projected < -cfg_.max_position)
            return reject(kRiskMaxPosition);
        if (o.type == OrderType::Limit) {
            if (o.price <= 0 || o.qty > static_cast<Qty>(std::numeric_limits<i64>::max() / o.price))
                return reject(kRiskMaxNotional);
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

    void note_fill(i64 signed_qty, u32 account) {
        i64& position = st_[account].position;
        if (signed_qty > 0 && position > std::numeric_limits<i64>::max() - signed_qty)
            position = std::numeric_limits<i64>::max();
        else if (signed_qty < 0 &&
                 (signed_qty == std::numeric_limits<i64>::min() ||
                  position < std::numeric_limits<i64>::min() - signed_qty))
            position = std::numeric_limits<i64>::min();
        else
            position += signed_qty;
    }

    // ---- working-order exposure reservations (exchange-time lifecycle) ----
    // Reserve the full order quantity at gateway acceptance.
    void reserve(u64 order_id, u32 account, i64 signed_qty) {
        reservations_[order_id] = RiskReservation{account, signed_qty};
    }
    // A fill consumes the reserved exposure at match time; the reservation
    // dies with its order (erased when the remaining reserved qty reaches 0).
    void reduce_reservation(u64 order_id, Qty qty) {
        auto it = reservations_.find(order_id);
        if (it == reservations_.end()) return;
        RiskReservation& r = it->second;
        if (r.signed_qty > 0)
            r.signed_qty -= static_cast<i64>(std::min<Qty>(qty, static_cast<Qty>(r.signed_qty)));
        else {
            const i64 magnitude = r.signed_qty == std::numeric_limits<i64>::min()
                                      ? std::numeric_limits<i64>::max()
                                      : -r.signed_qty;
            r.signed_qty += static_cast<i64>(std::min<Qty>(qty, static_cast<Qty>(magnitude)));
        }
        if (r.signed_qty == 0) reservations_.erase(it);
    }
    // The order reached an exchange terminal state without consuming the rest
    // of its reservation (IOC/FOK remainder, cancel, GTD expiry, book
    // admission failure): release whatever remains.
    void release_reservation(u64 order_id) { reservations_.erase(order_id); }
    bool has_reservation(u64 order_id) const {
        return reservations_.count(order_id) != 0;
    }
    // snapshot/hash/restore support (sorted map: deterministic order)
    const std::map<u64, RiskReservation>& reservations() const noexcept { return reservations_; }
    void restore_reservation(u64 order_id, const RiskReservation& r) { reservations_[order_id] = r; }
    void clear_reservations() { reservations_.clear(); }

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
        const auto add_i64 = [](i64 x, i64 y) {
            if (y > 0 && x > std::numeric_limits<i64>::max() - y)
                return std::numeric_limits<i64>::max();
            if (y < 0 &&
                (y == std::numeric_limits<i64>::min() ||
                 x < std::numeric_limits<i64>::min() - y))
                return std::numeric_limits<i64>::min();
            return x + y;
        };
        const auto add_u64 = [](u64 x, u64 y) {
            return std::numeric_limits<u64>::max() - x < y ? std::numeric_limits<u64>::max()
                                                            : x + y;
        };
        for (const auto& [acct, st] : st_) {
            (void)acct;
            a.position = add_i64(a.position, st.position);
            a.accepted = add_u64(a.accepted, st.accepted);
            a.rejected = add_u64(a.rejected, st.rejected);
            for (size_t i = 0; i <= kRiskDuplicateId; ++i)
                a.rejects_by_reason[i] = add_u64(a.rejects_by_reason[i],
                                                 st.rejects_by_reason[i]);
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
    std::map<u64, RiskReservation> reservations_;  // by order id (globally unique)
};

}  // namespace arbiter
