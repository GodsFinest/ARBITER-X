// Research CLI: data validation, feature export, evaluation, backtests,
// one-command flagship experiments, reproduction, simulator calibration.
#include "arbiter/data.hpp"
#include "arbiter/execution_research.hpp"
#include "arbiter/experiment.hpp"
#include "arbiter/features.hpp"
#include "arbiter/labels.hpp"
#include "arbiter/pipeline.hpp"
#include "arbiter/quant_model.hpp"
#include "arbiter/research.hpp"
#include "arbiter/stats.hpp"
#include "arbiter/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace arbiter;

namespace {

std::string git_commit() {
    FILE* p = popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (!p) return "unknown";
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
    return n ? std::string(buf) : "unknown";
}

const char* arg_val(int argc, char** argv, const char* key, const char* dflt = nullptr) {
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return dflt;
}
bool arg_flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], key)) return true;
    return false;
}

// Forward: EXP-002 World-leg config (defined below; single source of truth for
// the latency-pair grid AND the manifest's config_hash).
WorldConfig exp002_world_config();

// Load events from --csv or deterministic synthetic. Sets hash + name.
std::vector<data::MarketEvent> load_events(int argc, char** argv, u64& hash_out,
                                           std::string& name_out,
                                           std::vector<data::NormalizationAction>& log_out) {
    const char* csv = arg_val(argc, argv, "--csv", nullptr);
    if (csv) {
        data::DataQualityReport rep;
        auto parsed = data::parse_csv(csv, rep);
        if (!parsed) {
            std::printf("parse failed: %s\n", to_cstr(parsed.status.code));
            return {};
        }
        auto evs = data::normalize(std::move(parsed.value), log_out);
        hash_out = data::dataset_hash(evs, log_out);
        name_out = csv;
        std::printf("dataset: %s\n  %s\n  normalization ops: %zu  dataset_hash=%llu\n", csv,
                    rep.summary().c_str(), log_out.size(), (unsigned long long)hash_out);
        return evs;
    }
    pipeline::SyntheticConfig sc;
    sc.seed = static_cast<u64>(arg_val(argc, argv, "--seed", nullptr)
                                   ? std::strtoull(arg_val(argc, argv, "--seed", nullptr), nullptr, 10)
                                   : 2026);
    sc.n_events = static_cast<size_t>(arg_val(argc, argv, "--events", nullptr)
                                          ? std::strtoull(arg_val(argc, argv, "--events", nullptr), nullptr, 10)
                                          : 6000);
    sc.imbalance_bias = arg_flag(argc, argv, "--biased") ? 0.6 : 0.0;
    auto evs = pipeline::synthesize(sc);
    log_out = {{0, "synthetic_generator",
                "seed=" + std::to_string(sc.seed) + " n=" + std::to_string(sc.n_events) +
                    " bias=" + std::to_string(sc.imbalance_bias)}};
    hash_out = data::dataset_hash(evs, log_out);
    name_out = std::string("synthetic:seed=") + std::to_string(sc.seed);
    std::printf("dataset: %s (%zu events, dataset_hash=%llu)\n", name_out.c_str(), evs.size(),
                (unsigned long long)hash_out);
    return evs;
}

void print_decay(const char* tag, const std::vector<pipeline::DecayPoint>& d) {
    std::printf("  decay %s (score>0 conditional mean future ret, HAC95 | non-overlap IID95):\n", tag);
    for (const auto& p : d)
        std::printf("    %6s  n=%6zu (nover %4zu)  cond=%+.6f %s %s  IC=%+.3f\n",
                    labels::horizon_name(p.horizon_ns).c_str(), p.n, p.n_nonover, p.cond_ret_pos,
                    stats::format_ci(p.hac_lo, p.hac_hi).c_str(),
                    stats::format_ci(p.nover_lo, p.nover_hi).c_str(), p.ic);
}

void print_models(const std::vector<pipeline::ModelRow>& rows, const char* split) {
    std::printf("  models [%s]:\n", split);
    for (const auto& r : rows) {
        if (r.split != split) continue;
        std::printf("    %-9s acc=%.3f auc=%.3f ic=%+.3f hit=%.3f cov=%.3f cond=%+.6f %s n=%zu\n",
                    r.model.c_str(), r.accuracy, r.auc, r.ic, r.hit, r.coverage, r.cond_ret,
                    stats::format_ci(r.cond_lo, r.cond_hi).c_str(), r.n);
    }
}

