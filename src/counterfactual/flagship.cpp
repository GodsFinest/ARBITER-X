// Flagship experiment implementation. See header for the design contract.
#include "arbiter/flagship.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/research.hpp"
#include "arbiter/rng.hpp"
#include "arbiter/stats.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace arbiter {

namespace {

WorldConfig base_world_config(const FlagshipConfig& fc) {
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = fc.horizon_ns;
    cfg.warmup_ns = fc.warmup_ns;
    cfg.bots_observe_shadow = fc.bots_observe_shadow;
    cfg.sim.taker_self_excitation =
        fc.flow_model == FlowModel::Clustered ? fc.taker_self_excitation : 0.0;
    // deterministic clean channels for every cell: the flagship isolates
    // PLACEMENT; stochastic faults are a separate axis (network_grid)
    cfg.md_channel.jitter_ns = 0;
    cfg.order_channel.jitter_ns = 0;
    cfg.exec_channel.jitter_ns = 0;
    return cfg;
}

CellSeedOutcome run_one(const FlagshipConfig& fc, const LatencyAllocation& alloc, u64 seed) {
    WorldConfig cfg = alloc.apply_to(base_world_config(fc));
    World w(cfg, seed);
    const RunStats& s = w.run();
    CellSeedOutcome o;
    o.mm_marked_pnl = s.mm_cash + s.mm_inventory * s.final_mid;
    o.mom_marked_pnl = s.mom_cash + s.mom_inventory * s.final_mid;
    o.mm_max_drawdown = s.mm_max_drawdown;
    o.mm_fills = s.strategy_fills;
    o.mm_filled_qty = s.mm_real_filled_qty;
    o.mom_fills = s.momentum_fills;
    o.mom_filled_qty = s.mom_filled_qty;  // authoritative LOTS (measured)
    o.mm_orders_sent = s.strategy_orders_sent;
    o.mm_cancels_sent = s.strategy_cancels_sent;
    o.mm_orders_rejected = s.risk_rejected;
    o.final_state_hash = w.state_hash();
    o.events = s.events_processed;
    // markouts: mean of the archetype-separated histograms (0 when empty)
    if (s.adverse_100us.count()) o.mm_markout_100us = s.adverse_100us.mean();
    if (s.mom_adverse_100us.count()) o.mom_markout_100us = s.mom_adverse_100us.mean();
    // realized stage means, engine-measured verification of the allocation
    const u64 n = s.stage_traced_fills;
    if (n > 0) {
        o.mean_decision_ns = static_cast<double>(s.stage_sum_decision_to_depart) / n;
        o.mean_order_transit_ns = static_cast<double>(s.stage_sum_depart_to_gateway) / n;
        // md transit is not per-fill traceable (it precedes the decision):
        // realized = configured base delay for the clean channel; the
        // report marks this distinction explicitly.
        o.mean_md_transit_ns = static_cast<double>(alloc.md_transit_ns);
    }
    return o;
}

std::string cell_name(double md_share) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "md%.3f", md_share);
    return buf;
}

// Two-sided sign-permutation p-value for the mean of paired differences:
// under H0 (exchangeable signs), all 2^n sign flips of the difference
// vector are equally likely. Exact enumeration for n <= 20; seeded random
// flips for larger n (reproducible).
double permutation_pvalue(const std::vector<double>& d, u64 seed, size_t reps) {
    const size_t n = d.size();
    if (n == 0) return 1.0;
    double obs = 0.0;
    for (double v : d) obs += v;
    // enumerate all sign flips (n <= 20: <= 1M flips)
    if (n <= 20) {
        const size_t flips = 1u << n;
        size_t ge = 0;
        for (size_t f = 0; f < flips; ++f) {
            double s = 0.0;
            for (size_t i = 0; i < n; ++i) s += (f >> i) & 1u ? -d[i] : d[i];
            if (std::fabs(s) >= std::fabs(obs) - 1e-9) ++ge;
        }
        return static_cast<double>(ge) / static_cast<double>(flips);
    }
    // seeded random flips
    Rng rng(Rng::stream(seed, 0x5045524Dull).state);
    size_t ge = 0;
    for (size_t r = 0; r < reps; ++r) {
        double s = 0.0;
        for (size_t i = 0; i < n; ++i) s += rng.chance(0.5) ? -d[i] : d[i];
        if (std::fabs(s) >= std::fabs(obs) - 1e-9) ++ge;
    }
    return static_cast<double>(ge) / static_cast<double>(reps);
}

}  // namespace

