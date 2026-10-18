// Predictive models: baselines, logistic regression, stump, evaluation.
#include "arbiter/quant_model.hpp"
#include "arbiter/rng.hpp"
#include "arbiter/stats.hpp"

#include <algorithm>
#include <cmath>

namespace arbiter {
namespace models {

int predict_prev_dir(double prev_ret) {
    if (prev_ret > 0.0) return 1;
    if (prev_ret < 0.0) return -1;
    return 0;
}

int predict_spread_rule(double micro_dev_ticks, double thresh) {
    if (micro_dev_ticks > thresh) return 1;
    if (micro_dev_ticks < -thresh) return -1;
    return 0;
}

int predict_imbalance_rule(double imb, double thresh) {
    if (imb > thresh) return 1;
    if (imb < -thresh) return -1;
    return 0;  // dead zone |imb| <= thresh -> abstain
}

void LogisticModel::train(const Dataset& ds) {
    const size_t n = ds.X.size();
    if (n == 0) return;
    const size_t d = ds.X[0].size();
    mean_.assign(d, 0.0);
    sd_.assign(d, 1.0);
    for (size_t j = 0; j < d; ++j) {
        double s = 0.0;
        for (size_t i = 0; i < n; ++i) s += ds.X[i][j];
        mean_[j] = s / static_cast<double>(n);
        double v = 0.0;
        for (size_t i = 0; i < n; ++i) v += (ds.X[i][j] - mean_[j]) * (ds.X[i][j] - mean_[j]);
        const double sd = std::sqrt(v / static_cast<double>(n));
        sd_[j] = sd > 1e-12 ? sd : 1.0;  // constant columns -> zeros, no NaN
    }
    w_.assign(d, 0.0);
    b_ = 0.0;
    std::vector<double> grad(d);
    for (size_t it = 0; it < cfg_.iters; ++it) {
        std::fill(grad.begin(), grad.end(), 0.0);
        double gb = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double z = b_;
            for (size_t j = 0; j < d; ++j) z += w_[j] * (ds.X[i][j] - mean_[j]) / sd_[j];
            const double p = 1.0 / (1.0 + std::exp(-z));
            const double y = ds.y[i] > 0 ? 1.0 : 0.0;
            const double err = p - y;
            for (size_t j = 0; j < d; ++j) grad[j] += err * (ds.X[i][j] - mean_[j]) / sd_[j];
            gb += err;
        }
        const double nn = static_cast<double>(n);
        for (size_t j = 0; j < d; ++j) w_[j] -= cfg_.lr * (grad[j] / nn + cfg_.l2 * w_[j]);
        b_ -= cfg_.lr * gb / nn;
    }
}

double LogisticModel::predict_proba(const std::vector<double>& x) const {
    if (w_.empty()) return 0.5;
    double z = b_;
    for (size_t j = 0; j < w_.size() && j < x.size(); ++j)
        z += w_[j] * (x[j] - mean_[j]) / sd_[j];
    return 1.0 / (1.0 + std::exp(-z));
}

void Stump::train(const Dataset& ds) {
    const size_t n = ds.X.size();
    col = 0;
    thresh = 0.0;
    left = -1;
    right = 1;
    if (n == 0 || ds.X[0].empty()) return;
    const size_t d = ds.X[0].size();
    auto acc = [&](size_t c, double t, int l, int r) {
        size_t hits = 0;
        for (size_t i = 0; i < n; ++i) {
            const int p = ds.X[i][c] <= t ? l : r;
            if (p == (ds.y[i] > 0 ? 1 : (ds.y[i] < 0 ? -1 : 0))) ++hits;
        }
        return hits;
    };
    size_t best = 0;
    bool first = true;
    for (size_t c = 0; c < d; ++c) {
        // candidate thresholds: sorted unique midpoints (deterministic)
        std::vector<double> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = ds.X[i][c];
        std::sort(v.begin(), v.end());
        for (size_t k = 0; k + 1 < n; ++k) {
            if (v[k] == v[k + 1]) continue;
            const double t = (v[k] + v[k + 1]) * 0.5;
            for (int flip = 0; flip < 2; ++flip) {
                const int l = flip ? 1 : -1, r = flip ? -1 : 1;
                const size_t a = acc(c, t, l, r);
                if (first || a > best) {
                    first = false;
                    best = a;
                    col = c;
                    thresh = t;
                    left = l;
                    right = r;
                }
            }
        }
    }
    if (!ds.columns.empty() && col < ds.columns.size()) col_name = ds.columns[col];
}

double rank_auc(const std::vector<double>& score, const std::vector<int>& y01) {
    const size_t n = std::min(score.size(), y01.size());
    size_t pos = 0, neg = 0;
    for (size_t i = 0; i < n; ++i) (y01[i] > 0 ? pos : neg)++;
    if (pos == 0 || neg == 0) return 0.5;
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return score[a] < score[b]; });
    double rank_sum = 0.0;
    for (size_t r = 0; r < n; ++r)
        if (y01[idx[r]] > 0) rank_sum += static_cast<double>(r + 1);
    return (rank_sum - static_cast<double>(pos) * (pos + 1) / 2.0) /
           (static_cast<double>(pos) * neg);
}