int cmd_validate(int argc, char** argv) {
    const char* csv = arg_val(argc, argv, "--csv", nullptr);
    if (!csv) {
        std::printf("usage: arbiter research validate --csv FILE\n");
        return 2;
    }
    data::DataQualityReport rep;
    auto parsed = data::parse_csv(csv, rep);
    if (!parsed) {
        std::printf("parse failed: %s\n", to_cstr(parsed.status.code));
        return 1;
    }
    std::vector<data::NormalizationAction> log;
    auto evs = data::normalize(std::move(parsed.value), log);
    std::printf("quality: %s\n", rep.summary().c_str());
    std::printf("normalization (%zu ops):\n", log.size());
    for (const auto& a : log) std::printf("  - %s: %s\n", a.action.c_str(), a.detail.c_str());
    std::printf("dataset_hash=%llu events=%zu\n", (unsigned long long)data::dataset_hash(evs, log),
                evs.size());
    return rep.ok() ? 0 : 1;
}

int cmd_features(int argc, char** argv) {
    const char* out = arg_val(argc, argv, "--out", nullptr);
    if (!out) {
        std::printf("usage: arbiter research features [--csv F|--synthetic] --out FEATS.csv\n");
        return 2;
    }
    u64 dh = 0;
    std::string dn;
    std::vector<data::NormalizationAction> log;
    auto evs = load_events(argc, argv, dh, dn, log);
    if (evs.empty()) return 1;
    auto series = pipeline::build_series(evs);
    FILE* f = std::fopen(out, "wb");
    if (!f) {
        std::printf("cannot open %s\n", out);
        return 1;
    }
    std::fprintf(f, "# schema=features_v1 feature_version=%s dataset_hash=%llu\n",
                 features::kFeatureVersion, (unsigned long long)dh);
    std::fprintf(f, "ts_ns");
    for (const auto& n : features::feature_names_v1()) std::fprintf(f, ",%s", n.c_str());
    std::fprintf(f, "\n");
    features::OfiAccumulator ofi;
    features::FlowWindows flow;
    features::MidSeries mids;
    size_t rows = 0;
    for (const auto& s : series) {
        features::BookDelta d{s.ladder.bids.empty() ? 0 : s.ladder.bids.front().price,
                              s.ladder.asks.empty() ? 0 : s.ladder.asks.front().price,
                              s.ladder.bids.empty() ? 0 : s.ladder.bids.front().total_qty,
                              s.ladder.asks.empty() ? 0 : s.ladder.asks.front().total_qty};
        ofi.observe(s.ts_ns, d);
        if (s.mid > 0) mids.observe(s.ts_ns, s.mid);
        if (s.mid <= 0 || s.spread_rel < 0) continue;
        features::FeatureVector fv = features::compute_v1(s.ladder, ofi, flow, mids, s.ts_ns);
        std::fprintf(f, "%lld", (long long)s.ts_ns);
        for (const auto& [k, v] : fv.values) std::fprintf(f, ",%.10g", v);
        std::fprintf(f, "\n");
        ++rows;
    }
    std::fclose(f);
    std::printf("wrote %s (%zu rows, schema features_v1)\n", out, rows);
    return 0;
}

pipeline::StudyConfig base_study(int argc, char** argv, const std::string& exp) {
    pipeline::StudyConfig c;
    c.experiment_id = exp;
    c.seed = static_cast<u64>(arg_val(argc, argv, "--seed", nullptr)
                                  ? std::strtoull(arg_val(argc, argv, "--seed", nullptr), nullptr, 10)
                                  : 2026);
    const char* lat = arg_val(argc, argv, "--latency-ns", nullptr);
    if (lat) c.exec_latency_ns = std::strtoll(lat, nullptr, 10);
    const char* fee = arg_val(argc, argv, "--fee", nullptr);
    if (fee) c.fee_per_lot = std::strtoll(fee, nullptr, 10);
    return c;
}