FlagshipResult run_flagship(const FlagshipConfig& fc) {
    FlagshipResult res;
    res.cfg = fc;
    res.flow_model_name = fc.flow_model == FlowModel::Poisson ? "poisson" : "clustered";
    res.compiler = research::compiler_id();
    res.platform = research::platform_id();
    res.created_utc = research::utc_now();

    // definition hash: everything that changes numbers
    {
        Hasher h;
        h.tag("ARBITERX/FLAGSHIP/1");
        for (char c : fc.name) h.u8v(static_cast<u8>(c));
        h.i64v(fc.total_latency_ns);
        h.u64v(fc.md_shares.size());
        for (double x : fc.md_shares) {
            char b[32];
            std::snprintf(b, sizeof b, "%.17g", x);
            h.str(b);
        }
        h.u8v(static_cast<u8>(fc.flow_model));
        char b[32];
        std::snprintf(b, sizeof b, "%.17g", fc.taker_self_excitation);
        h.str(b);
        h.boolean(fc.bots_observe_shadow);
        h.u64v(fc.seeds.size());
        for (u64 s : fc.seeds) h.u64v(s);
        h.i64v(fc.horizon_ns);
        h.i64v(fc.warmup_ns);
        h.u64v(static_cast<u64>(fc.economic_threshold_ticks_qty_per_lot * 1e9));
        h.u64v(fc.permutation_reps);
        h.u64v(static_cast<u64>(fc.hac_lag));
        res.definition_hash = h.value();
    }

    // run the grid: every cell over every seed (paired by construction:
    // same seed -> same master RNG streams)
    res.cells.reserve(fc.md_shares.size());
    for (double share : fc.md_shares) {
        CellResult cell;
        cell.name = cell_name(share);
        cell.alloc = allocation_on_slice(fc.total_latency_ns, share);
        cell.per_seed.reserve(fc.seeds.size());
        for (u64 seed : fc.seeds) cell.per_seed.push_back(run_one(fc, cell.alloc, seed));
        double n = static_cast<double>(cell.per_seed.size());
        double mm_pnl = 0, mom_pnl = 0, mm_f = 0, mm_q = 0, mom_q = 0, mm_m = 0, mom_m = 0;
        for (const auto& o : cell.per_seed) {
            mm_pnl += o.mm_marked_pnl;
            mom_pnl += o.mom_marked_pnl;
            mm_f += o.mm_fills;
            mm_q += o.mm_filled_qty;
            mom_q += o.mom_filled_qty;
            mm_m += o.mm_markout_100us;
            mom_m += o.mom_markout_100us;
        }
        cell.mean_mm_pnl = mm_pnl / n;
        cell.mean_mom_pnl = mom_pnl / n;
        cell.mean_mm_fills = mm_f / n;
        cell.mean_mm_filled_qty = mm_q / n;
        cell.mean_mom_filled_qty = mom_q / n;
        cell.mean_mm_markout = mm_m / n;
        cell.mean_mom_markout = mom_m / n;
        res.cells.push_back(std::move(cell));
    }

    // contrast family: adjacent cells along the slice + the two extremes.
    // The extreme pair (md0.000 vs md1.000) is the PRIMARY contrast.
    auto add_pair = [&](size_t ia, size_t ib) {
        PairComparison p;
        p.cell_a = res.cells[ia].name;
        p.cell_b = res.cells[ib].name;
        p.alloc_a = res.cells[ia].alloc;
        p.alloc_b = res.cells[ib].alloc;
        const size_t nseeds = fc.seeds.size();
        p.mm_pnl_diff.reserve(nseeds);
        p.mom_pnl_diff.reserve(nseeds);
        p.mm_fills_diff.reserve(nseeds);
        p.mm_markout_diff.reserve(nseeds);
        for (size_t k = 0; k < nseeds; ++k) {
            const CellSeedOutcome& a = res.cells[ia].per_seed[k];
            const CellSeedOutcome& b = res.cells[ib].per_seed[k];
            p.mm_pnl_diff.push_back(static_cast<double>(b.mm_marked_pnl - a.mm_marked_pnl));
            p.mom_pnl_diff.push_back(static_cast<double>(b.mom_marked_pnl - a.mom_marked_pnl));
            p.mm_fills_diff.push_back(static_cast<double>(b.mm_fills - a.mm_fills));
            p.mm_markout_diff.push_back(b.mm_markout_100us - a.mm_markout_100us);
        }
        const stats::MeanCI ci = stats::mean_ci(p.mm_pnl_diff, fc.hac_lag);
        p.mean_diff = ci.mean;
        p.se_iid = ci.se_iid;
        p.ci95_lo = ci.ci95_lo;
        p.ci95_hi = ci.ci95_hi;
        p.p_permutation = permutation_pvalue(p.mm_pnl_diff, res.definition_hash,
                                             fc.permutation_reps);
        // economic scale: per-lot PnL difference (ticks*qty per filled lot,
        // using the two cells' average traded volume as the denominator)
        const double dq = res.cells[ib].mean_mm_filled_qty + res.cells[ia].mean_mm_filled_qty;
        p.pnl_diff_per_lot = dq > 0 ? (p.mean_diff / (dq / 2.0)) : 0.0;
        p.statistically_significant = p.p_permutation < 0.05;
        p.economically_material =
            std::fabs(p.pnl_diff_per_lot) >= fc.economic_threshold_ticks_qty_per_lot;
        res.pairs.push_back(std::move(p));
    };
    for (size_t i = 0; i + 1 < res.cells.size(); ++i) add_pair(i, i + 1);
    if (res.cells.size() >= 2) add_pair(0, res.cells.size() - 1);  // primary

    std::vector<double> raw_p;
    raw_p.reserve(res.pairs.size());
    for (const auto& pair : res.pairs) raw_p.push_back(pair.p_permutation);
    const std::vector<double> adjusted_p = stats::holm(raw_p);
    for (size_t i = 0; i < res.pairs.size(); ++i) {
        res.pairs[i].p_holm = adjusted_p[i];
        res.pairs[i].statistically_significant = adjusted_p[i] < 0.05;
    }
    // findings: falsification-first reading of the grid. NO hardcoded
    // narrative, each statement is computed from the data.
    {
        char b[256];
        // primary contrast = extremes (md0.000 vs md1.000), always present
        const PairComparison& prim = res.pairs.back();
        std::snprintf(b, sizeof b,
                      "primary contrast (%s -> %s): mean paired MM PnL diff %+.1f ticks*qty "
                      "(per-lot %+.2f), Holm p=%.4f, %s",
                      prim.cell_a.c_str(), prim.cell_b.c_str(), prim.mean_diff,
                      prim.pnl_diff_per_lot, prim.p_holm,
                      prim.statistically_significant ? "significant" : "not significant");
        res.findings.push_back(b);
        std::snprintf(b, sizeof b,
                      "economic materiality (primary): %s (threshold %.2f ticks*qty/fill)",
                      prim.economically_material ? "MATERIAL" : "not material",
                      fc.economic_threshold_ticks_qty_per_lot);
        res.findings.push_back(b);
        // fill-count movement across the slice (execution mechanics channel)
        const double f_lo = res.cells.front().mean_mm_filled_qty;
        const double f_hi = res.cells.back().mean_mm_filled_qty;
        std::snprintf(b, sizeof b,
                      "MM filled lots across the slice: %.1f (all-observation) vs %.1f "
                      "(all-action): %s", f_hi, f_lo,
                      std::fabs(f_hi - f_lo) > 1e-9 ? "placement changes fill mechanics"
                                                    : "fill mechanics invariant to placement");
        res.findings.push_back(b);
        // taker-side economics movement
        const double m_lo = res.cells.front().mean_mom_pnl;
        const double m_hi = res.cells.back().mean_mom_pnl;
        std::snprintf(b, sizeof b,
                      "taker (momentum) PnL across the slice: %.1f -> %.1f (%s)",
                      m_lo, m_hi,
                      std::fabs(m_hi - m_lo) > 1e-9 ? "moves" : "invariant");
        res.findings.push_back(b);
        // taker-side inference at the primary contrast (endpoint reported for
        // archetype heterogeneity; NOT part of the Holm family, the MM PnL
        // contrast is the predefined primary, the taker line is descriptive
        // with its own paired mean/se/p)
        {
            const PairComparison& prim2 = res.pairs.back();
            const stats::MeanCI mci = stats::mean_ci(prim2.mom_pnl_diff, fc.hac_lag);
            const double p_taker = permutation_pvalue(prim2.mom_pnl_diff,
                                                       res.definition_hash,
                                                       fc.permutation_reps);
            std::snprintf(b, sizeof b,
                          "taker primary-contrast paired PnL diff: %+.1f (se %.1f, "
                          "perm p=%.4f) — %s",
                          mci.mean, mci.se_iid, p_taker,
                          p_taker < 0.05 ? "significant" : "not significant");
            res.findings.push_back(b);
        }
        // verdict against H0 for the primary contrast
        const bool reject = prim.statistically_significant && prim.economically_material;
        if (reject) {
            std::snprintf(b, sizeof b,
                          "H0 (total-only sufficiency) at the primary contrast: REJECTED "
                          "(significant after Holm AND economically material)");
        } else if (prim.economically_material && !prim.statistically_significant) {
            std::snprintf(b, sizeof b,
                          "H0 at the primary contrast: NOT REJECTED — point estimate is "
                          "economically large but cross-seed variance swamps it (low power; "
                          "report as uncertain, do NOT interpret the sign as an effect)");
        } else {
            std::snprintf(b, sizeof b,
                          "H0 at the primary contrast: NOT REJECTED (placement effect small "
                          "and not significant at this budget/regime — a valid negative result)");
        }
        res.findings.push_back(b);
    }
    return res;
}

