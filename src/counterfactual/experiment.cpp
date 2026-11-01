#include "arbiter/experiment.hpp"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace arbiter {

// ---- shared variable table: config-driven experiments + reproduce ----

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

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

}  // namespace

bool Experiment::apply_key_pub(WorldConfig& c, const std::string& k, const std::string& raw,
                               std::string& err) {
    return apply_key(c, k, raw, err);
}

// ---- file-driven experiment loading ----

Result<Experiment> Experiment::from_config(const Config& cfg, const std::string& fallback_name) {
    const std::string name = cfg.has("experiment.name") ? cfg.get_str("experiment.name", fallback_name)
                                                        : fallback_name;
    Experiment exp(WorldConfig{});
    exp.set_name(name);
    exp.set_seed(cfg.get_u64("experiment.seed", 42));
    WorldConfig base;
    base.journal_events = false;
    base.horizon_ns = cfg.get_i64("experiment.horizon_ns", 40'000'000);
    if (cfg.has("experiment.horizon_ms"))
        base.horizon_ns = cfg.get_i64("experiment.horizon_ms", 40) * 1'000'000;
    base.warmup_ns = cfg.get_i64("experiment.warmup_ns", 2'000'000);

    // collect sections: "baseline" and "variant <NAME>" (dotted form from
    // the Config parser: baseline.<key>, variant.<NAME>.<key>)
    std::vector<std::string> variant_names;
    for (const auto& [k, v] : cfg.entries()) {
        if (k.rfind("variant.", 0) == 0) {
            const std::string rest = k.substr(8);
            const size_t dot = rest.find('.');
            if (dot == std::string::npos) continue;
            const std::string vname = rest.substr(0, dot);
            if (std::find(variant_names.begin(), variant_names.end(), vname) == variant_names.end())
                variant_names.push_back(vname);
        }
    }
    std::string err;
    // baseline overrides: applied to the BASE config (and recorded on the
    // baseline variant so artifacts can reproduce them)
    std::vector<std::pair<std::string, std::string>> base_ovr;
    for (const auto& [k, v] : cfg.entries()) {
        if (k.rfind("baseline.", 0) != 0) continue;
        if (!apply_key(base, k.substr(9), v, err))
            return ErrT<Experiment>(Errc::InvalidArgument, 0);
        base_ovr.emplace_back(k.substr(9), v);
    }
    // rebuild the Experiment from the parsed base
    exp = Experiment(base);
    exp.set_name(name);
    exp.set_seed(cfg.get_u64("experiment.seed", 42));
    {
        Variant v;
        v.name = "baseline";
        v.cfg = base;
        v.overrides = base_ovr;
        exp.add_raw_variant(std::move(v));
    }
    for (const std::string& vn : variant_names) {
        std::vector<std::pair<std::string, std::string>> keys;
        for (const auto& [k, v] : cfg.entries()) {
            const std::string pfx = "variant." + vn + ".";
            if (k.rfind(pfx, 0) != 0) continue;
            keys.emplace_back(k.substr(pfx.size()), v);
        }
        if (keys.empty()) continue;  // empty variant section: skip
        // validate keys against the table BEFORE registering (fail loudly;
        // add_variant re-applies to its own copy)
        for (const auto& [kk, vv] : keys) {
            WorldConfig scratch = base;
            if (!apply_key(scratch, kk, vv, err))
                return ErrT<Experiment>(Errc::InvalidArgument, 1);
        }
        exp.add_variant(vn, std::move(keys));
    }
    // Cross-field validation of the assembled grid: a config error must be a
    // reported CLI error, never a mid-run construction failure.
    if (const std::string invalid = base.validate(); !invalid.empty())
        return ErrT<Experiment>(Errc::InvalidArgument, 2);
    for (const auto& v : exp.variants())
        if (const std::string invalid = v.cfg.validate(); !invalid.empty())
            return ErrT<Experiment>(Errc::InvalidArgument, 3);
    return OkT(std::move(exp));
}