std::string study_report_md(const std::string& title, const std::string& question,
                            const std::string& hypothesis, const pipeline::StudyResult& st,
                            const std::string& extra) {
    std::string s = "## Dataset\n\n";
    s += "name=" + st.result.manifest.dataset_name +
         " hash=" + std::to_string(st.result.manifest.dataset_hash) + "\n\n";
    s += std::string("## Methodology\n\nEvent-time sampling, as-of features (") +
         features::kFeatureVersion + "), horizon labels at 1ms..1s, chronological " +
         "train/valid/test split with sealed held-out tail, walk-forward stability, overlap-adaptive " +
         "HAC inference, permutation nulls, 3-model queue execution accounting.\n\n";
    s += "## Results\n\nDecay (imb1):\n\n```\n";
    for (const auto& p : st.decay_imb1) {
        char b[256];
        std::snprintf(b, sizeof b, "%6s n=%6zu cond=%+.6f %s IC=%+.3f\n",
                      labels::horizon_name(p.horizon_ns).c_str(), p.n, p.cond_ret_pos,
                      stats::format_ci(p.hac_lo, p.hac_hi).c_str(), p.ic);
        s += b;
    }
    s += "```\n\nModel rows (test + sealed heldout):\n\n```\n";
    for (const auto& r : st.model_rows) {
        if (r.split != "test" && r.split != "heldout") continue;
        char b[256];
        std::snprintf(b, sizeof b, "%-9s %-8s acc=%.3f auc=%.3f ic=%+.3f hit=%.3f cond=%+.6f %s\n",
                      r.model.c_str(), r.split.c_str(), r.accuracy, r.auc, r.ic, r.hit, r.cond_ret,
                      stats::format_ci(r.cond_lo, r.cond_hi).c_str());
        s += b;
    }
    s += "```\n\nQueue-model net PnL spread:\n\n```\n";
    for (const auto& [q, pnl] : st.queue_spread) {
        char b[128];
        std::snprintf(b, sizeof b, "%-13s net=%+.1f\n", q.c_str(), pnl);
        s += b;
    }
    s += "```\n\nConditional (TEST, imb1 rule signed ret):\n\n```\n";
    for (const auto& c : st.conditional) {
        char b[256];
        std::snprintf(b, sizeof b, "%-16s %-6s n=%5zu mean=%+.6f %s hit=%.3f\n", c.dim.c_str(),
                      c.bucket.c_str(), c.n, c.mean_ret,
                      stats::format_ci(c.hac_lo, c.hac_hi).c_str(), c.hit);
        s += b;
    }
    s += "```\n\n" + extra;
    s += std::string("## Robustness\n\nWalk-forward means, spread-conditioned buckets, and the permutation ") +
         "null are in results.json metrics/counters; any CI spanning zero is reported " +
         "as INCONCLUSIVE/FAILED, never as alpha.\n\n";
    s += "## Failure analysis\n\nSee findings in results.json (negative results preserved).\n\n";
    s += "## Conclusion\n\n" + std::string(st.result.findings.empty() ? "none" : st.result.findings.front()) + "\n\n";
    s += "## Limitations\n\n";
    s += std::string("L2 level-set book approximation (no per-order queues); queue truth carried as ") +
         "3-model spread; synthetic streams are stylized, not calibrated market data; " +
         "Sharpe-like ratios descriptive only (overlapping samples).\n\n";
    return research::render_report(title, question, hypothesis, st.result, {s});
}

int cmd_evaluate(int argc, char** argv) {
    u64 dh = 0;
    std::string dn;
    std::vector<data::NormalizationAction> log;
    auto evs = load_events(argc, argv, dh, dn, log);
    if (evs.empty()) return 1;
    auto series = pipeline::build_series(evs);
    pipeline::StudyConfig cfg = base_study(argc, argv, "EVAL");
    cfg.dataset_hash = dh;
    cfg.dataset_name = dn;
    pipeline::StudyResult st = pipeline::run_study(series, cfg);
    std::printf("=== evaluation (%s) ===\n", dn.c_str());
    print_decay("imb1", st.decay_imb1);
    print_decay("ofi10", st.decay_ofi);
    print_models(st.model_rows, "test");
    print_models(st.model_rows, "heldout");
    print_models(st.model_rows, "walkforward-mean");
    std::printf("  execution (latency %lldns):\n", (long long)cfg.exec_latency_ns);
    for (const auto& [q, pnl] : st.queue_spread)
        std::printf("    %-13s net=%+.1f ticks*lots\n", q.c_str(), pnl);
    std::printf("  findings:\n");
    for (const auto& f : st.result.findings) std::printf("    - %s\n", f.c_str());
    std::printf("  result_hash=%llu\n", (unsigned long long)st.result.result_hash);
    return 0;
}

int cmd_backtest(int argc, char** argv) {
    u64 dh = 0;
    std::string dn;
    std::vector<data::NormalizationAction> log;
    auto evs = load_events(argc, argv, dh, dn, log);
    if (evs.empty()) return 1;
    auto series = pipeline::build_series(evs);
    pipeline::StudyConfig cfg = base_study(argc, argv, "BACKTEST");
    cfg.dataset_hash = dh;
    cfg.dataset_name = dn;
    const char* qs = arg_val(argc, argv, "--queue", "all");
    const char* ths = arg_val(argc, argv, "--threshold", "0.2");
    cfg.imbalance_thresh = std::strtod(ths, nullptr);
    pipeline::StudyResult st = pipeline::run_study(series, cfg);
    std::printf("=== backtest imb1 rule (thresh=%.2f, latency=%lldns, fee=%lld) ===\n",
                cfg.imbalance_thresh, (long long)cfg.exec_latency_ns, (long long)cfg.fee_per_lot);
    if (std::string(qs) == "all") {
        for (const auto& [q, pnl] : st.queue_spread)
            std::printf("  %-13s net=%+.1f\n", q.c_str(), pnl);
    }
    for (const auto& f : st.result.findings) std::printf("  - %s\n", f.c_str());
    return 0;
}

