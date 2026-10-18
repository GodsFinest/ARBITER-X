#pragma once
// Reference order book, deliberately simple, auditable, correct.
// Performance is explicitly NOT a goal here; this is the ground-truth model
// that the optimized book is differentially verified against.

#include "arbiter/book_common.hpp"

#include <list>
#include <map>
#include <unordered_map>

namespace arbiter {

class RefBook {
public:
    struct Options {
        u32 max_orders = 2'000'000;  // safety cap against runaway property tests
    };

    explicit RefBook(const Options& opt) : opt_(opt) {}

    // Process an incoming order. Fills are appended to `fills` (caller-owned,
    // may be reused). Time priority: resting orders execute strictly FIFO
    // within a price level; the incoming order receives seq on entry.
    // FOK is all-or-nothing: on InsufficientLiquidity the book is untouched.
    Status add_order(const Order& o, std::vector<Fill>& fills, AddResult* out = nullptr);
    Status cancel(u64 order_id);
    // Replace = cancel + add with the replacement's own id (loses time
    // priority, documented semantic; may immediately cross and execute).
    Status replace(u64 order_id, const Order& replacement, std::vector<Fill>& fills, AddResult* out = nullptr);
    // Directly rest an order with its exact leaves/qty (no matching). Used
    // only by snapshot restore/replay reconstruction: NOT a user entry point.
    Status rest_order(const Order& o);

    bool has(u64 order_id) const noexcept { return index_.count(order_id) != 0; }
    const Order* find(u64 order_id) const noexcept;

    Price best_bid() const noexcept { return bids_.empty() ? 0 : bids_.begin()->first; }
    Price best_ask() const noexcept { return asks_.empty() ? 0 : asks_.begin()->first; }
    Qty total_bid_qty() const noexcept;
    Qty total_ask_qty() const noexcept;
    size_t order_count() const noexcept { return index_.size(); }
    size_t level_count() const noexcept { return bids_.size() + asks_.size(); }
    u64 next_book_seq() const noexcept { return next_book_seq_; }

    LadderView to_ladder() const;
    void hash_into(Hasher& h) const { canonical_book_hash(to_ladder(), h); }
    u64 state_hash() const { return canonical_book_hash(to_ladder()); }

private:
    struct Level {
        Qty total = 0;
        std::list<Order> fifo;
    };
    struct Loc {
        Side side;
        Price px;
        std::list<Order>::iterator it;
    };

    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level, std::less<Price>> asks_;
    std::unordered_map<u64, Loc> index_;
    u64 next_book_seq_ = 1;
    Options opt_;

    // Core matcher: consumes resting liquidity for `o` (which has leaves>0),
    // appends fills, decrements o.leaves. Returns qty matched.
    Qty match_against(Order& o, std::vector<Fill>& fills);
    // Max qty obtainable within the taker's price constraint (for FOK).
    Qty available_within(Side taker_side, Price limit_price) const noexcept;
    void rest(Order o);
};

}  // namespace arbiter