std::string experiment_to_json(const ExperimentResult& r, const std::string& name) {
    // the artifact's identity uses the RESULT's experiment name (what the
    // definition hash was computed over), the caller's display name may
    // be a file stem
    const std::string& ename = r.experiment_name.empty() ? name : r.experiment_name;
    std::string out = "{\n  \"experiment\": \"" + json_escape(ename) + "\",\n";
    out += "  \"seed\": " + std::to_string(r.seed) +
           ",\n  \"definition_hash\": " + std::to_string(r.definition_hash) +
           ",\n  \"horizon_ns\": " + std::to_string(r.base_horizon_ns) + ",\n";
    out += "  \"variants\": [\n";
    for (size_t i = 0; i < r.names.size(); ++i) {
        const RunStats& s = r.stats[i];
        const BranchIdentity& bi = r.branches[i];
        out += "    {\"name\": \"" + json_escape(r.names[i]) +
               "\", \"final_hash\": " + std::to_string(r.final_hashes[i]) +
               ", \"first_divergence_event\": " +
               std::to_string(r.first_divergence_event[i]) + ",\n     \"config_hash\": " +
               std::to_string(r.config_hashes[i]) + ", \"world_id\": " +
               std::to_string(bi.world_id) + ", \"parent_world_id\": " +
               std::to_string(bi.parent_world_id) + ", \"fork_event\": " +
               std::to_string(bi.fork_event) + ",\n     \"events\": " +
               std::to_string(s.events_processed) + ", \"trades\": " +
               std::to_string(s.trades) + ", \"strategy_fills\": " +
               std::to_string(s.strategy_fills) + ",\n     \"mm_marked_pnl\": " +
               std::to_string(saturating_add_i64(
                   s.mm_cash, saturating_mul_i64(s.mm_inventory, s.final_mid))) +
               ", \"mm_inventory\": " + std::to_string(s.mm_inventory) +
               ",\n     \"risk_rejected\": " + std::to_string(s.risk_rejected) +
               ", \"md_dropped\": " + std::to_string(s.md_dropped) +
               ", \"md_recovered\": " + std::to_string(s.md_recovered) +
               ",\n     \"overrides\": [";
        // the override set that produced this variant (reproducibility)
        for (size_t j = 0; j < r.overrides[i].size(); ++j) {
            const std::string key = json_escape(r.overrides[i][j].first);
            const std::string value = json_escape(r.overrides[i][j].second);
            if (j != 0) out += ", ";
            out += "[\"" + key + "\", \"" + value + "\"]";
        }
        out += "]},\n";
    }
    out += "    {\"name\": \"END\"}\n  ]\n}\n";
    return out;
}

std::string experiment_summary(const ExperimentResult& r, const std::string& name) {
    std::string out = "== experiment: " + name + " ==\n";
    char buf[512];
    std::snprintf(buf, sizeof buf, "  seed=%llu definition_hash=%llu\n",
                  (unsigned long long)r.seed, (unsigned long long)r.definition_hash);
    out += buf;
    out += "  variant                     events    trades  fills    PnL(ticks*qty)  inv     first-divergent-checkpoint\n";
    for (size_t i = 0; i < r.names.size(); ++i) {
        const RunStats& s = r.stats[i];
        const i64 pnl = saturating_add_i64(s.mm_cash,
                                           saturating_mul_i64(s.mm_inventory, s.final_mid));
        const u64 div = r.first_divergence_event[i];
        std::snprintf(buf, sizeof buf, "  %-27s %8llu %8llu %6llu %14lld %7lld",
                      r.names[i].c_str(), (unsigned long long)s.events_processed,
                      (unsigned long long)s.trades, (unsigned long long)s.strategy_fills,
                      (long long)pnl, (long long)s.mm_inventory);
        out += buf;
        if (div == ~u64{0})
            out += "  identical\n";
        else {
            char b2[64];
            std::snprintf(b2, sizeof b2, "  @checkpoint(event %llu)\n", (unsigned long long)div);
            out += b2;
        }
    }
    return out;
}

// ---------------- reproduce ----------------