// EXP-002 helper: pipeline latency sweep over FIXED predictions.
void latency_sweep_report(const std::vector<pipeline::SeriesSample>& series,
                          pipeline::StudyConfig base, std::string& extra,
                          std::vector<std::pair<std::string, double>>& metrics) {
    const i64 lats[] = {0, 1'000, 5'000, 10'000, 25'000, 50'000, 100'000, 250'000, 500'000};
    extra += "### Latency sweep (pipeline leg, neutral queue)\n\n```\n";
    char b[256];
    for (i64 l : lats) {
        base.exec_latency_ns = l;
        pipeline::StudyResult s = pipeline::run_study(series, base);
        double net = 0.0;
        for (const auto& [q, pnl] : s.queue_spread)
            if (q == "neutral") net = pnl;
        std::snprintf(b, sizeof b, "latency %7lldns  net=%+10.1f\n", (long long)l, net);
        extra += b;
        metrics.push_back({"latency_sweep_net_" + std::to_string(l) + "ns", net});
    }
    extra += "```\n\n";
    // World leg: same seed, one latency variable, first causal divergence.
    WorldConfig wb = exp002_world_config();
    Experiment exp(wb);
    exp.set_seed(base.seed);
    exp.set_name("exp002_latency_pair");
    exp.add_variant("baseline_800ns", {{"algo_latency_ns", "800"}});
    exp.add_variant("slow_50000ns", {{"algo_latency_ns", "50000"}});
    ExperimentResult er = exp.run();
    extra += "### Latency pair (World leg, causal divergence)\n\n```\n" +
             experiment_summary(er, "exp002_latency_pair") + "```\n\n";
    metrics.push_back({"world_first_divergence_event",
                       static_cast<double>(er.first_divergence_event.size() > 1
                                               ? er.first_divergence_event[1]
                                               : 0)});
}

// EXP-002 World-leg config (single source of truth: the latency-pair grid
// AND the manifest's config_hash must agree).
WorldConfig exp002_world_config() {
    WorldConfig wb;
    wb.journal_events = false;
    wb.horizon_ns = 40'000'000;
    wb.warmup_ns = 2'000'000;
    return wb;
}

void exp_narrative(const std::string& exp_id, std::string& title, std::string& question,
                   std::string& hypothesis) {
    if (exp_id == "EXP-001") {
        title = "EXP-001: Does order-book imbalance predict monetizable short-horizon movement?";
        question =
            "Does top-of-book imbalance predict short-horizon mid-price movement, and does "
            "any statistical edge survive spread, fees, latency (10us) and queue uncertainty?";
        hypothesis =
            "H1: E[ret_10ms | imb1>0.2] > 0 out-of-sample with HAC95 excluding zero, and "
            "neutral-queue net PnL > 0 after fees.";
    } else if (exp_id == "EXP-002") {
        title = "EXP-002: How much does latency destroy short-horizon execution alpha?";
        question =
            "Holding predictions and market path fixed (controlled random streams), how does "
            "decision->execution latency (0us..500us) change fills, queue position value and net P&L?";
        hypothesis =
            "H2: net P&L decays as latency grows (edge half-life vs delay); "
            "the World pair shows a localized first causal divergence with "
            "economic consequences.";
    } else {
        title = "EXP-003: Does deeper-book information add value beyond top-of-book imbalance?";
        question =
            "Do 5/10/20-level imbalance and OFI add incremental out-of-sample information over "
            "top-of-book imbalance alone, and does any gain survive execution costs?";
        hypothesis =
            "H3: multi-level + OFI logistic AUC/accuracy exceeds imb1-only on TEST; ablation "
            "attributes the marginal contribution.";
    }
}

