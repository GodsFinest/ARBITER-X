// Research pipeline: series building, synthetic generator, full study.
#include "arbiter/pipeline.hpp"
#include "arbiter/quant_model.hpp"
#include "arbiter/rng.hpp"
#include "arbiter/stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

namespace arbiter {
namespace pipeline {

std::vector<SeriesSample> build_series(const std::vector<data::MarketEvent>& events) {
    // price -> qty per side (L2 level-set semantics)
    std::map<Price, Qty, std::greater<Price>> bids;
    std::map<Price, Qty> asks;
    std::map<u64, std::pair<Side, Price>> id_loc;  // L3 id -> (side, price)
    std::map<u64, Qty> id_qty;
    std::vector<SeriesSample> out;
    // Crossing quotes would execute against each other on a real venue, so
    // reconstruction clears crossed opposite-side levels on every insertion
    // (this also retires stale levels when the touch migrates). Zero-qty
    // SET rows remove the level (standard L2 snapshot convention).
    auto clear_crossed = [&](Side s, Price p) {
        if (is_buy(s)) {
            for (auto it = asks.begin(); it != asks.end();) {
                if (it->first > p) break;
                for (auto it2 = id_loc.begin(); it2 != id_loc.end();) {
                    if (!is_buy(it2->second.first) && it2->second.second == it->first) {
                        id_qty.erase(it2->first);
                        it2 = id_loc.erase(it2);
                    } else {
                        ++it2;
                    }
                }
                it = asks.erase(it);
            }
        } else {
            for (auto it = bids.begin(); it != bids.end();) {
                if (it->first < p) break;
                for (auto it2 = id_loc.begin(); it2 != id_loc.end();) {
                    if (is_buy(it2->second.first) && it2->second.second == it->first) {
                        id_qty.erase(it2->first);
                        it2 = id_loc.erase(it2);
                    } else {
                        ++it2;
                    }
                }
                it = bids.erase(it);
            }
        }
    };
    for (const auto& e : events) {
        const bool is_bid = is_buy(e.side);
        switch (e.type) {
            case data::MarketEventType::DepthL2:
            case data::MarketEventType::QuoteL1: {
                // Absolute level snapshot: the row STATES the current size.
                if (e.qty == 0) {
                    if (is_bid)
                        bids.erase(e.price);
                    else
                        asks.erase(e.price);
                } else {
                    clear_crossed(e.side, e.price);
                    if (is_bid)
                        bids[e.price] = e.qty;
                    else
                        asks[e.price] = e.qty;
                }
                break;
            }
            case data::MarketEventType::Add: {
                if (e.level == data::DataLevel::L3 && e.order_id != 0) {
                    id_loc[e.order_id] = {e.side, e.price};
                    id_qty[e.order_id] = e.qty;
                }
                // Incremental feed: the row ADDS liquidity.
                clear_crossed(e.side, e.price);
                if (is_bid)
                    bids[e.price] += e.qty;
                else
                    asks[e.price] += e.qty;
                break;
            }
            case data::MarketEventType::Cancel: {
                if (e.level == data::DataLevel::L3 && e.order_id != 0) {
                    auto it = id_loc.find(e.order_id);
                    if (it != id_loc.end()) {
                        const auto [s, p] = it->second;
                        const Qty q = id_qty[e.order_id];
                        if (is_buy(s)) {
                            auto b = bids.find(p);
                            if (b != bids.end()) b->second = b->second > q ? b->second - q : 0;
                        } else {
                            auto a = asks.find(p);
                            if (a != asks.end()) a->second = a->second > q ? a->second - q : 0;
                        }
                        id_loc.erase(it);
                        id_qty.erase(e.order_id);
                    }
                } else {
                    // L2 cancel: subtract at the level, floored at 0
                    if (is_bid) {
                        auto b = bids.find(e.price);
                        if (b != bids.end()) b->second = b->second > e.qty ? b->second - e.qty : 0;
                    } else {
                        auto a = asks.find(e.price);
                        if (a != asks.end()) a->second = a->second > e.qty ? a->second - e.qty : 0;
                    }
                }
                break;
            }
            case data::MarketEventType::Trade:
            case data::MarketEventType::Execution: {
                // consume liquidity at the traded price on the opposite side
                // of the aggressor (buy aggressor lifts the ask)
                if (is_bid) {
                    auto a = asks.find(e.price);
                    if (a != asks.end()) a->second = a->second > e.qty ? a->second - e.qty : 0;
                } else {
                    auto b = bids.find(e.price);
                    if (b != bids.end()) b->second = b->second > e.qty ? b->second - e.qty : 0;
                }
                break;
            }
        }
        // prune empty levels, retain top 20
        for (auto it = bids.begin(); it != bids.end();) {
            if (it->second == 0) it = bids.erase(it);
            else ++it;
        }
        for (auto it = asks.begin(); it != asks.end();) {
            if (it->second == 0) it = asks.erase(it);
            else ++it;
        }
        SeriesSample s;
        s.ts_ns = e.exchange_ts_ns;
        size_t k = 0;
        for (const auto& [p, q] : bids) {
            if (k++ >= 20 || q == 0) continue;
            s.ladder.bids.push_back({p, q, {}});
        }
        k = 0;
        for (const auto& [p, q] : asks) {
            if (k++ >= 20 || q == 0) continue;
            s.ladder.asks.push_back({p, q, {}});
        }
        s.mid = features::mid_price(s.ladder);
        s.spread_rel = features::relative_spread(s.ladder);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<data::MarketEvent> synthesize(SyntheticConfig cfg) {
    std::vector<data::MarketEvent> evs;
    evs.reserve(cfg.n_events);
    Rng depth_rng(Rng::stream(cfg.seed, 0xD3701ull).state);
    Rng trade_rng(Rng::stream(cfg.seed, 0x7ADE02ull).state);
    Rng px_rng(Rng::stream(cfg.seed, 0xB10D03ull).state);
    double mid = static_cast<double>(cfg.start_mid);
    i64 ts = 0;
    u64 seq = 1;
    for (size_t i = 0; i < cfg.n_events; ++i) {
        ts += cfg.dt_ns;
        // depth snapshot pair around the mid (L2 absolute levels)
        const Qty bq = 50 + static_cast<Qty>(depth_rng.bounded(150)) +
                       (cfg.imbalance_bias > 0 ? static_cast<Qty>(cfg.imbalance_bias * 100) : 0);
        const Qty aq = 50 + static_cast<Qty>(depth_rng.bounded(150)) +
                       (cfg.imbalance_bias < 0 ? static_cast<Qty>(-cfg.imbalance_bias * 100) : 0);
        const Price bb = static_cast<Price>(std::llround(mid)) - 1;
        const Price ba = static_cast<Price>(std::llround(mid)) + 1;
        data::MarketEvent d1, d2;
        d1.exchange_ts_ns = d2.exchange_ts_ns = ts;
        d1.receive_ts_ns = d2.receive_ts_ns = ts;
        d1.venue = d2.venue = "SYNTH";
        d1.instrument = d2.instrument = "SYNTH-1";
        d1.type = d2.type = data::MarketEventType::DepthL2;
        d1.level = d2.level = data::DataLevel::L2;
        d1.side = Side::Buy;
        d2.side = Side::Sell;
        d1.price = bb > 0 ? bb : 1;
        d2.price = ba;
        d1.qty = bq;
        d2.qty = aq;
        d1.seq = seq++;
        d2.seq = seq++;
        d1.session = d2.session = "SYNTH";
        evs.push_back(d1);
        evs.push_back(d2);
        // trade: direction coupled to imbalance when bias is on (KNOWN alpha)
        const double imb = (static_cast<double>(bq) - aq) / (static_cast<double>(bq) + aq);
        double p_up = 0.5 + imb * 0.35 * (cfg.imbalance_bias != 0.0 ? 1.0 : 0.0);
        if (p_up < 0.05) p_up = 0.05;
        if (p_up > 0.95) p_up = 0.95;
        const bool up = trade_rng.unit() < p_up;
        mid += (up ? 1.0 : -1.0) * (px_rng.unit() < 0.3 ? 1.0 : 0.0) + cfg.drift_per_event;
        if (mid < 100) mid = 100;
        if (i % 2 == 0) {
            data::MarketEvent t;
            t.exchange_ts_ns = ts + 1;
            t.receive_ts_ns = ts + 1;
            t.venue = "SYNTH";
            t.instrument = "SYNTH-1";
            t.type = data::MarketEventType::Trade;
            t.level = data::DataLevel::L1;
            t.side = up ? Side::Buy : Side::Sell;
            t.price = up ? ba : bb;
            t.qty = 1 + static_cast<Qty>(trade_rng.bounded(10));
            t.seq = seq++;
            t.session = "SYNTH";
            evs.push_back(t);
        }
    }
    return evs;
}

namespace {
// HAC lag from overlap: consecutive event-time samples share (horizon-dt) of
// their label windows, so dependence extends ~horizon/dt samples. Clamp to
// [10, 500] (compute guard). DOCUMENTED choice, printed in reports.
size_t hac_lag_for(i64 horizon_ns, i64 median_dt_ns) {
    if (median_dt_ns <= 0) median_dt_ns = 500'000;
    const long long lag = horizon_ns / median_dt_ns;
    if (lag < 10) return 10;
    if (lag > 500) return 500;
    return static_cast<size_t>(lag);
}
}  // namespace

StudyResult run_study(const std::vector<SeriesSample>& series, const StudyConfig& cfg) {
    StudyResult out;
    // ---- 1. as-of feature pass (event-time, thinned) ----
    features::OfiAccumulator ofi;
    features::FlowWindows flow;
    features::MidSeries mids;
    std::vector<labels::MidSample> mid_series;
    mid_series.reserve(series.size());
    struct Feat {
        i64 ts;
        double imb1, imb5, ofi10, micro, spread_rel, signed_vol, trade_imb, realvol, mid;
    };
    std::vector<Feat> feats;
    feats.reserve(series.size());
    size_t si = 0;
    for (const auto& s : series) {
        features::BookDelta d;
        d.bid_px = s.ladder.bids.empty() ? 0 : s.ladder.bids.front().price;
        d.ask_px = s.ladder.asks.empty() ? 0 : s.ladder.asks.front().price;
        d.bid_qty = s.ladder.bids.empty() ? 0 : s.ladder.bids.front().total_qty;
        d.ask_qty = s.ladder.asks.empty() ? 0 : s.ladder.asks.front().total_qty;
        ofi.observe(s.ts_ns, d);
        if (s.mid > 0.0) {
            mids.observe(s.ts_ns, s.mid);
            mid_series.push_back({s.ts_ns, s.mid});
        }
        if (si++ % (cfg.sample_every == 0 ? 1 : cfg.sample_every) != 0) continue;
        if (feats.size() >= cfg.max_samples) break;
        if (s.mid <= 0.0 || s.spread_rel < 0.0) continue;
        Feat f;
        f.ts = s.ts_ns;
        f.imb1 = features::depth_imbalance(s.ladder, 1);
        f.imb5 = features::depth_imbalance(s.ladder, 5);
        f.ofi10 = ofi.windowed(s.ts_ns, 10'000'000);
        f.micro = features::microprice_deviation_ticks(s.ladder);
        f.spread_rel = s.spread_rel;
        f.signed_vol = flow.signed_volume(s.ts_ns, 100'000'000);
        f.trade_imb = flow.trade_imbalance(s.ts_ns, 100'000'000);
        f.realvol = mids.realized_vol(s.ts_ns, 100'000'000);
        f.mid = s.mid;
        feats.push_back(f);
    }
    // ---- 2. chronological split (indices into feats) ----
    const size_t n = feats.size();
    data::ChronoSplit split = data::chrono_split(n, cfg.train_frac, cfg.valid_frac, cfg.held_out_frac);
    auto stat = [&](const char* name, double v) { out.result.metrics.push_back({name, v}); };
    auto count = [&](const char* name, long long v) { out.result.counters.push_back({name, v}); };
    count("n_samples", static_cast<long long>(n));
    count("n_train", static_cast<long long>(split.train_end));
    count("n_valid", static_cast<long long>(split.valid_end > split.train_end ? split.valid_end - split.train_end : 0));
    count("n_test", static_cast<long long>(split.held_out_begin > split.valid_end
                                               ? split.held_out_begin - split.valid_end
                                               : 0));

    // ---- 3. flagship decay curves: imb1 and OFI vs future return ----
    const i64 h0 = labels::kHorizonsNs[2];  // 10ms reference horizon for model rows
    // Median sample spacing from the overall span (robust to same-ts event
    // pairs, whose zero diffs would collapse a diff-median to ~ns).
    i64 median_dt = 500'000;
    if (feats.size() > 2 && feats.back().ts > feats.front().ts) {
        median_dt = (feats.back().ts - feats.front().ts) / static_cast<i64>(feats.size() - 1);
        if (median_dt <= 0) median_dt = 500'000;
    }
    for (int pass = 0; pass < 2; ++pass) {
        const bool use_ofi = pass == 1;
        std::vector<DecayPoint>& dst = use_ofi ? out.decay_ofi : out.decay_imb1;
        for (size_t hi = 0; hi < labels::kNumHorizons; ++hi) {
            const i64 hz = labels::kHorizonsNs[hi];
            const size_t lag = hac_lag_for(hz, median_dt);
            // non-overlapping stride: labels spaced >= horizon apart share no window
            const size_t stride =
                static_cast<size_t>(std::max<i64>(1, hz / (median_dt > 0 ? median_dt : 1)));
            std::vector<double> scores, rets, pos_rets, pos_nonover;
            scores.reserve(n);
            rets.reserve(n);
            size_t k = 0;
            for (const auto& f : feats) {
                labels::LabelSet l = labels::label_at(mid_series, f.ts, hz, f.spread_rel);
                if (!l.valid) continue;
                const double sc = use_ofi ? f.ofi10 : f.imb1;
                scores.push_back(sc);
                rets.push_back(l.ret);
                if (sc > 0) {
                    pos_rets.push_back(l.ret);
                    if (k++ % stride == 0) pos_nonover.push_back(l.ret);
                }
            }
            DecayPoint dp;
            dp.horizon_ns = hz;
            dp.n = pos_rets.size();
            dp.n_nonover = pos_nonover.size();
            if (!pos_rets.empty()) {
                stats::MeanCI ci = stats::mean_ci(pos_rets, lag);
                dp.cond_ret_pos = ci.mean;
                dp.hac_lo = ci.ci95_hac_lo;
                dp.hac_hi = ci.ci95_hac_hi;
            }
            if (pos_nonover.size() >= 2) {
                stats::MeanCI ci = stats::mean_ci(pos_nonover, 1);
                dp.nover_lo = ci.ci95_lo;
                dp.nover_hi = ci.ci95_hi;
            }
            dp.ic = stats::pearson(scores, rets).r;
            dst.push_back(dp);
        }
    }

    // ---- 4. model comparison on the 10ms directional label ----
    // Build design matrix once (columns = documented v1 subset).
    models::Dataset full;
    full.columns = {"imb_1", "imb_5", "ofi_10ms", "micro_dev", "trade_imb", "signed_vol"};
    // Momentum baseline WITHOUT overlap inflation: backward return over [t-h, t)
    // predicting [t, t+h). (Using the previous FORWARD label would share ~all
    // of its window with the current one at 0.5ms sampling, documented trap.)
    std::vector<double> prev_ret(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double mt = labels::mid_at(mid_series, feats[i].ts);
        const double mpast = labels::mid_at(mid_series, feats[i].ts - h0);
        prev_ret[i] = labels::log_return(mpast, mt);
    }
    for (size_t i = 0; i < n; ++i) {
        labels::LabelSet l = labels::label_at(mid_series, feats[i].ts, h0, feats[i].spread_rel);
        full.y.push_back(l.valid ? l.dir : 0);
        full.yret.push_back(l.valid ? l.ret : 0.0);
        full.X.push_back({feats[i].imb1, feats[i].imb5, feats[i].ofi10, feats[i].micro,
                          feats[i].trade_imb, feats[i].signed_vol});
    }
    auto slice = [&](size_t a, size_t b) {
        models::Dataset d;
        d.columns = full.columns;
        for (size_t i = a; i < b && i < n; ++i) {
            d.X.push_back(full.X[i]);
            d.y.push_back(full.y[i]);
            d.yret.push_back(full.yret[i]);
        }
        return d;
    };
    models::Dataset dtr = slice(0, split.train_end);
    models::Dataset dva = slice(split.train_end, split.valid_end);
    // TEST is the reported split; HELDOUT is the sealed tail, touched once,
    // never used for any selection (thresholds are CLI defaults, untuned).
    models::Dataset dte = slice(split.valid_end, split.held_out_begin);
    models::Dataset dho = slice(split.held_out_begin, n);
    count("n_heldout", static_cast<long long>(dho.X.size()));
    models::LogisticModel logit;
    logit.train(dtr);
    models::Stump stump;
    stump.train(dtr);
    auto score_of = [&](const std::string& m, const models::Dataset& d, size_t off) {
        std::vector<int> pred;
        std::vector<double> score;
        pred.reserve(d.X.size());
        score.reserve(d.X.size());
        for (size_t i = 0; i < d.X.size(); ++i) {
            const size_t gi = off + i;
            if (m == "zero") {
                pred.push_back(0);
                score.push_back(0.0);
            } else if (m == "prev") {
                const int p = models::predict_prev_dir(gi < prev_ret.size() ? prev_ret[gi] : 0.0);
                pred.push_back(p);
                score.push_back(gi < prev_ret.size() ? prev_ret[gi] : 0.0);
            } else if (m == "spread") {
                const int p = models::predict_spread_rule(d.X[i][3], 0.0);
                pred.push_back(p);
                score.push_back(d.X[i][3]);
            } else if (m == "imb1") {
                const int p = models::predict_imbalance_rule(d.X[i][0], cfg.imbalance_thresh);
                pred.push_back(p);
                score.push_back(d.X[i][0]);
            } else if (m == "ofi") {
                const double t = 10.0;
                const int p = d.X[i][2] > t ? 1 : (d.X[i][2] < -t ? -1 : 0);
                pred.push_back(p);
                score.push_back(d.X[i][2]);
            } else if (m == "logistic") {
                const double pr = logit.predict_proba(d.X[i]);
                pred.push_back(pr >= 0.5 ? 1 : -1);
                score.push_back(pr - 0.5);
            } else {  // stump
                const int p = stump.predict(d.X[i]);
                pred.push_back(p);
                score.push_back(static_cast<double>(p));
            }
        }
        return std::make_pair(pred, score);
    };
    const char* mnames[] = {"zero", "prev", "spread", "imb1", "ofi", "logistic", "stump"};
    const std::pair<const char*, models::Dataset*> splits[] = {
        {"train", &dtr}, {"valid", &dva}, {"test", &dte}, {"heldout", &dho}};
    const size_t offs[] = {0, split.train_end, split.valid_end, split.held_out_begin};
    const size_t lag_h0 = hac_lag_for(h0, median_dt);
    const size_t kNumSplits = 4;
    for (const char* mn : mnames) {
        for (size_t s = 0; s < kNumSplits; ++s) {
            auto [pred, score] = score_of(mn, *splits[s].second, offs[s]);
            models::ModelMetrics mm = models::evaluate(pred, score, *splits[s].second);
            ModelRow row;
            row.model = mn;
            row.split = splits[s].first;
            row.accuracy = mm.accuracy;
            row.auc = mm.auc;
            row.ic = mm.ic;
            row.hit = mm.hit_rate;
            row.coverage = mm.coverage;
            row.cond_ret = mm.mean_cond_ret;
            row.n = mm.n;
            // robust CI of conditional signed return
            std::vector<double> signed_rets;
            for (size_t i = 0; i < pred.size(); ++i)
                if (pred[i] != 0) signed_rets.push_back(splits[s].second->yret[i] * pred[i]);
            if (!signed_rets.empty()) {
                stats::MeanCI ci = stats::mean_ci(signed_rets, lag_h0);
                row.cond_lo = ci.ci95_hac_lo;
                row.cond_hi = ci.ci95_hac_hi;
            }
            out.model_rows.push_back(row);
        }
    }
    // walk-forward mean accuracy for imb1 + logistic (stability, not just average)
    {
        std::vector<stats::Fold> folds = stats::walk_forward(n, 4, 0.5);
        for (const char* mn : {"imb1", "logistic"}) {
            double acc_sum = 0.0;
            size_t nf = 0;
            for (const auto& f : folds) {
                models::Dataset dtr2 = slice(f.train_begin, f.train_end);
                models::Dataset dte2 = slice(f.test_begin, f.test_end);
                models::LogisticModel lg2;
                if (std::string(mn) == "logistic") lg2.train(dtr2);
                size_t hits = 0;
                for (size_t i = 0; i < dte2.X.size(); ++i) {
                    int p;
                    if (std::string(mn) == "logistic")
                        p = lg2.predict(dte2.X[i]);
                    else
                        p = models::predict_imbalance_rule(dte2.X[i][0], cfg.imbalance_thresh);
                    const int y = dte2.y[i] > 0 ? 1 : (dte2.y[i] < 0 ? -1 : 0);
                    if (p == y) ++hits;
                }
                if (!dte2.X.empty()) {
                    acc_sum += static_cast<double>(hits) / dte2.X.size();
                    ++nf;
                }
            }
            ModelRow row;
            row.model = mn;
            row.split = "walkforward-mean";
            row.accuracy = nf ? acc_sum / nf : 0.0;
            row.n = nf;
            out.model_rows.push_back(row);
        }
        count("walkforward_folds", static_cast<long long>(folds.size()));
    }

    // ---- 5. permutation null for the imb1 rule on TEST (falsification) ----
    {
        auto [pred, score] = score_of("imb1", dte, split.valid_end);
        models::ModelMetrics mm = models::evaluate(pred, score, dte);
        std::vector<double> null = models::permutation_null_accuracy(dte, score, cfg.seed, 200);
        size_t ge = 0;
        for (double v : null)
            if (v >= mm.accuracy) ++ge;
        const double p_perm = (static_cast<double>(ge) + 1.0) / (null.size() + 1.0);
        stat("perm_p_imb1_test", p_perm);
        stat("acc_imb1_test", mm.accuracy);
        double null_mean = 0.0;
        for (double v : null) null_mean += v;
        if (!null.empty()) null_mean /= null.size();
        stat("perm_null_mean_acc", null_mean);
    }

    // ---- 6. conditional analysis: spread buckets + imbalance deciles ----
    {
        // spread terciles on test
        std::vector<double> sp;
        for (size_t i = split.valid_end; i < split.held_out_begin; ++i) sp.push_back(feats[i].spread_rel);
        std::sort(sp.begin(), sp.end());
        const double q1 = sp.empty() ? 0.0 : sp[sp.size() / 3];
        const double q2 = sp.empty() ? 0.0 : sp[2 * sp.size() / 3];
        const char* bnames[] = {"tight", "mid", "wide"};
        for (int b = 0; b < 3; ++b) {
            std::vector<double> signed_rets;
            for (size_t i = split.valid_end; i < split.held_out_begin; ++i) {
                const double s = feats[i].spread_rel;
                const int bi = s <= q1 ? 0 : (s <= q2 ? 1 : 2);
                if (bi != b) continue;
                labels::LabelSet l = labels::label_at(mid_series, feats[i].ts, h0, s);
                if (!l.valid || l.dir == 0) continue;
                const int p = models::predict_imbalance_rule(feats[i].imb1, cfg.imbalance_thresh);
                if (p == 0) continue;
                signed_rets.push_back(l.ret * p);
            }
            ConditionalBucket cb;
            cb.dim = "spread";
            cb.bucket = bnames[b];
            cb.n = signed_rets.size();
            if (!signed_rets.empty()) {
                stats::MeanCI ci = stats::mean_ci(signed_rets, lag_h0);
                cb.mean_ret = ci.mean;
                cb.hac_lo = ci.ci95_hac_lo;
                cb.hac_hi = ci.ci95_hac_hi;
                size_t h = 0;
                for (double v : signed_rets)
                    if (v > 0) ++h;
                cb.hit = static_cast<double>(h) / signed_rets.size();
            }
            out.conditional.push_back(cb);
        }
        // realized-vol quintiles on test (regime conditioning: calm vs fast markets)
        std::vector<double> rv;
        for (size_t i = split.valid_end; i < split.held_out_begin; ++i) rv.push_back(feats[i].realvol);
        std::sort(rv.begin(), rv.end());
        for (size_t b = 0; b < 5; ++b) {
            const double lo = rv.empty() ? 0.0 : rv[b * rv.size() / 5];
            const double hi = rv.empty() ? 0.0 : (b == 4 ? 1e300 : rv[(b + 1) * rv.size() / 5]);
            std::vector<double> signed_rets;
            for (size_t i = split.valid_end; i < split.held_out_begin; ++i) {
                const double v = feats[i].realvol;
                if (v < lo || v >= hi) continue;
                labels::LabelSet l =
                    labels::label_at(mid_series, feats[i].ts, h0, feats[i].spread_rel);
                if (!l.valid || l.dir == 0) continue;
                const int p = models::predict_imbalance_rule(feats[i].imb1, cfg.imbalance_thresh);
                if (p == 0) continue;
                signed_rets.push_back(l.ret * p);
            }
            ConditionalBucket cb;
            cb.dim = "realvol_quintile";
            cb.bucket = "q" + std::to_string(b + 1);
            cb.n = signed_rets.size();
            if (!signed_rets.empty()) {
                stats::MeanCI ci = stats::mean_ci(signed_rets, lag_h0);
                cb.mean_ret = ci.mean;
                cb.hac_lo = ci.ci95_hac_lo;
                cb.hac_hi = ci.ci95_hac_hi;
                size_t h = 0;
                for (double v : signed_rets)
                    if (v > 0) ++h;
                cb.hit = static_cast<double>(h) / signed_rets.size();
            }
            out.conditional.push_back(cb);
        }
    }

    // ---- 7. execution economics: latency-shifted imb1 rule under 3 queue models ----
    {
        std::vector<execresearch::Opportunity> base;
        std::vector<double> hmid, mmid;
        for (size_t i = split.valid_end; i < split.held_out_begin; ++i) {
            const int p = models::predict_imbalance_rule(feats[i].imb1, cfg.imbalance_thresh);
            if (p == 0) continue;
            const i64 exec_t = feats[i].ts + cfg.exec_latency_ns;
            // Execution pricing uses interpolated mids (continuous-market
            // approximation) so sub-sample latencies have measurable effect;
            // statistics elsewhere use sampled mids only.
            const double exec_mid = labels::mid_at_interp(mid_series, exec_t);
            const double hz_mid = labels::mid_at_interp(mid_series, exec_t + h0);
            const double mk_mid = labels::mid_at_interp(mid_series, exec_t + 10'000'000);
            if (!(exec_mid > 0.0 && hz_mid > 0.0)) continue;
            execresearch::Opportunity o;
            o.signal_t_ns = feats[i].ts;
            o.exec_t_ns = exec_t;
            o.side = p > 0 ? Side::Buy : Side::Sell;
            o.ref_px = static_cast<Price>(std::llround(feats[i].mid));
            // stale execution: pay half-spread against the LATER mid
            const double half_sp = feats[i].spread_rel * exec_mid / 2.0;
            o.exec_px = static_cast<Price>(
                std::llround(exec_mid + (is_buy(o.side) ? half_sp : -half_sp)));
            if (o.exec_px <= 0) continue;
            o.qty = 5;
            o.ahead = 50;  // documented L2-approximation assumption (swept by CLI)
            o.traded_through = 60;
            o.score = feats[i].imb1;
            base.push_back(o);
            hmid.push_back(hz_mid);
            mmid.push_back(mk_mid > 0.0 ? mk_mid : exec_mid);
        }
        count("exec_opportunities", static_cast<long long>(base.size()));
        const execresearch::QueueModel qs[] = {execresearch::QueueModel::Conservative,
                                               execresearch::QueueModel::Neutral,
                                               execresearch::QueueModel::Aggressive};
        for (auto q : qs) {
            execresearch::CostModel cm;
            cm.fee_per_lot = cfg.fee_per_lot;
            execresearch::ExecReport r = execresearch::evaluate(base, hmid, mmid, q, cm);
            r.latency_ns = cfg.exec_latency_ns;
            out.queue_spread.push_back({execresearch::to_cstr(q), static_cast<double>(r.net_pnl)});
            stat(("exec_net_" + std::string(execresearch::to_cstr(q))).c_str(),
                 static_cast<double>(r.net_pnl));
            if (q == execresearch::QueueModel::Neutral) {
                stat("exec_fill_rate", r.fill_rate);
                stat("exec_hit_rate", r.hit_rate);
                stat("exec_adverse_ticks", r.adverse_ticks);
            }
        }
    }

    // ---- 8. manifest + findings (falsification-first wording) ----
    research::ResearchManifest m;
    m.run_id = cfg.run_id.empty() ? cfg.experiment_id + "/seed" + std::to_string(cfg.seed) : cfg.run_id;
    m.experiment_id = cfg.experiment_id;
    m.git_commit = "unknown";  // CLI fills from `git rev-parse`
    m.dataset_hash = cfg.dataset_hash;
    m.dataset_name = cfg.dataset_name;
    m.config_hash = cfg.config_hash;
    m.seed = cfg.seed;
    m.feature_version = features::kFeatureVersion;
    m.model_version = models::kModelVersion;
    m.compiler = research::compiler_id();
    m.platform = research::platform_id();
    m.manifest_hash = m.compute_hash();
    out.result.manifest = m;
    // verdict helpers
    auto split_row = [&](const std::string& model, const std::string& split) -> const ModelRow* {
        for (const auto& r : out.model_rows)
            if (r.model == model && r.split == split) return &r;
        return nullptr;
    };
    auto test_row = [&](const std::string& model) -> const ModelRow* {
        return split_row(model, "test");
    };
    const ModelRow* imb = test_row("imb1");
    const ModelRow* logi = test_row("logistic");
    if (imb && imb->cond_hi < 0)
        out.result.findings.push_back("IMB1@10ms TEST: FAILED (robust CI below zero)");
    else if (imb && imb->cond_lo > 0)
        out.result.findings.push_back("IMB1@10ms TEST: SUPPORTED (robust CI above zero, pre-cost)");
    else
        out.result.findings.push_back("IMB1@10ms TEST: INCONCLUSIVE (robust CI spans zero)");
    if (logi && imb && logi->accuracy > imb->accuracy + 0.02)
        out.result.findings.push_back("COMPLEXITY: logistic materially beats imb1 on TEST");
    else
        out.result.findings.push_back("COMPLEXITY: logistic does NOT materially beat imb1 on TEST");
    // Sealed-tail check: the held-out accuracy must agree with TEST within
    // tolerance, else the TEST result is flagged as unstable (no tuning on
    // heldout, this is a one-way gate, never a selection criterion).
    if (const ModelRow* imb_ho = split_row("imb1", "heldout"); imb && imb_ho && imb_ho->n > 0) {
        const double gap = imb_ho->accuracy - imb->accuracy;
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "HELDOUT: imb1 acc=%.3f vs TEST %.3f (%s)", imb_ho->accuracy,
                      imb->accuracy, gap >= -0.05 ? "consistent" : "DIVERGED — instability flag");
        out.result.findings.push_back(buf);
        stat("heldout_imb1_acc", imb_ho->accuracy);
    }
    out.result.result_hash = out.result.compute_result_hash();
    return out;
}

}  // namespace pipeline
}  // namespace arbiter