// ---------------- artifact + report + reproduce ----------------

namespace {

// minimal-scanner helpers (same discipline as experiment.cpp: our own
// format only, anything unexpected is an error, never a guess)
bool scan_key(const std::string& text, const std::string& key, std::string& out) {
    const std::string pat = "\"" + key + "\": \"";
    size_t pos = text.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    const size_t end = text.find('"', pos);
    if (end == std::string::npos) return false;
    out = text.substr(pos, end - pos);
    return true;
}

bool scan_num(const std::string& text, const std::string& key, double& out) {
    const std::string pat = "\"" + key + "\": ";
    size_t pos = text.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    out = std::strtod(text.c_str() + pos, nullptr);
    return true;
}

bool scan_int(const std::string& text, const std::string& key, u64& out) {
    const std::string pat = "\"" + key + "\": ";
    size_t pos = text.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    out = std::strtoull(text.c_str() + pos, nullptr, 10);
    return true;
}

bool read_file(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return !out.empty();
}

bool write_file(const std::string& path, const std::string& content) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool complete = std::fwrite(content.data(), 1, content.size(), file) == content.size();
    return std::fclose(file) == 0 && complete;
}

}  // namespace

std::string flagship_to_json(const FlagshipResult& r) {
    char buf[1024];
    std::string out = "{\n";
    out += "  \"experiment\": \"" + r.cfg.name + "\",\n";
    std::snprintf(buf, sizeof buf, "  \"definition_hash\": %llu,\n  \"total_latency_ns\": %lld,\n",
                  (unsigned long long)r.definition_hash, (long long)r.cfg.total_latency_ns);
    out += buf;
    out += "  \"flow_model\": \"" + r.flow_model_name + "\",\n";
    std::snprintf(buf, sizeof buf,
                  "  \"taker_self_excitation\": %.17g,\n  \"bots_observe_shadow\": %s,\n",
                  r.cfg.taker_self_excitation,
                  r.cfg.bots_observe_shadow ? "true" : "false");
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "  \"horizon_ns\": %lld,\n  \"warmup_ns\": %lld,\n"
                  "  \"economic_threshold\": %.17g,\n",
                  (long long)r.cfg.horizon_ns, (long long)r.cfg.warmup_ns,
                  r.cfg.economic_threshold_ticks_qty_per_lot);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"permutation_reps\": %llu,\n  \"hac_lag\": %llu,\n",
                  (unsigned long long)r.cfg.permutation_reps,
                  (unsigned long long)r.cfg.hac_lag);
    out += buf;
    out += "  \"seeds\": [";
    for (size_t i = 0; i < r.cfg.seeds.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%s%llu", i ? ", " : "", (unsigned long long)r.cfg.seeds[i]);
        out += buf;
    }
    out += "],\n";
    out += "  \"compiler\": \"" + r.compiler + "\",\n  \"platform\": \"" + r.platform + "\",\n";
    out += "  \"created_utc\": \"" + r.created_utc + "\",\n";
    out += "  \"cells\": [\n";
    for (size_t c = 0; c < r.cells.size(); ++c) {
        const CellResult& cell = r.cells[c];
        std::snprintf(buf, sizeof buf,
                      "    {\"name\": \"%s\", \"alloc\": \"%s\", \"md_share\": %.17g,\n"
                      "     \"mean_mm_pnl\": %.17g, \"mean_mom_pnl\": %.17g,\n"
                      "     \"mean_mm_filled_qty\": %.17g, \"mean_mom_filled_qty\": %.17g,\n"
                      "     \"mean_mm_markout\": %.17g, \"mean_mom_markout\": %.17g,\n"
                      "     \"seeds\": [\n",
                      cell.name.c_str(), cell.alloc.to_string().c_str(),
                      r.cfg.md_shares[c], cell.mean_mm_pnl, cell.mean_mom_pnl,
                      cell.mean_mm_filled_qty, cell.mean_mom_filled_qty,
                      cell.mean_mm_markout, cell.mean_mom_markout);
        out += buf;
        for (size_t k = 0; k < cell.per_seed.size(); ++k) {
            const CellSeedOutcome& o = cell.per_seed[k];
            std::snprintf(buf, sizeof buf,
                          "       {\"seed\": %llu, \"mm_pnl\": %lld, \"mom_pnl\": %lld,\n"
                          "        \"mm_fills\": %llu, \"mm_filled_qty\": %llu, \"mom_fills\": %llu,\n"
                          "        \"mm_markout\": %.17g, \"mom_markout\": %.17g,\n"
                          "        \"mm_drawdown\": %lld, \"final_hash\": %llu,\n"
                          "        \"realized_decision_ns\": %.17g, \"realized_order_ns\": %.17g}%s\n",
                          (unsigned long long)r.cfg.seeds[k], (long long)o.mm_marked_pnl,
                          (long long)o.mom_marked_pnl, (unsigned long long)o.mm_fills,
                          (unsigned long long)o.mm_filled_qty, (unsigned long long)o.mom_fills,
                          o.mm_markout_100us, o.mom_markout_100us, (long long)o.mm_max_drawdown,
                          (unsigned long long)o.final_state_hash,
                          o.mean_decision_ns, o.mean_order_transit_ns,
                          k + 1 < cell.per_seed.size() ? "," : "");
            out += buf;
        }
        std::snprintf(buf, sizeof buf, "     ]}%s\n", c + 1 < r.cells.size() ? "," : "");
        out += buf;
    }
    out += "  ],\n  \"pairs\": [\n";
    for (size_t i = 0; i < r.pairs.size(); ++i) {
        const PairComparison& p = r.pairs[i];
        std::snprintf(buf, sizeof buf,
                      "    {\"a\": \"%s\", \"b\": \"%s\",\n"
                      "     \"mean_mm_pnl_diff\": %.17g, \"ci95\": [%.17g, %.17g],\n"
                      "     \"p_perm\": %.17g, \"p_holm\": %.17g,\n"
                      "     \"pnl_diff_per_lot\": %.17g,\n"
                      "     \"significant\": %s, \"material\": %s}%s\n",
                      p.cell_a.c_str(), p.cell_b.c_str(), p.mean_diff, p.ci95_lo, p.ci95_hi,
                      p.p_permutation, p.p_holm, p.pnl_diff_per_lot,
                      p.statistically_significant ? "true" : "false",
                      p.economically_material ? "true" : "false",
                      i + 1 < r.pairs.size() ? "," : "");
        out += buf;
    }
    out += "  ],\n  \"findings\": [\n";
    for (size_t i = 0; i < r.findings.size(); ++i) {
        out += "    \"" + r.findings[i] + "\"";
        if (i + 1 < r.findings.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n}\n";
    return out;
}