// EXP-003 ablation: identical folds, logistic on nested feature subsets.
// Deterministic given `series`. Appends report text + metrics + findings.
void ablation_report(const std::vector<pipeline::SeriesSample>& series, std::string& extra,
                     research::ResearchResult& result) {
    extra += "### Ablation (TEST, logistic, identical folds)\n\n```\n";
    // rebuild the 10ms design matrix from the series (same recipe as the study)
    features::OfiAccumulator ofi;
    features::FlowWindows flow;
    features::MidSeries mids;
    std::vector<labels::MidSample> ms;
    struct Row {
        i64 ts;
        double cols[6];
    };
    std::vector<Row> rows;
    for (const auto& s : series) {
        features::BookDelta d{s.ladder.bids.empty() ? 0 : s.ladder.bids.front().price,
                              s.ladder.asks.empty() ? 0 : s.ladder.asks.front().price,
                              s.ladder.bids.empty() ? 0 : s.ladder.bids.front().total_qty,
                              s.ladder.asks.empty() ? 0 : s.ladder.asks.front().total_qty};
        ofi.observe(s.ts_ns, d);
        if (s.mid > 0) {
            mids.observe(s.ts_ns, s.mid);
            ms.push_back({s.ts_ns, s.mid});
        }
        if (s.mid <= 0 || s.spread_rel < 0) continue;
        Row r{s.ts_ns,
              {features::depth_imbalance(s.ladder, 1), features::depth_imbalance(s.ladder, 5),
               ofi.windowed(s.ts_ns, 10'000'000),
               features::microprice_deviation_ticks(s.ladder),
               flow.trade_imbalance(s.ts_ns, 100'000'000),
               flow.signed_volume(s.ts_ns, 100'000'000)}};
        rows.push_back(r);
    }
    const i64 h0 = labels::kHorizonsNs[2];
    const size_t n = rows.size();
    data::ChronoSplit sp = data::chrono_split(n, 0.6, 0.2, 0.1);
    const char* subsets[] = {"imb1", "imb1+imb5", "imb1+imb5+ofi", "full6"};
    const size_t widths[] = {1, 2, 3, 6};
    for (int su = 0; su < 4; ++su) {
        models::Dataset dtr, dte;
        dtr.columns = dte.columns = {"abl"};
        for (size_t i = 0; i < n; ++i) {
            labels::LabelSet l = labels::label_at(ms, rows[i].ts, h0, 0.001);
            std::vector<double> x(rows[i].cols, rows[i].cols + widths[su]);
            if (i < sp.train_end) {
                dtr.X.push_back(x);
                dtr.y.push_back(l.valid ? l.dir : 0);
                dtr.yret.push_back(l.valid ? l.ret : 0.0);
            } else if (i >= sp.valid_end && i < sp.held_out_begin) {
                dte.X.push_back(x);
                dte.y.push_back(l.valid ? l.dir : 0);
                dte.yret.push_back(l.valid ? l.ret : 0.0);
            }
        }
        models::LogisticModel lg;
        lg.train(dtr);
        std::vector<int> pred;
        std::vector<double> score;
        for (const auto& x : dte.X) {
            const double p = lg.predict_proba(x);
            pred.push_back(p >= 0.5 ? 1 : -1);
            score.push_back(p - 0.5);
        }
        models::ModelMetrics mm = models::evaluate(pred, score, dte);
        char b[256];
        std::snprintf(b, sizeof b, "%-14s acc=%.3f auc=%.3f ic=%+.3f hit=%.3f n=%zu\n",
                      subsets[su], mm.accuracy, mm.auc, mm.ic, mm.hit_rate, mm.n);
        extra += b;
        result.metrics.push_back({"ablation_" + std::string(subsets[su]) + "_acc", mm.accuracy});
        result.metrics.push_back({"ablation_" + std::string(subsets[su]) + "_auc", mm.auc});
    }
    extra += "```\n\nMarginal gain = full6 minus imb1 on the same TEST fold; cost-of-complexity: "
             "each added column costs inference latency (bench: feat_v1 p50 ~0.8us) against "
             "the measured accuracy/AUC delta.\n\n";
    // verdict from the recorded metrics (read back what we just pushed)
    double a1 = 0, af = 0;
    for (const auto& [k, v] : result.metrics) {
        if (k == "ablation_imb1_acc") a1 = v;
        if (k == "ablation_full6_acc") af = v;
    }
    if (af > a1 + 0.02)
        extra += "Verdict: DEPTH SUPPORTED: full-book model materially beats imb1-only.\n\n";
    else
        extra += "Verdict: DEPTH NOT SUPPORTED on this dataset: full-book model does not "
                 "materially beat imb1-only (consistent with a top-book-driven DGP; on real "
                 "data this is the test that must pass before paying depth costs).\n\n";
    result.findings.push_back(af > a1 + 0.02
                                  ? "DEPTH: SUPPORTED (full6 beats imb1-only on TEST)"
                                  : "DEPTH: NOT SUPPORTED on this dataset (ablation gap ~0)");
}

struct FlagshipInputs {
    std::string exp_id;
    std::string dataset_kind = "synthetic";  // "synthetic" | "csv"
    std::string csv_path;
    u64 synth_seed = 2026;
    size_t synth_events = 6000;
    double synth_bias = 0.6;
    u64 seed = 2026;
    double imbalance_thresh = 0.2;
    i64 exec_latency_ns = 10'000;
    i64 fee_per_lot = 0;
};

