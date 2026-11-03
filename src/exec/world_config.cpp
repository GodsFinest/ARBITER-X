#include "arbiter/world.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/rng.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace arbiter {
u64 WorldConfig::hash() const {
    Hasher h;
    h.tag("ARBITERX/WORLDCFG/3");  // /3: now covers risk.enabled, mm.enabled, momentum.enabled
    h.i64v(algo_latency_ns);
    h.i64v(cancel_latency_ns);
    h.i64v(exchange_processing_ns);
    h.i64v(publish_latency_ns);
    auto net = [&](const NetProfile& np) {
        h.i64v(np.base_delay_ns);
        h.i64v(np.jitter_ns);
        h.u64v(std::bit_cast<u64>(np.loss_rate));
        h.u64v(std::bit_cast<u64>(np.dup_rate));
        h.u64v(np.reorder_window);
        h.i64v(np.reorder_slot_ns);
        h.u64v(np.burst_loss_len);
        h.u64v(std::bit_cast<u64>(np.burst_loss_rate));
        h.u64v(std::bit_cast<u64>(np.corrupt_rate));
    };
    net(md_channel);
    net(order_channel);
    net(exec_channel);
    h.i64v(sim.start_mid);
    h.u64v(sim.provider_qty);
    h.u64v(sim.taker_qty);
    h.u64v(std::bit_cast<u64>(sim.provider_arrival_per_ms));
    h.u64v(std::bit_cast<u64>(sim.taker_arrival_per_ms));
    h.u64v(std::bit_cast<u64>(sim.cancel_fraction));
    h.u64v(std::bit_cast<u64>(sim.taker_self_excitation));
    h.i64v(sim.touch_offset_ticks);
    h.i64v(sim.deep_offset_ticks);
    h.i64v(sim.regime_dwell_ns);
    h.u64v(std::bit_cast<u64>(sim.volatile_rate_multiplier));
    h.u64v(sim.volatile_qty_multiplier);
    h.u64v(sim.provider_accounts);
    h.i64v(sim.min_price);
    h.i64v(sim.max_price);
    h.i64v(horizon_ns);
    h.u64v(max_events);
    h.i64v(fee_per_lot);
    h.boolean(bots_observe_shadow);  // interference mode is behavior-relevant
    h.boolean(risk.enabled);
    h.i64v(risk.min_price);
    h.i64v(risk.max_price);
    h.u64v(risk.max_order_qty);
    h.i64v(risk.max_position);
    h.i64v(risk.max_notional_per_order);
    h.u64v(risk.max_msgs_per_window);
    h.i64v(risk.rate_window_ns);
    h.boolean(risk.kill_switch);
    h.i64v(book_min_price);
    h.i64v(book_max_price);
    h.u32v(mm.account);
    h.u64v(mm.quote_size);
    h.i64v(mm.half_spread_ticks);
    h.i64v(mm.skew_denominator);
    h.i64v(mm.skew_ticks);
    h.i64v(mm.max_inventory);
    h.i64v(mm.requote_interval_ns);
    h.boolean(mm.enabled);
    h.u32v(momentum.account);
    h.u64v(momentum.size);
    h.i64v(momentum.min_signal);
    h.u64v(momentum.window);
    h.i64v(momentum.cooldown_ns);
    h.i64v(momentum.max_inventory);
    h.u64v(momentum.id_base);  // id-space layout: behavior-relevant (gateway dedup)
    h.boolean(momentum.enabled);
    h.i64v(warmup_ns);
    return h.value();
}

u64 World::world_id() const noexcept {
    return Rng::mix(cfg_hash_ ^ Rng::mix(master_seed_));
}