std::string flagship_report_md(const FlagshipResult& r) {
    char buf[512];
    std::string out;
    out += "# Flagship: latency-stage non-fungibility (fixed total budget)\n\n";
    out += "**This is a paired counterfactual estimate under the simulator's "
           "structural assumptions — not a causal claim about real markets.**\n\n";
    out += "## Research question\n\nFor a FIXED total trader-side latency budget L = "
           + std::to_string(r.cfg.total_latency_ns) + " ns, does the ALLOCATION of L across "
           "(observation md_transit, decision, order_transit) change execution economics?\n\n";
    out += "H0: only the total matters (E[Y|allocation] = f(L)). H1: placement matters "
           "(stage marginal effects differ).\n\n";
    out += "## Intervention\n\nONE-DIMENSIONAL SLICE of the 3-stage allocation simplex: "
           "the md-share axis {0..1} with the action stages (decision, order_transit) "
           "split evenly — NOT the full simplex (interior decision/order trade-offs are "
           "not explored). Identical total in every cell, enforced by the typed "
           "`LatencyAllocation` invariant; engine-measured realized stage latencies "
           "recorded per cell.\n\n";
    out += "## Market process\n\nflow_model=" + r.flow_model_name;
    if (r.flow_model_name == "clustered") {
        std::snprintf(buf, sizeof buf, " (post-trade intensity boost + trade-sign "
                     "persistence, k=%.2f; a minimal self-exciting mechanism with a "
                     "bit-exact Poisson null — NOT a Hawkes process)",
                     r.cfg.taker_self_excitation);
        out += buf;
    } else {
        out += " (k=0: the memoryless null process, bit-identical to the "
               "historical simulator behavior)";
    }
    out += "\n\n**Regime-contrast scope (limitation):** the two flow regimes "
           "differ in the taker process's dependence structure AND consequently in "
           "realized intensity and trade-sign runs (measured: ~7x trade-count "
           "difference at k=0.9; the momentum archetype only participates when sign "
           "runs emerge, k>=~0.85). The placebo therefore shows \"allocation has no "
           "effect under THIS null market\"; it does NOT isolate which aspect of the "
           "clustered process (dependence vs intensity) would drive any clustered-"
           "regime effect.\n\n";
    out += "## Replication\n\nSeed is the unit of replication (independent worlds; "
           "within-world events are never treated as independent). Cells are PAIRED by "
           "seed (common random numbers): same master seed, same named streams.\n\n";
    out += "## Interference mode\n\n";
    out += r.cfg.bots_observe_shadow
               ? "frozen (bots see a shadow touch excluding strategy orders; bot flow is "
                 "exogenous to the strategies)\n\n"
               : "reactive (bots observe the live touch and react to strategy quotes; the "
                 "measured effect includes market feedback — interference sensitivity is "
                 "probed by re-running frozen)\n\n";

    // grid table
    out += "| cell | allocation | MM PnL (ticks*qty) | MM filled lots | MM markout | "
           "Taker PnL | Taker filled lots |\n";
    out += "|---|---|---|---|---|---|---|\n";
    for (size_t c = 0; c < r.cells.size(); ++c) {
        const CellResult& cell = r.cells[c];
        std::snprintf(buf, sizeof buf, "| %s | %s | %.1f | %.1f | %+.2f | %.1f | %.1f |\n",
                      cell.name.c_str(), cell.alloc.to_string().c_str(), cell.mean_mm_pnl,
                      cell.mean_mm_filled_qty, cell.mean_mm_markout, cell.mean_mom_pnl,
                      cell.mean_mom_filled_qty);
        out += buf;
    }
    out += "\n## Paired contrasts (MM PnL, b − a, per seed)\n\n";
    out += "| contrast | mean diff | 95% CI (iid) | p (perm) | p (Holm) | per-lot | "
           "significant | material |\n|---|---|---|---|---|---|---|---|\n";
    for (const PairComparison& p : r.pairs) {
        std::snprintf(buf, sizeof buf,
                      "| %s → %s | %+.1f | [%+.1f, %+.1f] | %.4f | %.4f | %+.2f | %s | %s |\n",
                      p.cell_a.c_str(), p.cell_b.c_str(), p.mean_diff, p.ci95_lo, p.ci95_hi,
                      p.p_permutation, p.p_holm, p.pnl_diff_per_lot,
                      p.statistically_significant ? "yes" : "no",
                      p.economically_material ? "yes" : "no");
        out += buf;
    }
    out += "\n";
    for (const std::string& f : r.findings) out += "- " + f + "\n";
    if (!r.findings.empty()) out += "\n";
    out += "## Falsification status\n\n";
    out += "- Poisson null: run the same grid with `flow_model=poisson`. Interpretation "
           "scope: if an allocation effect existed in the null market it would indicate "
           "placement sensitivity WITHOUT clustered flow (mechanism contradiction); a "
           "null result here is necessary but NOT sufficient for the clustered-flow "
           "mechanism (the regimes also differ in intensity — see the regime-contrast "
           "scope above).\n";
    out += "- Interference: re-run with `bots_observe_shadow=true`; a large gap between "
           "reactive and frozen effects means the interpretation is unstable.\n";
    out += "- Kill conditions: no adjacent-pair materiality under clustered flow at the "
           "primary contrast; or materiality driven by one seed; or reference/optimized "
           "book disagreement (checked separately in the differential suite).\n\n";
    out += "## Reproducibility\n\n`arbiter flagship --config <file>` re-runs this design; "
           "`arbiter flagship --reproduce <artifact.json>` verifies every recorded hash "
           "bit-for-bit. definition_hash covers the full design (grid, seeds, regime, "
           "horizon, thresholds).\n";
    return out;
}

