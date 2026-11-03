#include "testfw.hpp"
#include "arbiter/stats.hpp"
#include "arbiter/rng.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
using namespace arbiter;
namespace { bool approx(double a, double b, double tol=1e-9) { return std::fabs(a-b)<=tol; } }
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
    CHECK_EQ(folds.size(), 4u);
    CHECK_EQ(folds.front().train_end, 50u);
    CHECK_EQ(folds.back().test_end, 100u);
    for (size_t i = 1; i < folds.size(); ++i) CHECK(folds[i].test_begin >= folds[i - 1].test_end);
    // small datasets: only non-empty test ranges are returned (a clearly
    // documented number of valid folds, never an empty one)
    auto tiny = stats::walk_forward(5, 4, 0.5);
    for (const auto& f : tiny) CHECK(f.test_end > f.test_begin);
    auto none = stats::walk_forward(2, 4, 0.5);
    CHECK(none.empty());
    for (size_t i = 1; i < tiny.size(); ++i) CHECK(tiny[i].test_begin >= tiny[i - 1].test_end);
}


AX_TEST_MAIN