namespace {

// Minimal field scanner for OUR OWN artifact format (zero-dep): find
// "key": value pairs. Not a general JSON parser; anything unexpected is
// an error.
u64 scan_u64(const std::string& text, const std::string& key, bool& ok) {
    ok = false;
    const std::string pat = "\"" + key + "\": ";
    size_t pos = text.find(pat);
    if (pos == std::string::npos) return 0;
    pos += pat.size();
    u64 v = 0;
    bool any = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        const u64 digit = static_cast<u64>(text[pos] - '0');
        if (v > (std::numeric_limits<u64>::max() - digit) / 10) return 0;
        v = v * 10 + digit;
        ++pos;
        any = true;
    }
    ok = any;
    return v;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, u32 cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool parse_json_string(const std::string& text, size_t opening, std::string& out,
                       size_t& after) {
    if (opening >= text.size() || text[opening] != '"') return false;
    out.clear();
    for (size_t i = opening + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            after = i + 1;
            return true;
        }
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i >= text.size()) return false;
        switch (text[i]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 >= text.size()) return false;
                u32 cp = 0;
                for (size_t j = 0; j < 4; ++j) {
                    const int digit = hex_digit(text[i + 1 + j]);
                    if (digit < 0) return false;
                    cp = (cp << 4) | static_cast<u32>(digit);
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDFFF) return false;
                append_utf8(out, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

std::string scan_string(const std::string& text, const std::string& key, bool& ok) {
    ok = false;
    const std::string pat = "\"" + key + "\": \"";
    size_t pos = text.find(pat);
    if (pos == std::string::npos) return {};
    pos += pat.size() - 1;
    std::string value;
    size_t after = 0;
    ok = parse_json_string(text, pos, value, after);
    return ok ? value : std::string{};
}

std::string read_file(const std::string& path, bool& ok) {
    ok = false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    std::fclose(f);
    ok = !all.empty();
    return all;
}

size_t json_object_end(const std::string& text, size_t opening) {
    if (opening >= text.size() || text[opening] != '{') return std::string::npos;
    size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t i = opening; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                quoted = false;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (depth == 0 || --depth == 0) return i + 1;
        }
    }
    return std::string::npos;
}

// Parse one variant record's overrides: "overrides": [["k", "v"], ["k2", "v2"]]
// Structure-scan: each pair is a [ ... ] bracket group inside the list.
std::vector<std::pair<std::string, std::string>> scan_overrides(const std::string& rec, bool& ok) {
    ok = false;
    std::vector<std::pair<std::string, std::string>> out;
    const std::string pat = "\"overrides\": [";
    size_t pos = rec.find(pat);
    if (pos == std::string::npos) {
        ok = true;  // absent: baseline with no overrides
        return out;
    }
    pos += pat.size();
    while (true) {
        while (pos < rec.size() && (rec[pos] == ' ' || rec[pos] == '\t' || rec[pos] == '\r' ||
                                    rec[pos] == '\n' || rec[pos] == ','))
            ++pos;
        if (pos >= rec.size()) return out;
        if (rec[pos] == ']') {
            ok = true;
            return out;
        }
        if (rec[pos] != '[') return out;
        size_t cursor = pos + 1;
        while (cursor < rec.size() && (rec[cursor] == ' ' || rec[cursor] == '\t' ||
                                       rec[cursor] == '\r' || rec[cursor] == '\n'))
            ++cursor;
        std::string key, value;
        size_t after = 0;
        if (!parse_json_string(rec, cursor, key, after)) return out;
        cursor = after;
        while (cursor < rec.size() && (rec[cursor] == ' ' || rec[cursor] == '\t' ||
                                       rec[cursor] == '\r' || rec[cursor] == '\n'))
            ++cursor;
        if (cursor >= rec.size() || rec[cursor++] != ',') return out;
        while (cursor < rec.size() && (rec[cursor] == ' ' || rec[cursor] == '\t' ||
                                       rec[cursor] == '\r' || rec[cursor] == '\n'))
            ++cursor;
        if (!parse_json_string(rec, cursor, value, after)) return out;
        cursor = after;
        while (cursor < rec.size() && (rec[cursor] == ' ' || rec[cursor] == '\t' ||
                                       rec[cursor] == '\r' || rec[cursor] == '\n'))
            ++cursor;
        if (cursor >= rec.size() || rec[cursor] != ']') return out;
        out.emplace_back(std::move(key), std::move(value));
        pos = cursor + 1;
    }
}

}  // namespace