Status write_flagship_artifacts(const std::string& dir, const FlagshipResult& r) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return fail(Errc::IoError, 0);
    const std::string json_path = dir + "/results.json";
    const std::string report_path = dir + "/report.md";
    const std::string json = flagship_to_json(r);
    if (!write_file(json_path, json)) return fail(Errc::IoError, 1);
    const std::string md = flagship_report_md(r);
    if (!write_file(report_path, md)) return fail(Errc::IoError, 2);
    return good();
}

FlagshipReproReport flagship_reproduce(const std::string& artifact_path) {
    FlagshipReproReport rep;
    rep.detail = "ok";
    std::string text;
    if (!read_file(artifact_path, text)) {
        rep.detail = "cannot read artifact: " + artifact_path;
        return rep;
    }
    std::string name, flow;
    if (!scan_key(text, "experiment", name)) {
        rep.detail = "missing experiment name";
        return rep;
    }
    if (!scan_key(text, "flow_model", flow)) {
        rep.detail = "missing flow_model";
        return rep;
    }
    u64 def_hash = 0;
    if (!scan_int(text, "definition_hash", def_hash)) {
        rep.detail = "missing definition_hash";
        return rep;
    }
    i64 total_ns = 0;
    {
        double v = 0;
        if (!scan_num(text, "total_latency_ns", v)) {
            rep.detail = "missing total_latency_ns";
            return rep;
        }
        total_ns = static_cast<i64>(v);
    }
    double self_exc = 0;
    if (!scan_num(text, "taker_self_excitation", self_exc)) {
        rep.detail = "missing taker_self_excitation";
        return rep;
    }
    bool shadow = text.find("\"bots_observe_shadow\": true") != std::string::npos;
    // seeds array
    std::vector<u64> seeds;
    {
        const size_t pos = text.find("\"seeds\": [");
        if (pos == std::string::npos) {
            rep.detail = "missing seeds";
            return rep;
        }
        size_t p = pos + 10;
        while (p < text.size() && text[p] != ']') {
            if (text[p] >= '0' && text[p] <= '9') {
                u64 v = 0;
                while (p < text.size() && text[p] >= '0' && text[p] <= '9')
                    v = v * 10 + static_cast<u64>(text[p++] - '0');
                seeds.push_back(v);
            } else {
                ++p;
            }
        }
    }
    // cells: (name, md_share, per-seed final_hash, mm_pnl, filled_qty, mom_pnl)
    struct RepCell {
        std::string name;
        double md_share = 0;
        std::vector<u64> hash;
        std::vector<i64> mm_pnl;
        std::vector<i64> mom_pnl;
    };
    std::vector<RepCell> cells;
    {
        size_t pos = text.find("\"cells\": [");
        if (pos == std::string::npos) {
            rep.detail = "missing cells";
            return rep;
        }
        size_t p = pos;
        while (true) {
            p = text.find("{\"name\": \"", p);
            if (p == std::string::npos || p > text.find("\"pairs\"", pos)) break;
            RepCell cell;
            const size_t nend = text.find('"', p + 10);
            cell.name = text.substr(p + 10, nend - (p + 10));
            scan_num(text.substr(p, text.find("]},", p) - p), "md_share", cell.md_share);
            // per-seed records inside this cell
            const size_t cell_end = text.find("]},", p);
            std::string rec = text.substr(p, cell_end == std::string::npos
                                                   ? std::string::npos
                                                   : cell_end - p);
            size_t q = 0;
            while ((q = rec.find("{\"seed\":", q)) != std::string::npos) {
                u64 s = 0;
                if (!scan_int(rec.substr(q), "final_hash", s)) {
                    rep.detail = "cell seed record missing final_hash";
                    return rep;
                }
                cell.hash.push_back(s);
                double mm = 0, mom = 0;
                scan_num(rec.substr(q), "mm_pnl", mm);
                scan_num(rec.substr(q), "mom_pnl", mom);
                cell.mm_pnl.push_back(static_cast<i64>(mm));
                cell.mom_pnl.push_back(static_cast<i64>(mom));
                q += 8;
            }
            cells.push_back(std::move(cell));
            p = cell_end;
            if (p == std::string::npos) break;
            // advance past the seed-array close + cell close
            p = text.find("]},", p);
            if (p == std::string::npos) break;
            p += 3;
        }
    }
    if (cells.empty() || seeds.empty()) {
        rep.detail = "no cells/seeds in artifact";
        return rep;
    }

    // rebuild the exact design from the artifact, every design field is
    // recorded in the artifact; nothing is guessed. A definition-hash
    // mismatch means software/version drift and is reported loudly.
    FlagshipConfig fc;
    fc.name = name;
    fc.total_latency_ns = total_ns;
    fc.flow_model = flow == "clustered" ? FlowModel::Clustered : FlowModel::Poisson;
    fc.taker_self_excitation = self_exc;
    fc.bots_observe_shadow = shadow;
    fc.seeds = seeds;
    fc.md_shares.clear();
    for (const RepCell& c : cells) fc.md_shares.push_back(c.md_share);
    {
        double v = 0;
        if (scan_num(text, "horizon_ns", v)) fc.horizon_ns = static_cast<i64>(v);
        if (scan_num(text, "warmup_ns", v)) fc.warmup_ns = static_cast<i64>(v);
        if (scan_num(text, "economic_threshold", v))
            fc.economic_threshold_ticks_qty_per_lot = v;
        u64 u = 0;
        if (scan_int(text, "permutation_reps", u)) fc.permutation_reps = u;
        if (scan_int(text, "hac_lag", u)) fc.hac_lag = static_cast<size_t>(u);
    }

    const FlagshipResult res = run_flagship(fc);
    if (res.definition_hash != def_hash) {
        rep.detail = "definition_hash mismatch: rebuilt " + std::to_string(res.definition_hash) +
                     " vs artifact " + std::to_string(def_hash) +
                     " (software/version drift? horizon/threshold defaults?)";
        return rep;
    }
    rep.cells_checked = cells.size();
    rep.seeds_checked = seeds.size();
    for (size_t c = 0; c < cells.size(); ++c) {
        for (size_t k = 0; k < seeds.size(); ++k) {
            const CellSeedOutcome& o = res.cells[c].per_seed[k];
            if (o.final_state_hash != cells[c].hash[k] ||
                o.mm_marked_pnl != cells[c].mm_pnl[k] || o.mom_marked_pnl != cells[c].mom_pnl[k])
                ++rep.mismatches;
        }
    }
    rep.ok = rep.mismatches == 0;
    if (!rep.ok)
        rep.detail = "mismatches: " + std::to_string(rep.mismatches);
    return rep;
}

}  // namespace arbiter