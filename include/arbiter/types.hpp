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
#include <limits>
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

// Saturating arithmetic is used at accounting/reporting boundaries where a
// malformed or adversarially large input must not invoke signed-overflow UB.
// Normal validated world values stay well inside these limits; saturation is
// the explicit failure behavior for direct API callers and corrupted state.
inline constexpr i64 saturating_add_i64(i64 a, i64 b) noexcept {
    if (b == std::numeric_limits<i64>::min())
        return a > 0 ? std::numeric_limits<i64>::min() + a : std::numeric_limits<i64>::min();
    if (b > 0 && a > std::numeric_limits<i64>::max() - b)
        return std::numeric_limits<i64>::max();
    if (b < 0 && a < std::numeric_limits<i64>::min() - b)
        return std::numeric_limits<i64>::min();
    return a + b;
}

inline constexpr i64 saturating_sub_i64(i64 a, i64 b) noexcept {
    if (b == std::numeric_limits<i64>::min())
        return a >= 0 ? std::numeric_limits<i64>::max()
                      : std::numeric_limits<i64>::max() + (a + 1);
    return saturating_add_i64(a, -b);
}

inline constexpr i64 saturating_mul_i64(i64 a, i64 b) noexcept {
    if (a == 0 || b == 0) return 0;
    constexpr i64 kMin = std::numeric_limits<i64>::min();
    constexpr i64 kMax = std::numeric_limits<i64>::max();
    if (a == -1) return b == kMin ? kMax : -b;
    if (b == -1) return a == kMin ? kMax : -a;
    if (a > 0) {
        if (b > 0) return a > kMax / b ? kMax : a * b;
        return b < kMin / a ? kMin : a * b;
    }
    if (b > 0) return a < kMin / b ? kMin : a * b;
    return a < kMax / b ? kMax : a * b;
}

inline constexpr u64 saturating_add_u64(u64 a, u64 b) noexcept {
    return std::numeric_limits<u64>::max() - a < b ? std::numeric_limits<u64>::max()
                                                    : a + b;
}

inline constexpr i64 signed_qty_i64(Side side, Qty qty) noexcept {
    const Qty bounded = qty > static_cast<Qty>(std::numeric_limits<i64>::max())
                            ? static_cast<Qty>(std::numeric_limits<i64>::max())
                            : qty;
    const i64 q = static_cast<i64>(bounded);
    return is_buy(side) ? q : -q;
}

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
