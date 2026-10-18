#pragma once
// ARBITER-X core domain types.
//
// Determinism rule: the deterministic core contains NO floating-point state.
// Prices are int64 tick counts, quantities uint64, and money (PnL/notional) is
// int64 in units of ticks*qty. IEEE double appears only as a *draw value* for
// stochastic decisions (delay jitter, loss coins) and in wall-clock benchmark
// statistics, never in comparable state. See docs/DETERMINISM.md.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arbiter {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using LogicalNs = i64;  // deterministic logical time, ns since experiment t0
using Price = i64;      // price in integer ticks (>= 1)
using Qty = u64;        // quantity in lots

enum class Side : u8 { Buy = 0, Sell = 1 };
enum class OrderType : u8 { Limit = 0, Market = 1 };
enum class Tif : u8 { GTC = 0, IOC = 1, FOK = 2, GTD = 3 };
enum class Liquidity : u8 { Maker = 0, Taker = 1 };

constexpr const char* to_cstr(Side s) noexcept { return s == Side::Buy ? "BUY" : "SELL"; }
constexpr const char* to_cstr(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::Market: return "MARKET";
    }
    return "?";
}
constexpr const char* to_cstr(Tif t) noexcept {
    switch (t) {
        case Tif::GTC: return "GTC";
        case Tif::IOC: return "IOC";
        case Tif::FOK: return "FOK";
        case Tif::GTD: return "GTD";
    }
    return "?";
}
constexpr const char* to_cstr(Liquidity l) noexcept { return l == Liquidity::Maker ? "MAKER" : "TAKER"; }

constexpr bool is_buy(Side s) noexcept { return s == Side::Buy; }
// Would an aggressive order at `aggressor_px` trade with a resting order at `resting_px`?
constexpr bool crosses(Side aggressor, Price aggressor_px, Price resting_px) noexcept {
    return is_buy(aggressor) ? (aggressor_px >= resting_px) : (aggressor_px <= resting_px);
}

// Hard domain bounds enforced at the entry gateway (risk layer) so that
// price*qty fits comfortably in int64 (max notional 1e15 << 9.2e18).
struct DomainBounds {
    static constexpr Price kMinPrice = 1;
    static constexpr Price kMaxPrice = 1'000'000'000;  // 1e9 ticks
    static constexpr Qty kMaxQty = 1'000'000;          // 1e6 lots
    static constexpr u64 kMaxOrderId = 1'000'000'000'000ull;
};

}  // namespace arbiter