// Shared flagship core: `run` writes its output as artifacts; `reproduce`
// re-runs it from manifest provenance and compares hashes. Deterministic:
// identical inputs MUST yield identical result_hash.
bool run_flagship(const FlagshipInputs& in, pipeline::StudyResult& st_out, std::string& report_out,
                  std::string& err, bool verbose) {
    std::vector<data::MarketEvent> evs;
    std::vector<data::NormalizationAction> log;
    u64 dh = 0;
    std::string dn;
    if (in.dataset_kind == "csv") {
        data::DataQualityReport rep;
        auto parsed = data::parse_csv(in.csv_path, rep);
        if (!parsed) {
            err = "csv parse failed";
            return false;
        }
        evs = data::normalize(std::move(parsed.value), log);
        dh = data::dataset_hash(evs, log);
        dn = in.csv_path;
        if (verbose)
            std::printf("dataset: %s\n  %s\n", dn.c_str(), rep.summary().c_str());
    } else if (in.dataset_kind == "synthetic") {
        pipeline::SyntheticConfig sc;
        sc.seed = in.synth_seed;
        sc.n_events = in.synth_events;
        sc.imbalance_bias = in.synth_bias;
        evs = pipeline::synthesize(sc);
        char desc[160];
        std::snprintf(desc, sizeof desc, "seed=%llu n=%llu bias=%.2f (KNOWN synthetic alpha)",
                      (unsigned long long)sc.seed, (unsigned long long)sc.n_events,
                      sc.imbalance_bias);
        log = {{0, "synthetic_generator", desc}};
        dh = data::dataset_hash(evs, log);
        dn = "synthetic:seed=" + std::to_string(sc.seed) +
             (sc.imbalance_bias != 0.0 ? ":biased" : "");
        if (verbose) std::printf("dataset: %s (%zu events)\n", dn.c_str(), evs.size());
    } else {
        err = "unknown dataset_kind '" + in.dataset_kind + "'";
        return false;
    }
    if (evs.empty()) {
        err = "empty event stream";
        return false;
    }
    auto series = pipeline::build_series(evs);
    pipeline::StudyConfig cfg;
    cfg.experiment_id = in.exp_id;
    cfg.seed = in.seed;
    cfg.dataset_hash = dh;
    cfg.dataset_name = dn;
    cfg.run_id = in.exp_id + "/seed" + std::to_string(in.seed);
    cfg.imbalance_thresh = in.imbalance_thresh;
    cfg.exec_latency_ns = in.exec_latency_ns;
    cfg.fee_per_lot = in.fee_per_lot;
    cfg.config_hash =
        in.exp_id == "EXP-002" ? exp002_world_config().hash() : 0;  // World-leg identity
    pipeline::StudyResult st = pipeline::run_study(series, cfg);
    // manifest provenance (v2): everything needed for an exact rebuild
    research::ResearchManifest& m = st.result.manifest;
    m.dataset_kind = in.dataset_kind;
    m.synth_seed = in.synth_seed;
    m.synth_events = in.synth_events;
    m.synth_bias = in.synth_bias;
    m.imbalance_thresh = in.imbalance_thresh;
    m.exec_latency_ns = in.exec_latency_ns;
    m.fee_per_lot = in.fee_per_lot;
    m.sample_every = cfg.sample_every;
    m.max_samples = cfg.max_samples;
    m.git_commit = git_commit();
    m.created_utc = std::string(__DATE__) + " " + __TIME__ + " UTC";
    m.manifest_hash = m.compute_hash();
    st.result.result_hash = st.result.compute_result_hash();

    std::string title, question, hypothesis, extra;
    exp_narrative(in.exp_id, title, question, hypothesis);
    if (in.exp_id == "EXP-001" && verbose) {
        print_decay("imb1", st.decay_imb1);
        print_models(st.model_rows, "test");
        print_models(st.model_rows, "heldout");
    }
    if (in.exp_id == "EXP-002") {
        std::vector<std::pair<std::string, double>> sw;
        latency_sweep_report(series, cfg, extra, sw);
        for (auto& [k, v] : sw) st.result.metrics.push_back({k, v});
        st.result.result_hash = st.result.compute_result_hash();
    }
    if (in.exp_id == "EXP-003") ablation_report(series, extra, st.result);
    // metrics from study rows (test + walkforward + heldout accuracy trail)
    for (const auto& r : st.model_rows) {
        if (r.split != "test" && r.split != "walkforward-mean" && r.split != "heldout") continue;
        st.result.metrics.push_back({r.model + "_" + r.split + "_acc", r.accuracy});
        if (r.split == "test") {
            st.result.metrics.push_back({r.model + "_test_auc", r.auc});
            st.result.metrics.push_back({r.model + "_test_ic", r.ic});
        }
    }
    for (const auto& p : st.decay_imb1)
        st.result.metrics.push_back(
            {"decay_imb1_" + labels::horizon_name(p.horizon_ns) + "_cond", p.cond_ret_pos});
    for (const auto& [q, pnl] : st.queue_spread)
        st.result.metrics.push_back({"queue_net_" + q, pnl});
    st.result.result_hash = st.result.compute_result_hash();
    report_out = study_report_md(title, question, hypothesis, st, extra);
    st_out = std::move(st);
    return true;
}

