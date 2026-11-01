#pragma once
// Optimized order book, the performance counterpart of RefBook.
//
// Design (benchmarked and differentially verified against RefBook):
//   * contiguous price ladder: one std::vector<Level> per side, indexed by
//     tick offset within a bounded price span [min_tick, max_tick]
//   * slab-allocated order nodes (std::vector pool + intrusive free list),
//     index-linked FIFO per level, no per-order heap allocation
//   * cached best bid/ask, advanced by short scans only when the best level
//     empties (amortized cheap under realistic churn)
//
// Deliberate tradeoffs (documented in docs/PERFORMANCE.md):
//   * bounded price span: orders outside [min_tick, max_tick] are rejected
//     with OutOfRange (the risk layer configures the span)
//   * emptying the best level triggers an O(span) worst-case scan; amortized
//     cost is low under realistic arrival patterns, and that cost is measured

#include "arbiter/book_common.hpp"
#include "arbiter/flatmap.hpp"

#include <unordered_map>
#include <vector>

namespace arbiter {

class OptBook {
public:
    static constexpr size_t kMaxLadderTicks = 1'000'000;

    struct Options {
        Price min_tick = 1;
        Price max_tick = 1'000'000;
        size_t reserve_orders = 1u << 12;
        // Active-order cap (aligned with RefBook::Options::max_orders): a
        // resting entry that would NOT fully execute is rejected with
        // Capacity BEFORE matching (zero fills, zero mutation) when the book
        // already holds max_orders live orders. A fully executable order
        // needs no resting slot and is admitted even at capacity.
        u32 max_orders = 2'000'000;
    };

    explicit OptBook(const Options& opt);

    Status add_order(const Order& o, std::vector<Fill>& fills, AddResult* out = nullptr);
    Status cancel(u64 order_id);
    Status replace(u64 order_id, const Order& replacement, std::vector<Fill>& fills, AddResult* out = nullptr);
    // Directly rest an order with its exact leaves/qty (no matching). Used
    // only by snapshot restore/replay reconstruction: NOT a user entry point.
    Status rest_order(const Order& o);

    bool has(u64 order_id) const noexcept { return index_.contains(order_id); }
    const Order* find(u64 order_id) const noexcept;

    Price best_bid() const noexcept { return best_bid_ < 0 ? 0 : best_bid_; }
    Price best_ask() const noexcept { return best_ask_ < 0 ? 0 : best_ask_; }
    Qty total_bid_qty() const noexcept;
    Qty total_ask_qty() const noexcept;
    size_t order_count() const noexcept { return index_.size(); }
    size_t level_count() const noexcept;
    u64 next_book_seq() const noexcept { return next_book_seq_; }
    void set_next_book_seq(u64 v) noexcept { next_book_seq_ = v; }
    // Account ownership of every resting order (snapshot restore needs it:
    // accounts do not affect matching, so they are absent from the canonical
    // ladder, but they DO route fill notifications and cancel confirms).
    std::vector<std::pair<u64, u32>> order_accounts() const;
    void set_order_account(u64 id, u32 account) noexcept;
    size_t pool_capacity() const noexcept { return pool_.size(); }

    // Queue-model primitives (used by the strategy's local view):
    // qty resting at an exact price; consume up to `qty` FIFO from that level.
    Qty qty_at(Side s, Price p) const noexcept;
    Qty consume_at(Side s, Price p, Qty qty) noexcept;

    LadderView to_ladder() const;
    void hash_into(Hasher& h) const { canonical_book_hash(to_ladder(), h); }
    u64 state_hash() const { return canonical_book_hash(to_ladder()); }

private:
    static constexpr u32 kNone = 0xFFFFFFFFu;
    static constexpr Price kNoBid = -1;     // prices are >= 1, so -1 = empty side
    static constexpr Price kNoAsk = -1;

    // Preflight for one resting entry: guarantees the commit phase can obtain
    // a pool node and an index slot WITHOUT allocating. Grows the pool / index
    // HERE, before any book mutation, so an allocation failure (which may
    // throw) can never leave a half-matched book. Returns Capacity when the
    // 24-bit node index space is exhausted (a normal, side-effect-free
    // failure); throws bad_alloc only from the pre-mutation growth itself.
    Status prepare_rest_resources(u64 order_id);

    struct Node {
        Order ord;
        u32 prev;
        u32 next;
    };
    struct Level {
        Qty total = 0;
        u32 head = kNone;
        u32 tail = kNone;
        u32 count = 0;
    };
    struct Loc {
        Side side;
        u32 off;   // level offset within side array
        u32 node;  // pool index
    };

    Options opt_;
    std::vector<Level> bids_;  // off 0 = highest price (max_tick)
    std::vector<Level> asks_;  // off 0 = lowest price (min_tick)
    std::vector<Node> pool_;
    u32 free_head_ = kNone;
    // order id -> packed Loc{off | node<<32 | side<<56}; preallocated,
    // allocation-free in steady state (see FlatIdMap)
    FlatIdMap index_;
    Price best_bid_ = kNoBid;
    Price best_ask_ = kNoAsk;
    u64 next_book_seq_ = 1;

    Level& level_for(Side s, u32 off) noexcept { return is_buy(s) ? bids_[off] : asks_[off]; }
    const Level& level_for(Side s, u32 off) const noexcept { return is_buy(s) ? bids_[off] : asks_[off]; }
    u32 offset_for(Side s, Price p) const noexcept {
        return is_buy(s) ? static_cast<u32>(opt_.max_tick - p) : static_cast<u32>(p - opt_.min_tick);
    }
    Price price_for(Side s, u32 off) const noexcept {
        return is_buy(s) ? opt_.max_tick - static_cast<Price>(off) : opt_.min_tick + static_cast<Price>(off);
    }

    static u64 pack_loc(Side s, u32 off, u32 node) noexcept {
        return static_cast<u64>(off) | (static_cast<u64>(node) << 32) |
               (static_cast<u64>(is_buy(s) ? 1 : 0) << 56);
    }
    static Side loc_side(u64 v) noexcept { return (v >> 56) ? Side::Buy : Side::Sell; }
    static u32 loc_off(u64 v) noexcept { return static_cast<u32>(v & 0xFFFFFFFFu); }
    static u32 loc_node(u64 v) noexcept { return static_cast<u32>((v >> 32) & 0xFFFFFFu); }

    // Takes a node from the free list. GENUINELY non-allocating: the caller
    // must have run prepare_rest_resources() first (free list non-empty), so
    // the noexcept is honest — the commit phase cannot throw.
    u32 alloc_node(const Order& o) noexcept;
    void free_node(u32 idx) noexcept;
    void attach(Side s, u32 off, u32 node_idx) noexcept;
    void detach(Side s, u32 off, u32 node_idx) noexcept;
    void recompute_best_after_empty(Side s, u32 emptied_off) noexcept;
#ifndef NDEBUG
    void assert_best_cache(Side side, u32 from_off) const noexcept;
#else
    void assert_best_cache(Side, u32) const noexcept {}
#endif
    Qty match_against(Order& o, std::vector<Fill>& fills);
    Qty available_within(Side taker_side, Price limit_price) const noexcept;
    // Infallible after prepare_rest_resources(): rest cannot fail, so a
    // failed add_order can never carry fills (see add_order).
    void rest(Order o);
    Status validate_incoming(const Order& o) const noexcept;
};

}  // namespace arbiter