ModelMetrics evaluate(const std::vector<int>& pred, const std::vector<double>& score,
                      const Dataset& ds) {
    ModelMetrics m;
    m.n = ds.y.size();
    if (m.n == 0) return m;
    std::vector<int> y01(m.n);
    for (size_t i = 0; i < m.n; ++i) y01[i] = ds.y[i] > 0 ? 1 : 0;
    m.auc = rank_auc(score, y01);
    size_t hits = 0, scored = 0, covered = 0, hr_h = 0, hr_n = 0;
    double se_sum = 0.0, ae_sum = 0.0, ret_sum = 0.0;
    for (size_t i = 0; i < m.n; ++i) {
        const int y = ds.y[i] > 0 ? 1 : (ds.y[i] < 0 ? -1 : 0);
        const int p = pred[i];
        if (p == y) ++hits;
        ++scored;
        const double e = static_cast<double>(p - y);
        se_sum += e * e;
        ae_sum += std::fabs(e);
        if (p != 0) {
            ++covered;
            ret_sum += ds.yret[i] * p;  // signed return captured by the call
            if (y != 0) {
                ++hr_n;
                if (p == y) ++hr_h;
            }
        }
    }
    m.accuracy = static_cast<double>(hits) / static_cast<double>(scored);
    m.mse = se_sum / scored;
    m.mae = ae_sum / scored;
    m.coverage = static_cast<double>(covered) / scored;
    m.mean_cond_ret = covered ? ret_sum / covered : 0.0;
    m.hit_rate = hr_n ? static_cast<double>(hr_h) / hr_n : 0.0;
    m.ic = stats::pearson(score, ds.yret).r;
    return m;
}

std::vector<double> permutation_null_accuracy(const Dataset& ds,
                                              const std::vector<double>& score, u64 seed,
                                              size_t reps) {
    std::vector<double> null;
    null.reserve(reps);
    const size_t n = ds.y.size();
    if (n == 0) return null;
    std::vector<int> perm = ds.y;
    Rng rng(Rng::stream(seed, 0x9E9A777ull).state);
    std::vector<int> pred(n);
    for (size_t i = 0; i < n; ++i) pred[i] = score[i] >= 0.0 ? 1 : -1;
    for (size_t r = 0; r < reps; ++r) {
        // Fisher-Yates with the deterministic stream
        for (size_t i = n - 1; i > 0; --i) {
            const size_t j = static_cast<size_t>(rng.bounded(i + 1));
            std::swap(perm[i], perm[j]);
        }
        size_t hits = 0;
        for (size_t i = 0; i < n; ++i) {
            const int y = perm[i] > 0 ? 1 : (perm[i] < 0 ? -1 : 0);
            if (pred[i] == y) ++hits;
        }
        null.push_back(static_cast<double>(hits) / static_cast<double>(n));
    }
    return null;
}

}  // namespace models
}  // namespace arbiter