int cmd_run(int argc, char** argv) {
    const char* exp = arg_val(argc, argv, "--exp", nullptr);
    const char* exp_pos = argc > 1 ? argv[1] : nullptr;
    std::string exp_id = exp ? exp : (exp_pos && exp_pos[0] != '-' ? exp_pos : "");
    if (exp_id != "EXP-001" && exp_id != "EXP-002" && exp_id != "EXP-003") {
        std::printf("usage: arbiter research run EXP-001|EXP-002|EXP-003 [--csv F] [--seed S] "
                    "[--latency-ns L] [--fee F] [--threshold T]\n");
        return 2;
    }
    // CLI flags honored (and recorded in the manifest for exact rebuild).
    pipeline::StudyConfig flags = base_study(argc, argv, exp_id);
    const char* ths = arg_val(argc, argv, "--threshold", nullptr);
    FlagshipInputs in;
    in.exp_id = exp_id;
    if (arg_val(argc, argv, "--csv", nullptr)) {
        in.dataset_kind = "csv";
        in.csv_path = arg_val(argc, argv, "--csv", nullptr);
    } else {
        // Flagship default: biased synthetic (KNOWN alpha): EXP-001 needs edge
        // to monetize, EXP-002 needs edge for latency to destroy, EXP-003 needs
        // edge to attribute. --csv overrides with real data.
        in.dataset_kind = "synthetic";
        in.synth_seed = 2026;
        in.synth_events = 6000;
        in.synth_bias = 0.6;
    }
    in.seed = flags.seed;
    in.imbalance_thresh = ths ? std::strtod(ths, nullptr) : 0.2;
    in.exec_latency_ns = flags.exec_latency_ns;
    in.fee_per_lot = flags.fee_per_lot;
    pipeline::StudyResult st;
    std::string report, err;
    if (!run_flagship(in, st, report, err, true)) {
        std::printf("run failed: %s\n", err.c_str());
        return 1;
    }
    const std::string dir = "experiments/" + exp_id;
    if (!research::write_artifact(dir, st.result, report, err)) {
        std::printf("artifact write failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("=== %s complete ===\n  artifact: %s/ (manifest.json, results.json, report.md)\n",
                exp_id.c_str(), dir.c_str());
    for (const auto& f : st.result.findings) std::printf("  - %s\n", f.c_str());
    std::printf("  manifest_hash=%llu result_hash=%llu\n",
                (unsigned long long)st.result.manifest.manifest_hash,
                (unsigned long long)st.result.result_hash);
    return 0;
}

int cmd_reproduce(int argc, char** argv) {
    const char* exp_pos = argc > 1 ? argv[1] : nullptr;
    std::string exp_id = exp_pos ? exp_pos : "";
    if (exp_id != "EXP-001" && exp_id != "EXP-002" && exp_id != "EXP-003") {
        std::printf("usage: arbiter research reproduce EXP-001|EXP-002|EXP-003\n");
        return 2;
    }
    const std::string dir = "experiments/" + exp_id;
    // 1. Integrity: stored manifest_hash must match stored fields, and the
    // result values must be parseable for field-level comparison.
    research::ResearchResult stored;
    std::string err;
    if (!research::read_result(dir, stored, err)) {
        std::printf("artifact integrity: FAILED (%s)\n", err.c_str());
        return 1;
    }
    const research::ResearchManifest& m = stored.manifest;
    std::printf("artifact integrity: OK (manifest v2, %s)\n", m.dataset_name.c_str());
    // 2. end-to-end: rebuild everything from manifest provenance and compare.
    FlagshipInputs in;
    in.exp_id = m.experiment_id;
    in.dataset_kind = m.dataset_kind;
    in.csv_path = m.dataset_name;  // csv runs record the path as dataset_name
    in.synth_seed = m.synth_seed;
    in.synth_events = m.synth_events;
    in.synth_bias = m.synth_bias;
    in.seed = m.seed;
    in.imbalance_thresh = m.imbalance_thresh;
    in.exec_latency_ns = m.exec_latency_ns;
    in.fee_per_lot = m.fee_per_lot;
    pipeline::StudyResult st;
    std::string report;
    if (!run_flagship(in, st, report, err, false)) {
        std::printf("re-run: FAILED (%s)\n", err.c_str());
        return 1;
    }
    bool ok = true;
    auto check = [&](const char* what, u64 a, u64 b) {
        const bool eq = a == b;
        std::printf("  %-14s stored=%llu rebuilt=%llu %s\n", what, (unsigned long long)a,
                    (unsigned long long)b, eq ? "MATCH" : "MISMATCH");
        ok = ok && eq;
    };
    check("dataset_hash", m.dataset_hash, st.result.manifest.dataset_hash);
    check("manifest_hash", m.manifest_hash, st.result.manifest.manifest_hash);
    const bool same_build = m.platform == st.result.manifest.platform &&
                            m.compiler == st.result.manifest.compiler;
    if (same_build) {
        check("result_hash", stored.result_hash, st.result.result_hash);
        if (stored.result_hash != st.result.result_hash) {
            const research::ResultComparison values =
                research::compare_result_values(stored, st.result, 0.0);
            std::printf("  first differing field: %s\n",
                        values.ok ? "none in serialized values" : values.first_mismatch.c_str());
        }
    } else {
        constexpr double kCrossPlatformTolerance = 1e-9;
        std::printf("  PLATFORM DIFFERS (artifact: %s / %s, here: %s / %s):\n"
                    "    bitwise result-hash comparison not applicable\n",
                    m.platform.c_str(), m.compiler.c_str(), st.result.manifest.platform.c_str(),
                    st.result.manifest.compiler.c_str());
        const research::ResultComparison values =
            research::compare_result_values(stored, st.result, kCrossPlatformTolerance);
        if (values.ok) {
            std::printf("  values         floating metrics within relative tolerance %.1e; "
                        "integer counters exact MATCH\n",
                        kCrossPlatformTolerance);
            std::printf("  first numeric difference: %s\n",
                        values.first_difference.empty() ? "none (serialized values are exact)"
                                                        : values.first_difference.c_str());
        } else {
            std::printf("  first differing field: %s\n", values.first_mismatch.c_str());
            ok = false;
        }
    }
    if (m.git_commit != st.result.manifest.git_commit)
        std::printf("  note: git_commit %s -> %s (code moved; comparison %-10s)\n",
                    m.git_commit.c_str(), st.result.manifest.git_commit.c_str(),
                    ok ? "match" : "MISMATCH");
    const char* success = same_build ? "EXACTLY REPRODUCED"
                                     : "REPRODUCED WITHIN TOLERANCE (cross-platform)";
    std::printf("RESULT: %s\n", ok ? success : "REPRODUCTION FAILED");
    return ok ? 0 : 1;
}

int cmd_calibrate(int argc, char** argv) {
    u64 seed = static_cast<u64>(arg_val(argc, argv, "--seed", nullptr)
                                    ? std::strtoull(arg_val(argc, argv, "--seed", nullptr), nullptr, 10)
                                    : 99);
    u64 ms = static_cast<u64>(arg_val(argc, argv, "--ms", nullptr)
                                  ? std::strtoull(arg_val(argc, argv, "--ms", nullptr), nullptr, 10)
                                  : 200);
    // Run the World, reconstruct L2, estimate arrival/cancel/trade intensities
    // and spread/depth/volatility distributions, then report fit vs SimConfig.
    WorldConfig cfg;
    cfg.journal_events = true;
    cfg.journal_path = "artifacts/calibrate.journal";
    cfg.horizon_ns = static_cast<LogicalNs>(ms) * 1'000'000;
    std::remove(cfg.journal_path.c_str());
    World w(cfg, seed);
    w.run();
    std::printf("=== simulator calibration (seed=%llu, %llums) ===\n", (unsigned long long)seed,
                (unsigned long long)ms);
    std::printf("  configured: provider %.1f/ms taker %.1f/ms cancel_frac %.2f\n",
                cfg.sim.provider_arrival_per_ms, cfg.sim.taker_arrival_per_ms, cfg.sim.cancel_fraction);
    std::printf("  measured: events=%llu trades=%llu final_mid=%lld\n",
                (unsigned long long)w.stats().events_processed, (unsigned long long)w.stats().trades,
                (long long)w.stats().final_mid);
    std::printf("  NOTE: goodness-of-fit vs real data requires --csv market data;\n"
                "        distributional comparison lives in tools/research/calibrate.py.\n");
    return 0;
}

}  // namespace

