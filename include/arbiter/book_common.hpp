#pragma once
// Shared book abstractions: canonical ladder view, canonical hashing, and the
// invariant checker. Both RefBook and OptBook must produce identical ladder
// views and identical canonical hashes for identical histories, this is the
// backbone of differential verification (docs/VERIFICATION.md).

#include "arbiter/codec.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/order.hpp"
#include "arbiter/types.hpp"

#include <vector>

namespace arbiter {

struct LadderOrder {
    u64 id;
    Qty leaves;
    Qty qty;  // original quantity (for invariants: leaves <= qty)
};

struct LadderLevel {
    Price price;
    Qty total_qty;
    std::vector<LadderOrder> orders;  // FIFO (time priority) order
};

struct LadderView {
    std::vector<LadderLevel> bids;  // best (highest price) first
    std::vector<LadderLevel> asks;  // best (lowest price) first

    Price best_bid() const noexcept { return bids.empty() ? 0 : bids.front().price; }
    Price best_ask() const noexcept { return asks.empty() ? 0 : asks.front().price; }
};

// Canonical, implementation-independent hash of resting book state.
// Only behaviorally relevant fields participate: level prices, level totals,
// and the ordered (id, leaves, qty) per level. Metadata that cannot affect
// future behavior (arrival timestamps, priority seqs) is excluded so two
// implementations with identical behavior always hash identically.
void canonical_book_hash(const LadderView& v, Hasher& h);
u64 canonical_book_hash(const LadderView& v);

// Structural invariants checked over the canonical view (test-time):
//   * bid prices strictly descending, ask prices strictly ascending
//   * level totals equal sum of order leaves
//   * every level non-empty, every order has leaves >= 1 and leaves <= qty
//   * order ids globally unique
//   * no crossed book: best_bid < best_ask whenever both sides exist
Status check_book_invariants(const LadderView& v);

// Fill/execution conservation counters, updated by the engine harness in tests:
// total matched qty must equal sum of taker fills and sum of maker deltas.
struct MatchLedger {
    u64 qty_matched = 0;         // sum of fill qty
    u64 taker_filled_delta = 0;  // sum of taker-side executed qty
    u64 maker_filled_delta = 0;  // sum of maker-side executed qty
    u64 trades = 0;
    void add(const Fill& f) noexcept {
        qty_matched += f.qty;
        maker_filled_delta += f.qty;  // maker leaves decreased by f.qty
        ++trades;
    }
    void reset() noexcept { *this = MatchLedger{}; }
};

// Canonical ladder codec (shared by snapshots and wire snapshot frames).
void encode_ladder(ByteWriter& w, const LadderView& v);
// Returns false on structural violation (bad counts, truncation).
bool decode_ladder(ByteReader& r, LadderView& v);

// Shared entry validation for all books (domain bounds, duplicate ids,
// market-order price rule). `duplicate_id` must be supplied by the caller
// from its own index state.
Status validate_order_entry(const Order& o, bool duplicate_id);

}  // namespace arbiter