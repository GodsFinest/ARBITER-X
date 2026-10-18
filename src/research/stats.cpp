// Robust statistics for dependent HF observations.
#include "arbiter/stats.hpp"
#include "arbiter/rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace arbiter {
namespace stats {

namespace {
double mean_of(const std::vector<double>& x) {
    if (x.empty()) return 0.0;
    double s = 0.0;
    for (double v : x) s += v;
    return s / static_cast<double>(x.size());
}
}  // namespace

double hac_variance(const std::vector<double>& x, size_t lag) {
    const size_t n = x.size();
    if (n < 2) return 0.0;
    const double m = mean_of(x);
    std::vector<double> d(n);
    for (size_t i = 0; i < n; ++i) d[i] = x[i] - m;
    double gamma0 = 0.0;
    for (double v : d) gamma0 += v * v;
    gamma0 /= static_cast<double>(n);
    double v = gamma0;
    const size_t L = std::min(lag, n - 1);
    for (size_t j = 1; j <= L; ++j) {
        double gj = 0.0;
        for (size_t i = j; i < n; ++i) gj += d[i] * d[i - j];
        gj /= static_cast<double>(n);
        const double w = 1.0 - static_cast<double>(j) / static_cast<double>(L + 1);  // Bartlett
        v += 2.0 * w * gj;
    }
    return v / static_cast<double>(n);  // variance of the mean
}

MeanCI mean_ci(const std::vector<double>& x, size_t hac_lag) {
    MeanCI r;
    r.n = x.size();
    if (x.empty()) return r;
    r.mean = mean_of(x);
    double s2 = 0.0;
    for (double v : x) s2 += (v - r.mean) * (v - r.mean);
    const double var = r.n > 1 ? s2 / static_cast<double>(r.n - 1) : 0.0;
    r.se_iid = std::sqrt(var / static_cast<double>(r.n));
    const double hv = hac_variance(x, hac_lag);
    r.se_hac = std::sqrt(hv < 0.0 ? 0.0 : hv);
    r.ci95_lo = r.mean - 1.96 * r.se_iid;
    r.ci95_hi = r.mean + 1.96 * r.se_iid;
    r.ci95_hac_lo = r.mean - 1.96 * r.se_hac;
    r.ci95_hac_hi = r.mean + 1.96 * r.se_hac;
    return r;
}


CorrResult pearson(const std::vector<double>& x, const std::vector<double>& y) {
    CorrResult r;
    const size_t n = std::min(x.size(), y.size());
    r.n = n;
    if (n < 2) return r;
    const double mx = mean_of(x), my = mean_of(y);
    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
        syy += (y[i] - my) * (y[i] - my);
    }
    if (sxx <= 0.0 || syy <= 0.0) return r;
    r.r = sxy / std::sqrt(sxx * syy);
    return r;
}



std::vector<double> bonferroni(const std::vector<double>& p) {
    std::vector<double> q(p.size());
    const double m = static_cast<double>(p.size());
    for (size_t i = 0; i < p.size(); ++i) q[i] = std::min(1.0, p[i] * m);
    return q;
}

std::vector<double> holm(const std::vector<double>& p) {
    const size_t m = p.size();
    std::vector<size_t> idx(m);
    for (size_t i = 0; i < m; ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return p[a] < p[b]; });
    std::vector<double> adjusted(m, 1.0);
    double running_max = 0.0;
    for (size_t k = 0; k < m; ++k) {
        const double raw = std::clamp(p[idx[k]], 0.0, 1.0);
        const double candidate = static_cast<double>(m - k) * raw;
        running_max = std::max(running_max, std::min(1.0, candidate));
        adjusted[idx[k]] = running_max;
    }
    return adjusted;
}

std::vector<double> fdr_bh(const std::vector<double>& p) {
    const size_t m = p.size();
    std::vector<size_t> idx(m);
    for (size_t i = 0; i < m; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return p[a] < p[b]; });
    std::vector<double> q(m, 1.0);
    double running = 1.0;
    for (size_t k = m; k-- > 0;) {
        const double v = p[idx[k]] * static_cast<double>(m) / static_cast<double>(k + 1);
        running = std::min(running, v);
        q[idx[k]] = std::min(1.0, running);
    }
    return q;
}

std::vector<Fold> walk_forward(size_t n, size_t n_folds, double min_train_frac) {
    std::vector<Fold> folds;
    if (n_folds == 0 || n < n_folds + 1) return folds;
    const size_t test_block = n / (n_folds + 1);  // last n_folds blocks are tests
    if (test_block == 0) return folds;
    const size_t min_train = static_cast<size_t>(n * min_train_frac);
    for (size_t f = 0; f < n_folds; ++f) {
        const size_t test_begin = n - (n_folds - f) * test_block;
        const size_t test_end = test_begin + test_block;
        if (test_begin < min_train) continue;  // expanding train too small: skip
        folds.push_back({0, test_begin, test_begin, test_end});
    }
    return folds;
}


std::string format_ci(double lo, double hi) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "[%.6g, %.6g]", lo, hi);
    return std::string(buf);
}

}  // namespace stats
}  // namespace arbiter