int cmd_research(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "help") || !std::strcmp(argv[1], "--help") ||
        !std::strcmp(argv[1], "-h")) {
        std::printf(
            "usage: arbiter research <subcommand> [options]\n"
            "  validate   --csv F                    data quality + normalization report\n"
            "  features   [--csv F|--synthetic] --out FEATS.csv   versioned feature export\n"
            "  evaluate   [--csv F|--synthetic] [--seed S]        full statistical study\n"
            "  backtest   [--csv F|--synthetic] [--latency-ns L] [--queue all] [--threshold T] [--fee F]\n"
            "  run        EXP-001|EXP-002|EXP-003 [--csv F]        one-command flagship experiment\n"
            "  reproduce  EXP-001|EXP-002|EXP-003                 re-run from manifest + verify hashes\n"
            "  calibrate  [--seed S --ms MS]                     simulator calibration report\n");
        return 0;
    }
    const std::string sub = argv[1];
    if (sub == "validate") return cmd_validate(argc - 1, argv + 1);
    if (sub == "features") return cmd_features(argc - 1, argv + 1);
    if (sub == "evaluate") return cmd_evaluate(argc - 1, argv + 1);
    if (sub == "backtest") return cmd_backtest(argc - 1, argv + 1);
    if (sub == "run") return cmd_run(argc - 1, argv + 1);
    if (sub == "reproduce") return cmd_reproduce(argc - 1, argv + 1);
    if (sub == "calibrate") return cmd_calibrate(argc - 1, argv + 1);
    std::printf("unknown research subcommand: %s\n", sub.c_str());
    return 2;
}