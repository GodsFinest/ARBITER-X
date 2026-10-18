#include "arbiter/experiment.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace arbiter {

// ---- shared variable table: config-driven experiments + reproduce ----

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
    else if (k == "sim.cancel_fraction") c.sim.cancel_fraction = static_cast<double>(v) / 1e9;
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
    // try i64 fields first (most keys), then u64-net, then f64
    if (apply_i64(c, k, std::strtoll(raw.c_str(), nullptr, 10))) return true;
    if (apply_i64_net(c, k, std::strtoll(raw.c_str(), nullptr, 10))) return true;
    if (apply_u64_net(c, k, std::strtoull(raw.c_str(), nullptr, 10))) return true;
    if (apply_f64(c, k, std::strtod(raw.c_str(), nullptr))) return true;
    if (k == "risk.kill_switch" || k == "risk.enabled" || k == "mm.enabled" ||
        k == "momentum.enabled" || k == "bots_observe_shadow" || k == "stage_tracing") {
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
    return OkT(std::move(exp));
}

std::string experiment_to_json(const ExperimentResult& r, const std::string& name) {
    // the artifact's identity uses the RESULT's experiment name (what the
    // definition hash was computed over), the caller's display name may
    // be a file stem
    const std::string& ename = r.experiment_name.empty() ? name : r.experiment_name;
    std::string out = "{\n  \"experiment\": \"" + ename + "\",\n";
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "  \"seed\": %llu,\n  \"definition_hash\": %llu,\n  \"horizon_ns\": %lld,\n",
                  (unsigned long long)r.seed, (unsigned long long)r.definition_hash,
                  (long long)r.base_horizon_ns);
    out += buf;
    out += "  \"variants\": [\n";
    for (size_t i = 0; i < r.names.size(); ++i) {
        const RunStats& s = r.stats[i];
        const BranchIdentity& bi = r.branches[i];
        std::snprintf(buf, sizeof buf,
                       "    {\"name\": \"%s\", \"final_hash\": %llu, \"first_divergence_event\": %llu,\n"
                       "     \"config_hash\": %llu, \"world_id\": %llu, "
                       "\"parent_world_id\": %llu, \"fork_event\": %llu,\n"
                       "     \"events\": %llu, \"trades\": %llu, \"strategy_fills\": %llu,\n"
                       "     \"mm_marked_pnl\": %lld, \"mm_inventory\": %lld,\n"
                       "     \"risk_rejected\": %llu, \"md_dropped\": %llu, \"md_recovered\": %llu,\n"
                       "     \"overrides\": [",
                       r.names[i].c_str(),
                       (unsigned long long)r.final_hashes[i],
                       (unsigned long long)r.first_divergence_event[i],
                       (unsigned long long)r.config_hashes[i],
                       (unsigned long long)bi.world_id,
                       (unsigned long long)bi.parent_world_id,
                       (unsigned long long)bi.fork_event,
                       (unsigned long long)s.events_processed,
                       (unsigned long long)s.trades,
                       (unsigned long long)s.strategy_fills,
                       (long long)(s.mm_cash + s.mm_inventory * s.final_mid),
                       (long long)s.mm_inventory,
                       (unsigned long long)s.risk_rejected,
                       (unsigned long long)s.md_dropped,
                       (unsigned long long)s.md_recovered);
        out += buf;
        // the override set that produced this variant (reproducibility)
        for (size_t j = 0; j < r.overrides[i].size(); ++j) {
            std::snprintf(buf, sizeof buf, "%s[\"%s\", \"%s\"]",
                          j ? ", " : "", r.overrides[i][j].first.c_str(),
                          r.overrides[i][j].second.c_str());
            out += buf;
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
    out += "  variant                     events    trades  fills    PnL(ticks*qty)  inv     first-divergence\n";
    for (size_t i = 0; i < r.names.size(); ++i) {
        const RunStats& s = r.stats[i];
        const i64 pnl = s.mm_cash + s.mm_inventory * s.final_mid;
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
            std::snprintf(b2, sizeof b2, "  @event %llu\n", (unsigned long long)div);
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
        v = v * 10 + static_cast<u64>(text[pos] - '0');
        ++pos;
        any = true;
    }
    ok = any;
    return v;
}

std::string scan_string(const std::string& text, const std::string& key, bool& ok) {
    ok = false;
    const std::string pat = "\"" + key + "\": \"";
    size_t pos = text.find(pat);
    if (pos == std::string::npos) return {};
    pos += pat.size();
    const size_t end = text.find('"', pos);
    if (end == std::string::npos) return {};
    ok = true;
    return text.substr(pos, end - pos);
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

// Parse one variant record's overrides: "overrides": [["k", "v"], ["k2", "v2"]]
// Structure-scan: each pair is a [ ... ] bracket group inside the list.
std::vector<std::pair<std::string, std::string>> scan_overrides(const std::string& rec, bool& ok) {
    ok = false;
    std::vector<std::pair<std::string, std::string>> out;
    const std::string pat = "\"overrides\": [";
    size_t pos = rec.find(pat);
    if (pos == std::string::npos) return out;  // absent: baseline with no overrides
    pos += pat.size();
    while (true) {
        const size_t b = rec.find('[', pos);
        if (b == std::string::npos) break;
        const size_t e = rec.find(']', b);
        if (e == std::string::npos) return out;  // malformed: unterminated pair
        // inside [b, e]: "key", "value"
        const size_t q1 = rec.find('"', b);
        if (q1 == std::string::npos || q1 > e) break;
        const size_t q2 = rec.find('"', q1 + 1);
        const size_t q3 = rec.find('"', q2 + 1);
        const size_t q4 = rec.find('"', q3 + 1);
        if (q2 == std::string::npos || q3 == std::string::npos || q4 == std::string::npos ||
            q4 > e) {
            ok = false;
            return out;
        }
        out.emplace_back(rec.substr(q1 + 1, q2 - q1 - 1), rec.substr(q3 + 1, q4 - q3 - 1));
        pos = e + 1;
    }
    ok = true;
    return out;
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
            const size_t nend = rest.find('"', vpos + 10);
            if (nend == std::string::npos) break;
            const std::string vname = rest.substr(vpos + 10, nend - (vpos + 10));
            if (vname == "END") break;
            // per-variant fields: scan within this variant's record only
            const size_t vend = rest.find("},", nend);
            std::string rec = rest.substr(vpos, vend == std::string::npos ? std::string::npos : vend - vpos);
            bool fok = false, cok = false, ook = false;
            const u64 fh = scan_u64(rec, "final_hash", fok);
            const u64 ch = scan_u64(rec, "config_hash", cok);
            auto ovr = scan_overrides(rec, ook);
            if (!fok || !cok) {
                rep.detail = "variant record incomplete: " + vname;
                return rep;
            }
            vnames.push_back(vname);
            vfinal.push_back(fh);
            vcfg.push_back(ch);
            vovr.push_back(std::move(ovr));
            vpos = nend;
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