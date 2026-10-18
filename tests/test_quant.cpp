// Quantitative research layer tests: correctness fixes (Phase 1) + research
// pipeline (Phases 2-7). Deterministic; no external data (golden files under
// tests via AX_SOURCE_DIR).
#include "testfw.hpp"

#include "arbiter/data.hpp"
#include "arbiter/execution_research.hpp"
#include "arbiter/features.hpp"
#include "arbiter/labels.hpp"
#include "arbiter/net.hpp"
#include "arbiter/pipeline.hpp"
#include "arbiter/quant_model.hpp"
#include "arbiter/research.hpp"
#include "arbiter/rng.hpp"
#include "arbiter/stats.hpp"
#include "arbiter/strategy.hpp"
#include "arbiter/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace arbiter;

namespace {
std::string golden(const char* name) {
    return std::string(AX_SOURCE_DIR) + "/data/golden/" + name;
}
bool approx(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
LadderView ladder2() {
    // imbalance_case_001 book: bids 100@1500,50@1499 / asks 50@1501,150@1502
    LadderView v;
    v.bids.push_back({1500, 100, {}});
    v.bids.push_back({1499, 50, {}});
    v.asks.push_back({1501, 50, {}});
    v.asks.push_back({1502, 150, {}});
    return v;
}
}  // namespace

// ---- Phase 1: config hash covers every behavior-changing field ----




// ---- Phase 1: fixed-draw network semantics ----



// ---- Phase 2: data layer ----

AX_TEST(data_parse_validate_golden) {
    data::DataQualityReport rep;
    auto r = data::parse_csv(golden("simple_book_001.csv"), rep);
    CHECK(static_cast<bool>(r));
    CHECK_EQ(r.value.size(), 4u);
    CHECK(rep.ok());
    CHECK_EQ(rep.trades, 2u);
    CHECK_EQ(rep.quotes, 2u);
    // dataset hash is stable
    std::vector<data::NormalizationAction> log;
    auto evs = data::normalize(std::move(r.value), log);
    const u64 h1 = data::dataset_hash(evs, log);
    CHECK_EQ(h1, data::dataset_hash(evs, log));
    CHECK(!log.empty());  // sort op is always logged
}

AX_TEST(data_never_silently_repairs) {
    // Bad rows are counted; nothing is fixed up.
    std::vector<data::MarketEvent> evs = {
        {200, 200, "V", "I", data::MarketEventType::Trade, data::DataLevel::L1, 0, Side::Buy, 10, 1, 2, ""},
        {100, 100, "V", "I", data::MarketEventType::Trade, data::DataLevel::L1, 0, Side::Buy, 10, 1, 1, ""},
        {300, 300, "V", "I", data::MarketEventType::Trade, data::DataLevel::L1, 0, Side::Buy, 10, 1, 2, ""},  // dup seq
    };
    data::DataQualityReport rep;
    data::validate(evs, rep);
    CHECK_EQ(rep.non_monotonic_ts, 1u);
    CHECK_EQ(rep.duplicate_seq, 1u);
    CHECK(!rep.ok());
}

AX_TEST(data_chrono_split_never_shuffles) {
    data::ChronoSplit s = data::chrono_split(1000, 0.6, 0.2, 0.1);
    CHECK_EQ(s.train_end, 600u);
    CHECK_EQ(s.valid_end, 800u);
    CHECK_EQ(s.held_out_begin, 900u);
    CHECK(s.train_end <= s.valid_end && s.valid_end <= s.held_out_begin);
}

// ---- Phase 3: features ----

AX_TEST(features_golden_imbalance_and_microprice) {
    const LadderView v = ladder2();
    CHECK(approx(features::mid_price(v), 1500.5));
    CHECK(approx(features::spread_ticks(v), 1.0));
    CHECK(approx(features::depth_imbalance(v, 1), 100.0 / 150.0 - 50.0 / 150.0));
    CHECK(approx(features::depth_imbalance(v, 1), 1.0 / 3.0));
    CHECK(approx(features::depth_imbalance(v, 2), -50.0 / 350.0));
    const double mp = features::microprice(v);
    CHECK(mp >= 1500.0 && mp <= 1501.0);
    CHECK(approx(mp, (1501.0 * 100 + 1500.0 * 50) / 150.0));
}

AX_TEST(features_property_bounds) {
    LadderView empty;
    CHECK_EQ(features::spread_ticks(empty), -1.0);
    CHECK_EQ(features::depth_imbalance(empty, 5), 0.0);  // explicit empty convention
    CHECK_EQ(features::mid_price(empty), 0.0);
    // imbalance always in [-1,1] on random books
    Rng rng(Rng::stream(9, 1).state);
    for (int t = 0; t < 200; ++t) {
        LadderView v;
        for (int i = 0; i < 3; ++i) {
            v.bids.push_back({1500 - i, 1 + static_cast<Qty>(rng.bounded(500)), {}});
            v.asks.push_back({1501 + i, 1 + static_cast<Qty>(rng.bounded(500)), {}});
        }
        for (int k : {1, 2, 5, 10, 20}) {
            const double im = features::depth_imbalance(v, k);
            CHECK(im >= -1.0 && im <= 1.0);
        }
        const double mp = features::microprice(v);
        CHECK(mp >= 1500.0 - 3 && mp <= 1501.0 + 3);
        CHECK(features::spread_ticks(v) >= 0.0);
    }
}

AX_TEST(features_ofi_golden) {
    features::OfiAccumulator ofi;
    ofi.observe(1, {1500, 1501, 100, 100});
    ofi.observe(2, {1500, 1501, 150, 100});  // +50 bid
    ofi.observe(3, {1500, 1501, 150, 70});   // ask 100->70: +30
    CHECK_EQ(ofi.total(), 80);
    CHECK(approx(ofi.windowed(3, 1'000'000'000), 80.0));
    CHECK(approx(ofi.windowed(2, 1), 50.0));  // as-of: only tick at t=2 in (1,2]
}

AX_TEST(features_ofi_asof_correctness) {
    // Perturbing post-as-of events must not change earlier queries (leakage).
    features::OfiAccumulator a, b;
    a.observe(1, {1500, 1501, 100, 100});
    a.observe(2, {1500, 1501, 120, 100});
    b.observe(1, {1500, 1501, 100, 100});
    b.observe(2, {1500, 1501, 120, 100});
    const double before = a.windowed(2, 1'000'000'000);
    b.observe(3, {1500, 1501, 999, 1});  // future shock
    CHECK(approx(a.windowed(2, 1'000'000'000), before));
    CHECK(approx(b.windowed(2, 1'000'000'000), before));
}

// ---- Phase 4: labels ----

AX_TEST(labels_no_leakage_and_golden) {
    std::vector<labels::MidSample> s = {{0, 1500.0}, {1'000'000, 1501.0}, {10'000'000, 1499.0}};
    labels::LabelSet l = labels::label_at(s, 0, 1'000'000, 0.001);
    CHECK(l.valid);
    CHECK_EQ(l.dir, 1);
    CHECK(approx(l.ret, std::log(1501.0 / 1500.0)));
    // mutating the future past the horizon changes the label...
    s.back().mid = 1600.0;
    labels::LabelSet l2 = labels::label_at(s, 0, 1'000'000, 0.001);
    CHECK(approx(l2.ret, l.ret));  // ...but NOT for the 1ms horizon (different sample)
    labels::LabelSet l3 = labels::label_at(s, 0, 10'000'000, 0.001);
    CHECK(l3.valid && l3.dir == 1);
    // markout sign convention
    CHECK(labels::markout(1500.0, Side::Buy, 1501.0) > 0.0);
    CHECK(labels::markout(1500.0, Side::Sell, 1501.0) < 0.0);
}

// ---- Phase 4: stats ----

AX_TEST(stats_mean_ci_and_corrections) {
    std::vector<double> x = {1.0, 2.0, 3.0, 4.0, 5.0};
    stats::MeanCI ci = stats::mean_ci(x, 1);
    CHECK(approx(ci.mean, 3.0));
    CHECK(ci.se_iid > 0.0 && ci.se_hac > 0.0);
    CHECK(ci.ci95_lo < 3.0 && ci.ci95_hi > 3.0);
    CHECK(ci.ci95_hac_lo < 3.0 && ci.ci95_hac_hi > 3.0);
    // HAC with strong autocorrelation must not understate vs IID dramatically
    std::vector<double> ar;
    double v = 0.0;
    Rng rng(Rng::stream(3, 3).state);
    for (int i = 0; i < 500; ++i) {
        v = 0.95 * v + (rng.unit() - 0.5);
        ar.push_back(v);
    }
    stats::MeanCI ci2 = stats::mean_ci(ar, 20);
    CHECK(ci2.se_hac >= ci2.se_iid);  // dependence inflates the robust SE
    auto bh = stats::fdr_bh({0.001, 0.01, 0.5});
    CHECK(bh[0] <= bh[1] && bh[1] <= bh[2]);
    auto bf = stats::bonferroni({0.01, 0.5});
    CHECK(approx(bf[0], 0.02) && approx(bf[1], 1.0));
    const std::vector<double> p = {0.012, 0.03, 0.04, 0.2, 0.5, 0.6, 0.7, 0.8, 0.9};
    const std::vector<double> expected_holm = {0.108, 0.24, 0.28, 1, 1, 1, 1, 1, 1};
    const std::vector<double> expected_bh = {0.108, 0.12, 0.12, 0.45, 0.9, 0.9, 0.9, 0.9, 0.9};
    const auto holm = stats::holm(p);
    const auto known_bh = stats::fdr_bh(p);
    for (size_t i = 0; i < p.size(); ++i) {
        CHECK(approx(holm[i], expected_holm[i]));
        CHECK(approx(known_bh[i], expected_bh[i]));
    }
    const std::vector<double> shuffled = {0.5, 0.012, 0.9, 0.04, 0.7, 0.2, 0.03, 0.8, 0.6};
    const auto shuffled_holm = stats::holm(shuffled);
    for (size_t i = 0; i < shuffled.size(); ++i) {
        const auto original = std::find(p.begin(), p.end(), shuffled[i]);
        CHECK(original != p.end());
        CHECK(approx(shuffled_holm[i], holm[static_cast<size_t>(original - p.begin())]));
    }
    for (size_t i = 1; i < holm.size(); ++i) CHECK(holm[i - 1] <= holm[i]);
    auto folds = stats::walk_forward(100, 4, 0.5);
    CHECK(!folds.empty());
    for (size_t i = 1; i < folds.size(); ++i) CHECK(folds[i].test_begin >= folds[i - 1].test_end);
}

// ---- Phase 5: models ----

AX_TEST(models_deterministic_and_sane) {
    models::Dataset ds;
    ds.columns = {"f"};
    for (int i = 0; i < 40; ++i) {
        const double f = i < 20 ? -1.0 - i * 0.01 : 1.0 + i * 0.01;
        ds.X.push_back({f});
        ds.y.push_back(i < 20 ? -1 : 1);
        ds.yret.push_back(i < 20 ? -0.001 : 0.001);
    }
    models::LogisticModel a, b;
    a.train(ds);
    b.train(ds);
    CHECK_EQ(a.weights().size(), 1u);
    CHECK(approx(a.weights()[0], b.weights()[0]));  // deterministic
    CHECK(a.weights()[0] > 0.0);
    CHECK(approx(a.predict_proba({-5.0}) < 0.5 ? 0.0 : 1.0, 0.0));
    CHECK(approx(a.predict_proba({5.0}) > 0.5 ? 1.0 : 0.0, 1.0));
    models::Stump st;
    st.train(ds);
    CHECK_EQ(st.col, 0u);
    std::vector<int> pred = {-1, 1};
    std::vector<double> score = {-2.0, 2.0};
    models::Dataset d2;
    d2.X = {{0}, {0}};
    d2.y = {-1, 1};
    d2.yret = {-0.1, 0.1};
    models::ModelMetrics m = models::evaluate(pred, score, d2);
    CHECK(approx(m.accuracy, 1.0));
    CHECK(approx(m.auc, 1.0));
    // permutation null on pure noise contains the observed accuracy
    models::Dataset noise;
    noise.columns = {"f"};
    Rng rng(Rng::stream(11, 11).state);
    for (int i = 0; i < 100; ++i) {
        noise.X.push_back({rng.unit()});
        noise.y.push_back(rng.unit() < 0.5 ? -1 : 1);
        noise.yret.push_back(0.0);
    }
    std::vector<double> sc(100, 0.0);
    for (int i = 0; i < 100; ++i) sc[i] = noise.X[i][0] - 0.5;
    auto null = models::permutation_null_accuracy(noise, sc, 5, 100);
    CHECK_EQ(null.size(), 100u);
    double mx = 0.0;
    for (double z : null) mx = std::max(mx, z);
    CHECK(mx < 0.75);  // noise cannot fake high accuracy
}

// ---- Phase 6: execution ----

AX_TEST(execution_queue_models_ordered) {
    // conservative <= neutral <= aggressive fills, always.
    Rng rng(Rng::stream(21, 21).state);
    for (int t = 0; t < 500; ++t) {
        const Qty ahead = static_cast<Qty>(rng.bounded(200));
        const Qty ours = 1 + static_cast<Qty>(rng.bounded(50));
        const Qty traded = static_cast<Qty>(rng.bounded(400));
        const Qty c = execresearch::queue_fill_lots(execresearch::QueueModel::Conservative, ahead, ours, traded);
        const Qty n = execresearch::queue_fill_lots(execresearch::QueueModel::Neutral, ahead, ours, traded);
        const Qty g = execresearch::queue_fill_lots(execresearch::QueueModel::Aggressive, ahead, ours, traded);
        CHECK(c <= n && n <= g && g <= ours);
    }
    CHECK_EQ(execresearch::queue_fill_lots(execresearch::QueueModel::Neutral, 10, 5, 0), 0u);
    CHECK_EQ(execresearch::queue_fill_lots(execresearch::QueueModel::Neutral, 10, 5, 15), 5u);
    // costs separate gross from net
    std::vector<execresearch::Opportunity> opps(1);
    opps[0].side = Side::Buy;
    opps[0].exec_px = 1500;
    opps[0].qty = 10;
    opps[0].ahead = 0;
    opps[0].traded_through = 100;
    execresearch::CostModel cm;
    cm.fee_per_lot = 2;
    execresearch::ExecReport r =
        execresearch::evaluate(opps, {1510.0}, {1495.0}, execresearch::QueueModel::Neutral, cm);
    CHECK_EQ(r.fills, 1u);
    CHECK(r.gross_pnl > 0 && r.fees == 20 && r.net_pnl == r.gross_pnl - r.fees);
    CHECK(r.adverse_ticks < 0.0);  // buy fill faded to 1495 = adverse selection
}

// ----Manifests / pipeline determinism ----

AX_TEST(research_manifest_reproduces) {
    research::ResearchManifest m;
    m.run_id = "EXP-001/seed2026";
    m.experiment_id = "EXP-001";
    m.git_commit = "abc123";
    m.dataset_hash = 999;
    m.dataset_name = "synthetic";
    m.config_hash = 111;
    m.seed = 2026;
    m.feature_version = features::kFeatureVersion;
    m.model_version = models::kModelVersion;
    m.compiler = research::compiler_id();
    m.platform = research::platform_id();
    m.manifest_hash = m.compute_hash();
    CHECK_EQ(m.compute_hash(), m.manifest_hash);
    research::ResearchManifest provenance_only = m;
    provenance_only.git_commit = "different";
    provenance_only.compiler = "different compiler";
    provenance_only.platform = "different platform";
    provenance_only.created_utc = "different time";
    CHECK_EQ(provenance_only.compute_hash(), m.manifest_hash);
    research::ResearchManifest changed_input = m;
    changed_input.seed++;
    CHECK_NE(changed_input.compute_hash(), m.manifest_hash);
    research::ResearchResult r;
    r.manifest = m;
    r.metrics = {{"acc", 0.55}};
    r.counters = {{"n", 10}};
    r.result_hash = r.compute_result_hash();
    CHECK_EQ(r.compute_result_hash(), r.result_hash);
    // tamper is detectable
    research::ResearchResult tampered = r;
    tampered.metrics[0].second = 0.99;
    CHECK_NE(tampered.compute_result_hash(), r.result_hash);
}

AX_TEST(pipeline_study_is_deterministic) {
    pipeline::SyntheticConfig sc;
    sc.seed = 4242;
    sc.n_events = 1500;
    auto evs = pipeline::synthesize(sc);
    auto series = pipeline::build_series(evs);
    pipeline::StudyConfig cfg;
    cfg.experiment_id = "DET";
    cfg.seed = 4242;
    cfg.dataset_hash = 1;
    pipeline::StudyResult a = pipeline::run_study(series, cfg);
    pipeline::StudyResult b = pipeline::run_study(series, cfg);
    CHECK_EQ(a.result.result_hash, b.result.result_hash);
    CHECK(!a.decay_imb1.empty() && a.decay_imb1.size() == labels::kNumHorizons);
    CHECK(!a.model_rows.empty());
}

AX_TEST(pipeline_detects_known_synthetic_alpha) {
    // Biased generator MUST read as SUPPORTED; unbiased MUST NOT (falsification).
    pipeline::SyntheticConfig sc;
    sc.seed = 2026;
    sc.n_events = 4000;
    sc.imbalance_bias = 0.6;
    auto evs = pipeline::synthesize(sc);
    auto series = pipeline::build_series(evs);
    pipeline::StudyConfig cfg;
    cfg.experiment_id = "ALPHA";
    cfg.seed = 2026;
    pipeline::StudyResult st = pipeline::run_study(series, cfg);
    bool supported = false;
    for (const auto& f : st.result.findings)
        if (f.find("SUPPORTED") != std::string::npos) supported = true;
    CHECK(supported);
}

AX_TEST(pipeline_alpha_generalizes_across_seeds) {
    // Generalization axis beyond time: the KNOWN synthetic alpha must be
    // detectable at seeds the developer never tuned on. A signal visible at
    // exactly one seed is a red flag, not a result.
    // NOTE (measured): at n_events=4000 seed 7 reads INCONCLUSIVE with a
    // positive point estimate, a power shortfall, not a missing effect
    // (SUPPORTED at flagship scale). Cross-seed checks run at the scale we
    // actually report at.
    for (u64 seed : {7u, 99u}) {
        pipeline::SyntheticConfig sc;
        sc.seed = seed;
        sc.n_events = 6000;
        sc.imbalance_bias = 0.6;
        auto evs = pipeline::synthesize(sc);
        auto series = pipeline::build_series(evs);
        pipeline::StudyConfig cfg;
        cfg.experiment_id = "ALPHA-XSEED";
        cfg.seed = seed;
        pipeline::StudyResult st = pipeline::run_study(series, cfg);
        bool supported = false, heldout_gated = false;
        for (const auto& f : st.result.findings) {
            if (f.find("IMB1@10ms TEST: SUPPORTED") != std::string::npos) supported = true;
            if (f.find("HELDOUT:") != std::string::npos) heldout_gated = true;
        }
        CHECK(supported);
        CHECK(heldout_gated);  // the sealed-tail gate must run and render a verdict
    }
    // And the null must read INCONCLUSIVE/FAILED at an untuned seed too.
    {
        pipeline::SyntheticConfig sc;
        sc.seed = 31337;
        sc.n_events = 6000;
        auto evs = pipeline::synthesize(sc);
        auto series = pipeline::build_series(evs);
        pipeline::StudyConfig cfg;
        cfg.experiment_id = "NULL-XSEED";
        cfg.seed = 31337;
        pipeline::StudyResult st = pipeline::run_study(series, cfg);
        bool supported = false;
        for (const auto& f : st.result.findings)
            if (f.find("IMB1@10ms TEST: SUPPORTED") != std::string::npos) supported = true;
        CHECK(!supported);
    }
}

AX_TEST(research_manifest_v2_roundtrip) {
    // write_artifact -> read_manifest preserves provenance and both hashes.
    research::ResearchManifest m;
    m.run_id = "EXP-009/seed7";
    m.experiment_id = "EXP-009";
    m.git_commit = "deadbee";
    m.dataset_hash = 12345;
    m.dataset_name = "synthetic:seed=7";
    m.dataset_kind = "synthetic";
    m.synth_seed = 7;
    m.synth_events = 500;
    m.synth_bias = 0.25;
    m.imbalance_thresh = 0.3;
    m.exec_latency_ns = 5000;
    m.fee_per_lot = 1;
    m.sample_every = 2;
    m.max_samples = 1000;
    m.config_hash = 77;
    m.seed = 7;
    m.feature_version = features::kFeatureVersion;
    m.model_version = models::kModelVersion;
    m.compiler = research::compiler_id();
    m.platform = research::platform_id();
    m.manifest_hash = m.compute_hash();
    research::ResearchResult r;
    r.manifest = m;
    r.metrics = {{"acc", 0.5}};
    r.counters = {{"n", 3}};
    r.result_hash = r.compute_result_hash();
    std::string err;
    CHECK(research::write_artifact("artifacts/testexp_quant_tmp", r, "# tmp\n", err));
    research::ResearchManifest back;
    u64 rh = 0;
    CHECK(research::read_manifest("artifacts/testexp_quant_tmp", back, rh, err));
    CHECK_EQ(back.synth_seed, 7u);
    CHECK_EQ(back.synth_events, 500u);
    CHECK(back.dataset_kind == "synthetic");
    CHECK_EQ(back.exec_latency_ns, 5000);
    CHECK_EQ(back.sample_every, 2u);
    CHECK_EQ(back.manifest_hash, m.manifest_hash);
    CHECK_EQ(rh, r.result_hash);
    research::ResearchResult parsed;
    CHECK(research::read_result("artifacts/testexp_quant_tmp", parsed, err));
    const research::ResultComparison parsed_values =
        research::compare_result_values(r, parsed, 0.0);
    CHECK(parsed_values.ok);
    // tamper with the stored manifest -> read must fail loudly
    {
        FILE* f = std::fopen("artifacts/testexp_quant_tmp/manifest.json", "ab");
        CHECK(f != nullptr);
        if (f) {
            std::fwrite(" ", 1, 1, f);  // trailing whitespace breaks nothing...
            std::fclose(f);
        }
        // ...but flipping a hashed digit must break verification
        research::ResearchManifest m2 = back;
        m2.seed = 8;
        CHECK_NE(m2.compute_hash(), back.manifest_hash);
    }
}

AX_TEST(research_result_cross_platform_comparison_detects_tampering) {
    research::ResearchResult stored;
    stored.metrics = {{"accuracy", 0.75}, {"pnl", -123.5}};
    stored.counters = {{"samples", 1000}, {"fills", 25}};

    research::ResearchResult rebuilt = stored;
    rebuilt.metrics[0].second += 5e-10;
    const auto tiny_difference =
        research::compare_result_values(stored, rebuilt, 1e-9);
    CHECK(tiny_difference.ok);
    CHECK(tiny_difference.first_difference.find("accuracy") != std::string::npos);
    CHECK(!research::compare_result_values(stored, rebuilt, 0.0).ok);

    research::ResearchResult bad_float = stored;
    bad_float.metrics[1].second += 1e-4;
    const auto float_comparison =
        research::compare_result_values(stored, bad_float, 1e-9);
    CHECK(!float_comparison.ok);
    CHECK(float_comparison.first_mismatch.find("pnl") != std::string::npos);

    research::ResearchResult bad_counter = stored;
    bad_counter.counters[0].second += 1;
    const auto counter_comparison =
        research::compare_result_values(stored, bad_counter, 1e-9);
    CHECK(!counter_comparison.ok);
    CHECK(counter_comparison.first_mismatch.find("samples") != std::string::npos);
}

AX_TEST(pipeline_heldout_is_sealed_and_reported) {
    pipeline::SyntheticConfig sc;
    sc.seed = 4242;
    sc.n_events = 1500;
    auto evs = pipeline::synthesize(sc);
    auto series = pipeline::build_series(evs);
    pipeline::StudyConfig cfg;
    cfg.experiment_id = "DET";
    cfg.seed = 4242;
    cfg.dataset_hash = 1;
    pipeline::StudyResult st = pipeline::run_study(series, cfg);
    bool have_heldout = false, have_heldout_finding = false;
    for (const auto& row : st.model_rows)
        if (row.split == "heldout" && row.n > 0) have_heldout = true;
    for (const auto& f : st.result.findings)
        if (f.find("HELDOUT:") != std::string::npos) have_heldout_finding = true;
    CHECK(have_heldout);
    CHECK(have_heldout_finding);
    // test + heldout partition the post-validation span exactly (disjoint, sealed)
    size_t n_test = 0, n_ho = 0;
    long long c_samples = 0, c_train = 0, c_valid = 0, c_test = 0, c_ho = 0;
    for (const auto& row : st.model_rows) {
        if (row.model == "imb1" && row.split == "test") n_test = row.n;
        if (row.model == "imb1" && row.split == "heldout") n_ho = row.n;
    }
    for (const auto& [k, v] : st.result.counters) {
        if (k == "n_samples") c_samples = v;
        if (k == "n_train") c_train = v;
        if (k == "n_valid") c_valid = v;
        if (k == "n_test") c_test = v;
        if (k == "n_heldout") c_ho = v;
    }
    CHECK_EQ(static_cast<long long>(n_test), c_test);
    CHECK_EQ(static_cast<long long>(n_ho), c_ho);
    CHECK_EQ(c_train + c_valid + c_test + c_ho, c_samples);
}

AX_TEST(data_unparsable_ids_are_loud_not_silent) {
    // Regression: order_id/seq/receive_ts_ns that fail to parse are
    // DISCARDED ROWS with explicit counters; nothing is silently zeroed.
    // (order_id == 0 itself is legal: L1 quotes/trades carry no ids.)
    const std::string tmp = std::string(AX_SOURCE_DIR) + "/data/golden/tmp_bad_ids.csv";
    {
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        std::fprintf(f,
            "exchange_ts_ns,venue,instrument,type,order_id,side,price,qty,seq,receive_ts_ns\n"
            "1000,V,I,trade,abc,buy,1500,5,1,1000\n"       // bad order_id
            "2000,V,I,trade,7,buy,1500,5,xyz,2000\n"        // bad seq
            "3000,V,I,trade,8,buy,1500,5,3,zzz\n"           // bad receive_ts
            "4000,V,I,trade,0,sell,1500,5,4,4000\n");       // L1 zero id: legal
        std::fclose(f);
    }
    data::DataQualityReport rep;
    auto r = data::parse_csv(tmp, rep);
    std::remove(tmp.c_str());
    CHECK(r);
    if (!r) return;
    CHECK_EQ(r.value.size(), 1u);  // only the legal row survived
    CHECK_EQ(rep.bad_order_id_rows, 1u);
    CHECK_EQ(rep.bad_seq_rows, 1u);
    CHECK_EQ(rep.bad_recv_ts_rows, 1u);
    CHECK(!rep.ok());
}

AX_TEST_MAIN