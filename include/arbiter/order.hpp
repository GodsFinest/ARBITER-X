#pragma once
// Order and fill domain records.

#include "arbiter/types.hpp"

namespace arbiter {

struct Order {
    u64 id = 0;            // globally unique per experiment
    u32 account = 0;       // strategy / bot account id
    Side side{};
    OrderType type{};
    Tif tif{};
    Price price = 0;       // 0 for market orders
    Qty qty = 0;           // original quantity
    Qty leaves = 0;        // remaining quantity
    LogicalNs arrival = 0; // logical ns when the order entered the book
    u64 book_seq = 0;      // exchange-assigned time-priority sequence
    LogicalNs expiry = 0;  // GTD: absolute logical expiry ns; 0 = none

    Qty filled() const noexcept { return qty - leaves; }
    bool resting() const noexcept { return type == OrderType::Limit; }
};

struct Fill {
    u64 taker_order_id = 0;
    u64 maker_order_id = 0;
    u32 maker_account = 0;
    u32 taker_account = 0;
    Price price = 0;
    Qty qty = 0;
    LogicalNs t = 0;  // logical time of the trade
    Liquidity maker_liq = Liquidity::Maker;
    // Sides of the two participating orders (filled in by the World when
    // routing notifications; books leave them default).
    Side taker_side = Side::Buy;
    Side maker_side = Side::Sell;
};

// Outcome of an aggressive order entering the book.
struct AddResult {
    Qty filled = 0;    // total executed against resting liquidity
    Qty canceled = 0;  // taker remainder that did not rest (IOC/market/FOK leftovers)
};

}  // namespace arbiter