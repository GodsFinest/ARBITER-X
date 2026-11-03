#include "arbiter/world_overrides.hpp"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
namespace arbiter {
namespace {
// Apply ONE dotted-key override to a WorldConfig. Returns false on an
// unknown key (reproduction must never guess). Values arrive pre-parsed by
// the caller per type; this table maps names to fields.
bool apply_i64(WorldConfig& c, const std::string& k, i64 v) {
    if (k == "algo_latency_ns") c.algo_latency_ns = v;
    else if (k == "cancel_latency_ns") c.cancel_latency_ns = v;
    else if (k == "exchange_processing_ns") c.exchange_processing_ns = v;
    else if (k == "publish_latency_ns") c.publish_latency_ns = v;
    else if (k == "horizon_ns") c.horizon_ns = v;
    else if (k == "warmup_ns") c.warmup_ns = v;
    else if (k == "fee_per_lot") c.fee_per_lot = v;
    else if (k == "book_min_price") c.book_min_price = v;
    else if (k == "book_max_price") c.book_max_price = v;
    else if (k == "risk.min_price") c.risk.min_price = v;
    else if (k == "risk.max_price") c.risk.max_price = v;
    else if (k == "risk.max_order_qty") c.risk.max_order_qty = static_cast<u64>(v);
    else if (k == "risk.max_position") c.risk.max_position = v;
    else if (k == "risk.max_notional_per_order") c.risk.max_notional_per_order = v;
    else if (k == "risk.max_msgs_per_window") c.risk.max_msgs_per_window = static_cast<u32>(v);
    else if (k == "risk.rate_window_ns") c.risk.rate_window_ns = v;
    else if (k == "mm.account") c.mm.account = static_cast<u32>(v);
    else if (k == "mm.quote_size") c.mm.quote_size = static_cast<u64>(v);
    else if (k == "mm.half_spread_ticks") c.mm.half_spread_ticks = v;
    else if (k == "mm.skew_ticks") c.mm.skew_ticks = v;
    else if (k == "mm.skew_denominator") c.mm.skew_denominator = v;
    else if (k == "mm.max_inventory") c.mm.max_inventory = v;
    else if (k == "mm.requote_interval_ns") c.mm.requote_interval_ns = v;
    else if (k == "momentum.account") c.momentum.account = static_cast<u32>(v);
    else if (k == "momentum.size") c.momentum.size = static_cast<u64>(v);
    else if (k == "momentum.min_signal") c.momentum.min_signal = v;
    else if (k == "momentum.window") c.momentum.window = static_cast<size_t>(v);
    else if (k == "momentum.cooldown_ns") c.momentum.cooldown_ns = v;
    else if (k == "momentum.max_inventory") c.momentum.max_inventory = v;
    else if (k == "momentum.id_base") c.momentum.id_base = static_cast<u64>(v);
    else if (k == "sim.start_mid") c.sim.start_mid = v;
    else if (k == "sim.provider_qty") c.sim.provider_qty = static_cast<u64>(v);
    else if (k == "sim.taker_qty") c.sim.taker_qty = static_cast<u64>(v);
    else if (k == "sim.touch_offset_ticks") c.sim.touch_offset_ticks = v;
    else if (k == "sim.deep_offset_ticks") c.sim.deep_offset_ticks = v;
    else if (k == "sim.regime_dwell_ns") c.sim.regime_dwell_ns = v;
    else if (k == "sim.provider_accounts") c.sim.provider_accounts = static_cast<u32>(v);
    else if (k == "sim.min_price") c.sim.min_price = v;
    else if (k == "sim.max_price") c.sim.max_price = v;
    else return false;
    return true;
}

bool apply_f64(WorldConfig& c, const std::string& k, double v) {
    if (k == "sim.cancel_fraction") c.sim.cancel_fraction = v;  // fraction (0..1), NOT scaled
    else if (k == "sim.taker_self_excitation") c.sim.taker_self_excitation = v;
    else if (k == "md_channel.loss_rate") c.md_channel.loss_rate = v;
    else if (k == "md_channel.dup_rate") c.md_channel.dup_rate = v;
    else if (k == "md_channel.burst_loss_rate") c.md_channel.burst_loss_rate = v;
    else if (k == "md_channel.corrupt_rate") c.md_channel.corrupt_rate = v;
    else if (k == "order_channel.loss_rate") c.order_channel.loss_rate = v;
    else if (k == "order_channel.dup_rate") c.order_channel.dup_rate = v;
    else if (k == "order_channel.corrupt_rate") c.order_channel.corrupt_rate = v;
    else if (k == "exec_channel.loss_rate") c.exec_channel.loss_rate = v;
    else if (k == "exec_channel.dup_rate") c.exec_channel.dup_rate = v;
    else if (k == "exec_channel.corrupt_rate") c.exec_channel.corrupt_rate = v;
    else if (k == "sim.provider_arrival_per_ms") c.sim.provider_arrival_per_ms = v;
    else if (k == "sim.taker_arrival_per_ms") c.sim.taker_arrival_per_ms = v;
    else if (k == "sim.volatile_rate_multiplier") c.sim.volatile_rate_multiplier = v;
    else return false;
    return true;
}

bool apply_i64_net(WorldConfig& c, const std::string& k, i64 v) {
    if (k == "md_channel.base_delay_ns") c.md_channel.base_delay_ns = v;
    else if (k == "md_channel.jitter_ns") c.md_channel.jitter_ns = v;
    else if (k == "md_channel.reorder_slot_ns") c.md_channel.reorder_slot_ns = v;
    else if (k == "order_channel.base_delay_ns") c.order_channel.base_delay_ns = v;
    else if (k == "order_channel.jitter_ns") c.order_channel.jitter_ns = v;
    else if (k == "order_channel.reorder_slot_ns") c.order_channel.reorder_slot_ns = v;
    else if (k == "exec_channel.base_delay_ns") c.exec_channel.base_delay_ns = v;
    else if (k == "exec_channel.jitter_ns") c.exec_channel.jitter_ns = v;
    else if (k == "exec_channel.reorder_slot_ns") c.exec_channel.reorder_slot_ns = v;
    else return false;
    return true;
}

bool apply_u64_net(WorldConfig& c, const std::string& k, u64 v) {
    if (k == "md_channel.reorder_window") c.md_channel.reorder_window = static_cast<u32>(v);
    else if (k == "md_channel.burst_loss_len") c.md_channel.burst_loss_len = static_cast<u32>(v);
    else if (k == "order_channel.reorder_window") c.order_channel.reorder_window = static_cast<u32>(v);
    else if (k == "order_channel.burst_loss_len") c.order_channel.burst_loss_len = static_cast<u32>(v);
    else if (k == "exec_channel.reorder_window") c.exec_channel.reorder_window = static_cast<u32>(v);
    else if (k == "exec_channel.burst_loss_len") c.exec_channel.burst_loss_len = static_cast<u32>(v);
    else if (k == "sim.volatile_qty_multiplier") c.sim.volatile_qty_multiplier = static_cast<u64>(v);
    else return false;
    return true;
}

// Apply one (key, raw-string-value) pair; type-dispatched by field table.
bool apply_key(WorldConfig& c, const std::string& k, const std::string& raw, std::string& err) {
    auto parse_i64 = [&](i64& v) {
        errno = 0;
        char* end = nullptr;
        const long long x = std::strtoll(raw.c_str(), &end, 10);
        if (errno == ERANGE || end == raw.c_str() || *end != '\0') return false;
        v = static_cast<i64>(x);
        return true;
    };
    auto parse_u64 = [&](u64& v) {
        if (!raw.empty() && raw.front() == '-') return false;
        errno = 0;
        char* end = nullptr;
        const unsigned long long x = std::strtoull(raw.c_str(), &end, 10);
        if (errno == ERANGE || end == raw.c_str() || *end != '\0') return false;
        v = static_cast<u64>(x);
        return true;
    };
    auto parse_f64 = [&](double& v) {
        errno = 0;
        char* end = nullptr;
        const double x = std::strtod(raw.c_str(), &end);
        if (errno == ERANGE || end == raw.c_str() || *end != '\0' || !std::isfinite(x))
            return false;
        v = x;
        return true;
    };
    // Probe the type table before parsing so a malformed value for a known
    // key is rejected instead of falling through as an "unknown" override.
    WorldConfig probe = c;
    if (apply_f64(probe, k, 0.0)) {
        double v = 0.0;
        if (!parse_f64(v)) { err = k + "=" + raw; return false; }
        const bool probability = k.ends_with(".loss_rate") || k.ends_with(".dup_rate") ||
                                 k.ends_with(".burst_loss_rate") ||
                                 k.ends_with(".corrupt_rate") ||
                                 k == "sim.cancel_fraction" ||
                                 k == "sim.taker_self_excitation";
        if ((probability && (v < 0.0 || v > 1.0)) ||
            (k == "sim.taker_self_excitation" && v >= 1.0) ||
            ((k == "sim.provider_arrival_per_ms" || k == "sim.taker_arrival_per_ms" ||
              k == "sim.volatile_rate_multiplier") && v < 0.0)) {
            err = k + "=" + raw;
            return false;
        }
        return apply_f64(c, k, v);
    }
    probe = c;
    if (apply_i64(probe, k, 0)) {
        i64 v = 0;
        if (!parse_i64(v)) { err = k + "=" + raw; return false; }
        const bool unsigned_field =
            k == "risk.max_order_qty" || k == "risk.max_msgs_per_window" ||
            k == "mm.account" || k == "mm.quote_size" || k == "momentum.account" ||
            k == "momentum.size" || k == "momentum.window" || k == "momentum.id_base" ||
            k == "sim.provider_qty" || k == "sim.taker_qty" || k == "sim.provider_accounts";
        const bool positive_period = k == "mm.requote_interval_ns" ||
                                     k == "momentum.cooldown_ns" ||
                                     k == "risk.rate_window_ns";
        const bool u32_field = k == "risk.max_msgs_per_window" || k == "mm.account" ||
                               k == "momentum.account" || k == "sim.provider_accounts";
        if ((unsigned_field && v < 0) || (positive_period && v <= 0) ||
            (u32_field && static_cast<u64>(v) > 0xFFFFFFFFull)) {
            err = k + "=" + raw;
            return false;
        }
        return apply_i64(c, k, v);
    }
    probe = c;
    if (apply_i64_net(probe, k, 0)) {
        i64 v = 0;
        if (!parse_i64(v) || v < 0) { err = k + "=" + raw; return false; }
        return apply_i64_net(c, k, v);
    }
    probe = c;
    if (apply_u64_net(probe, k, 0)) {
        u64 v = 0;
        if (!parse_u64(v)) { err = k + "=" + raw; return false; }
        return apply_u64_net(c, k, v);
    }
    if (k == "risk.kill_switch" || k == "risk.enabled" || k == "mm.enabled" ||
        k == "momentum.enabled" || k == "bots_observe_shadow" || k == "stage_tracing") {
        if (raw != "true" && raw != "1" && raw != "false" && raw != "0") {
            err = k + "=" + raw;
            return false;
        }
        const bool b = raw == "true" || raw == "1";
        if (k == "risk.kill_switch") c.risk.kill_switch = b;
        else if (k == "risk.enabled") c.risk.enabled = b;
        else if (k == "mm.enabled") c.mm.enabled = b;
        else if (k == "momentum.enabled") c.momentum.enabled = b;
        else if (k == "bots_observe_shadow") c.bots_observe_shadow = b;
        else c.stage_tracing = b;
        return true;
    }
    err = k;
    return false;
}

}
bool apply_world_override(WorldConfig& c, const std::string& key,
                          const std::string& raw, std::string& error) {
    return apply_key(c, key, raw, error);
}
}