ReproduceReport experiment_reproduce(const std::string& artifact_path) {
    ReproduceReport rep;
    rep.detail = "ok";
    bool ok = false;
    const std::string text = read_file(artifact_path, ok);
    if (!ok) {
        rep.detail = "cannot read artifact: " + artifact_path;
        return rep;
    }
    const std::string name = scan_string(text, "experiment", ok);
    if (!ok) {
        rep.detail = "artifact missing experiment name";
        return rep;
    }
    const u64 seed = scan_u64(text, "seed", ok);
    if (!ok) {
        rep.detail = "artifact missing seed";
        return rep;
    }
    const u64 definition_hash = scan_u64(text, "definition_hash", ok);
    if (!ok) {
        rep.detail = "artifact missing definition_hash";
        return rep;
    }
    const u64 horizon = scan_u64(text, "horizon_ns", ok);
    if (!ok) {
        rep.detail = "artifact missing horizon_ns";
        return rep;
    }

    // harvest (name, final_hash, config_hash, overrides) per variant
    std::vector<std::string> vnames;
    std::vector<u64> vfinal, vcfg;
    std::vector<std::vector<std::pair<std::string, std::string>>> vovr;
    {
        size_t pos = text.find("\"variants\":");
        if (pos == std::string::npos) {
            rep.detail = "artifact missing variants";
            return rep;
        }
        std::string rest = text.substr(pos);
        size_t vpos = 0;
        while (true) {
            vpos = rest.find("{\"name\": \"", vpos);
            if (vpos == std::string::npos) break;
            std::string vname;
            size_t nend = 0;
            if (!parse_json_string(rest, vpos + 9, vname, nend)) {
                rep.detail = "malformed variant name";
                return rep;
            }
            if (vname == "END") break;
            // per-variant fields: scan within this variant's record only
            const size_t vend = json_object_end(rest, vpos);
            if (vend == std::string::npos) {
                rep.detail = "unterminated variant record: " + vname;
                return rep;
            }
            const std::string rec = rest.substr(vpos, vend - vpos);
            bool fok = false, cok = false, ook = false;
            const u64 fh = scan_u64(rec, "final_hash", fok);
            const u64 ch = scan_u64(rec, "config_hash", cok);
            auto ovr = scan_overrides(rec, ook);
            if (!fok || !cok || !ook) {
                rep.detail = "variant record incomplete: " + vname;
                return rep;
            }
            vnames.push_back(vname);
            vfinal.push_back(fh);
            vcfg.push_back(ch);
            vovr.push_back(std::move(ovr));
            vpos = vend;
        }
    }
    if (vnames.empty()) {
        rep.detail = "no variants in artifact";
        return rep;
    }

    // rebuild the grid from the recorded override sets (GENERAL: every
    // artifact carries its own definition; no built-in mutator table needed).
    // Variant 0's overrides are BASELINE overrides, applied to the base
    // before any variant is built.
    WorldConfig base;
    base.journal_events = false;
    base.warmup_ns = 2'000'000;
    base.horizon_ns = static_cast<LogicalNs>(horizon);
    for (const auto& [k, val] : vovr[0]) {
        std::string err;
        if (!Experiment::apply_key_pub(base, k, val, err)) {
            rep.detail = "unknown baseline variable in artifact: " + k;
            return rep;
        }
    }
    Experiment exp(base);
    exp.set_seed(seed);
    exp.set_name(name);
    {
        Variant v;
        v.name = vnames[0];
        v.cfg = base;
        v.overrides = vovr[0];
        exp.add_raw_variant(std::move(v));
    }
    for (size_t i = 1; i < vnames.size(); ++i) {
        // validate keys before registering (unknown keys fail loudly)
        for (const auto& [k, val] : vovr[i]) {
            WorldConfig scratch = base;
            std::string err;
            if (!Experiment::apply_key_pub(scratch, k, val, err)) {
                rep.detail = "unknown variable in artifact: " + k;
                return rep;
            }
        }
        exp.add_variant(vnames[i], vovr[i]);
    }
    if (exp.definition_hash() != definition_hash) {
        rep.detail = "definition_hash mismatch: rebuilt " + std::to_string(exp.definition_hash()) +
                     " vs artifact " + std::to_string(definition_hash);
        return rep;
    }

    const ExperimentResult res = exp.run();
    rep.variants_checked = vnames.size();
    for (size_t i = 0; i < vnames.size(); ++i) {
        if (res.final_hashes[i] != vfinal[i]) ++rep.hash_mismatches;
        if (res.config_hashes[i] != vcfg[i]) ++rep.hash_mismatches;
    }
    rep.ok = rep.hash_mismatches == 0;
    if (!rep.ok)
        rep.detail = "hash mismatches: " + std::to_string(rep.hash_mismatches);
    return rep;
}

}  // namespace arbiter