// Full pre-construction configuration validation. Every rule here is a
// degenerate-or-unsatisfiable configuration that would otherwise surface as
// an allocation failure, a division by zero, silent double accounting, or an
// account collision. `cancel_latency_ns == -1` is the documented "follow
// algo_latency_ns" sentinel and stays exactly that.
std::string validate_world_config(const WorldConfig& c) {
    auto bad = [](const std::string& what) { return what; };
    auto finite01 = [](double v) { return std::isfinite(v) && v >= 0.0 && v <= 1.0; };
    constexpr Price kMaxWorldBookPrice = 100'000;
    constexpr i64 kMaxFeePerLot = std::numeric_limits<i64>::max() /
                                  static_cast<i64>(DomainBounds::kMaxQty);
    constexpr i64 kMaxPosition = std::numeric_limits<i64>::max() -
                                  static_cast<i64>(DomainBounds::kMaxQty);
    constexpr i64 kMaxNotional = DomainBounds::kMaxPrice *
                                 static_cast<i64>(DomainBounds::kMaxQty);
    auto net = [&](const char* name, const NetProfile& np) -> std::string {
        if (np.base_delay_ns < 0) return bad(std::string(name) + ".base_delay_ns < 0");
        if (np.jitter_ns < 0) return bad(std::string(name) + ".jitter_ns < 0");
        if (np.reorder_slot_ns < 0) return bad(std::string(name) + ".reorder_slot_ns < 0");
        if (!finite01(np.loss_rate)) return bad(std::string(name) + ".loss_rate outside [0,1]");
        if (!finite01(np.dup_rate)) return bad(std::string(name) + ".dup_rate outside [0,1]");
        if (!finite01(np.burst_loss_rate))
            return bad(std::string(name) + ".burst_loss_rate outside [0,1]");
        if (!finite01(np.corrupt_rate)) return bad(std::string(name) + ".corrupt_rate outside [0,1]");
        constexpr i64 kMax = std::numeric_limits<i64>::max();
        if (np.base_delay_ns > kMax - np.jitter_ns)
            return bad(std::string(name) + ".base_delay_ns + jitter_ns overflows");
        const i64 primary = np.base_delay_ns + np.jitter_ns;
        if (np.base_delay_ns > kMax - primary)
            return bad(std::string(name) + ".duplicate delay overflows");
        if (np.reorder_window > 1 &&
            np.reorder_slot_ns > kMax / static_cast<i64>(np.reorder_window - 1))
            return bad(std::string(name) + ".reorder delay overflows");
        const i64 reorder_max =
            np.reorder_window > 1
                ? np.reorder_slot_ns * static_cast<i64>(np.reorder_window - 1)
                : 0;
        if (primary > kMax - reorder_max || primary + np.base_delay_ns > kMax - reorder_max)
            return bad(std::string(name) + ".delivery delay overflows");
        return {};
    };
    if (const std::string e = net("md_channel", c.md_channel); !e.empty()) return e;
    if (const std::string e = net("order_channel", c.order_channel); !e.empty()) return e;
    if (const std::string e = net("exec_channel", c.exec_channel); !e.empty()) return e;

    if (c.algo_latency_ns < 0) return bad("algo_latency_ns < 0");
    if (c.cancel_latency_ns < -1) return bad("cancel_latency_ns < -1 (only -1 is the documented sentinel)");
    if (c.exchange_processing_ns < 0) return bad("exchange_processing_ns < 0");
    if (c.publish_latency_ns < 0) return bad("publish_latency_ns < 0");
    if (c.horizon_ns <= 0) return bad("horizon_ns <= 0");
    if (c.warmup_ns < 0) return bad("warmup_ns < 0");
    if (c.fee_per_lot < 0) return bad("fee_per_lot < 0");
    if (c.fee_per_lot > kMaxFeePerLot) return bad("fee_per_lot overflows fee accounting");

    // book span: must be a representable, strictly positive OptBook ladder
    if (c.book_min_price < DomainBounds::kMinPrice)
        return bad("book_min_price below the representable price domain");
    if (c.book_max_price > kMaxWorldBookPrice)
        return bad("book_max_price exceeds the supported optimized ladder");
    if (c.book_max_price <= c.book_min_price) return bad("book_max_price <= book_min_price");
    if (static_cast<u64>(c.book_max_price) - static_cast<u64>(c.book_min_price) >=
        static_cast<u64>(0xFFFFFFFFu))
        return bad("book price span exceeds the representable ladder");

    // sim
    if (!std::isfinite(c.sim.provider_arrival_per_ms) || c.sim.provider_arrival_per_ms < 0.0)
        return bad("sim.provider_arrival_per_ms invalid");
    if (!std::isfinite(c.sim.taker_arrival_per_ms) || c.sim.taker_arrival_per_ms < 0.0)
        return bad("sim.taker_arrival_per_ms invalid");
    if (!std::isfinite(c.sim.volatile_rate_multiplier) || c.sim.volatile_rate_multiplier < 0.0)
        return bad("sim.volatile_rate_multiplier invalid");
    if (!finite01(c.sim.cancel_fraction)) return bad("sim.cancel_fraction outside [0,1]");
    if (!std::isfinite(c.sim.taker_self_excitation) || c.sim.taker_self_excitation < 0.0 ||
        c.sim.taker_self_excitation >= 1.0)
        return bad("sim.taker_self_excitation outside [0,1)");
    if (c.sim.provider_qty == 0) return bad("sim.provider_qty == 0");
    if (c.sim.taker_qty == 0) return bad("sim.taker_qty == 0");
    if (c.sim.volatile_qty_multiplier == 0) return bad("sim.volatile_qty_multiplier == 0");
    if (c.sim.provider_accounts == 0) return bad("sim.provider_accounts == 0");
    if (c.sim.provider_qty > DomainBounds::kMaxQty || c.sim.taker_qty > DomainBounds::kMaxQty)
        return bad("sim quantity exceeds the representable quantity domain");
    if (c.sim.volatile_qty_multiplier > DomainBounds::kMaxQty / c.sim.provider_qty ||
        c.sim.volatile_qty_multiplier > DomainBounds::kMaxQty / c.sim.taker_qty)
        return bad("sim volatile quantity can overflow the representable quantity domain");
    if (c.sim.provider_accounts > 100)
        return bad("sim.provider_accounts overlaps the taker-bot account");
    if (c.sim.regime_dwell_ns <= 0) return bad("sim.regime_dwell_ns <= 0");
    if (c.sim.touch_offset_ticks < 0 || c.sim.deep_offset_ticks < 0)
        return bad("sim quote offsets < 0");
    if (c.sim.touch_offset_ticks > DomainBounds::kMaxPrice ||
        c.sim.deep_offset_ticks > DomainBounds::kMaxPrice)
        return bad("sim quote offsets exceed the price domain");
    if (c.sim.min_price < DomainBounds::kMinPrice ||
        c.sim.max_price > DomainBounds::kMaxPrice || c.sim.max_price <= c.sim.min_price)
        return bad("sim price range invalid");
    if (c.sim.start_mid < c.sim.min_price || c.sim.start_mid > c.sim.max_price)
        return bad("sim.start_mid outside sim price range");
    if (c.sim.min_price < c.book_min_price || c.sim.max_price > c.book_max_price)
        return bad("sim price range outside book range");

    // risk bounds
    if (c.risk.min_price < DomainBounds::kMinPrice ||
        c.risk.max_price > DomainBounds::kMaxPrice || c.risk.max_price < c.risk.min_price)
        return bad("risk price bounds invalid");
    if (c.risk.max_order_qty == 0 || c.risk.max_order_qty > DomainBounds::kMaxQty)
        return bad("risk.max_order_qty outside the representable quantity domain");
    if (c.risk.max_position < 0 || c.risk.max_position > kMaxPosition)
        return bad("risk.max_position outside the safe arithmetic range");
    if (c.risk.max_notional_per_order < 0) return bad("risk.max_notional_per_order < 0");
    if (c.risk.max_notional_per_order > kMaxNotional)
        return bad("risk.max_notional_per_order overflows order notional");
    if (c.risk.max_msgs_per_window == 0) return bad("risk.max_msgs_per_window == 0");
    if (c.risk.rate_window_ns <= 0) return bad("risk.rate_window_ns <= 0");

    // strategy periods/quantities (zero periods would schedule zero-delay
    // timer storms; zero denominators divide)
    if (c.mm.requote_interval_ns <= 0) return bad("mm.requote_interval_ns <= 0");
    if (c.mm.quote_size == 0 || c.mm.quote_size > DomainBounds::kMaxQty)
        return bad("mm.quote_size outside the representable quantity domain");
    if (c.mm.half_spread_ticks < 0) return bad("mm.half_spread_ticks < 0");
    if (c.mm.skew_denominator <= 0) return bad("mm.skew_denominator <= 0 (division)");
    if (c.mm.max_inventory < 0 || c.mm.max_inventory > kMaxPosition)
        return bad("mm.max_inventory outside the safe arithmetic range");
    if (c.momentum.cooldown_ns <= 0) return bad("momentum.cooldown_ns <= 0");
    if (c.momentum.size == 0 || c.momentum.size > DomainBounds::kMaxQty)
        return bad("momentum.size outside the representable quantity domain");
    if (c.momentum.min_signal <= 0) return bad("momentum.min_signal <= 0");
    if (c.momentum.window == 0) return bad("momentum.window == 0");
    if (c.momentum.max_inventory < 0 || c.momentum.max_inventory > kMaxPosition)
        return bad("momentum.max_inventory outside the safe arithmetic range");
    if (c.momentum.id_base == 0) return bad("momentum.id_base == 0 (order ids start at 1)");
    if (c.mm.account == 0 || c.momentum.account == 0)
        return bad("strategy account ids must be nonzero");

    // account collisions: strategy accounts must be distinct and disjoint
    // from the bot account ranges (provider bots take
    // [100, 100+provider_accounts), taker bots take 200) — a collision would
    // double-account fills across risk/notification routing.
    const u32 provider_lo = 100, provider_hi = 100 + c.sim.provider_accounts;  // exclusive
    auto collides = [&](u32 acct, const char* who) -> std::string {
        if (acct == 200) return bad(std::string(who) + " account collides with the taker-bot account (200)");
        if (acct >= provider_lo && acct < provider_hi)
            return bad(std::string(who) + " account collides with the provider-bot account range");
        return {};
    };
    if (c.mm.account == c.momentum.account)
        return bad("mm.account == momentum.account (double accounting)");
    if (const std::string e = collides(c.mm.account, "mm"); !e.empty()) return e;
    if (const std::string e = collides(c.momentum.account, "momentum"); !e.empty()) return e;
    return {};
}


} // namespace arbiter
