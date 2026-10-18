#pragma once
// Statistical analysis for dependent high-frequency observations.
//
// WARNING (see docs/RESEARCH.md): high-frequency microstructure
// samples are autocorrelated. IID t-tests on overlapping/tick-consecutive
// samples OVERSTATE significance. This library therefore provides:
//   * HAC (Newey-West) standard errors for mean estimates,
//   * non-overlapping-subsample estimators,
// and every report prints BOTH the naive IID interval and the robust one so
// the dependence discount is visible. Multiple testing: Holm, Bonferroni,
// and BH-FDR.
//
// (Pruned 2026-09-09: block-bootstrap / Wilson hit-rate / calibration-bucket
// / z-pvalue helpers were dead code, no experiment or test called them;
// the methodology docs previously overstated their use.)

#include "arbiter/types.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace arbiter {
namespace stats {

struct MeanCI {
    double mean = 0.0;
    double se_iid = 0.0;     // s/sqrt(n)
    double se_hac = 0.0;     // Newey-West SE (lag parameter documented by caller)
    double ci95_lo = 0.0, ci95_hi = 0.0;      // IID-based 95% CI
    double ci95_hac_lo = 0.0, ci95_hac_hi = 0.0;  // HAC-based 95% CI
    size_t n = 0;
};

MeanCI mean_ci(const std::vector<double>& x, size_t hac_lag = 10);

// Newey-West HAC variance of the mean with Bartlett kernel, lag L.
// Documented choice: default L=10 samples; callers studying longer-memory
// processes must justify larger L in the report.
double hac_variance(const std::vector<double>& x, size_t lag);

// Pearson correlation (information coefficient) with n.
struct CorrResult {
    double r = 0.0;
    size_t n = 0;
};
CorrResult pearson(const std::vector<double>& x, const std::vector<double>& y);

// Multiple-testing corrections over a family of p-values.
std::vector<double> holm(const std::vector<double>& p);        // Holm step-down FWER
std::vector<double> bonferroni(const std::vector<double>& p);  // min(1, p*m)
std::vector<double> fdr_bh(const std::vector<double>& p);      // Benjamini-Hochberg q-values

// Chronological walk-forward splitter: expanding train window, fixed test
// blocks. Returns (train_end, test_end) index pairs; test blocks never
// overlap each other. Documented in every report that uses it.
struct Fold {
    size_t train_begin = 0, train_end = 0, test_begin = 0, test_end = 0;
};
std::vector<Fold> walk_forward(size_t n, size_t n_folds, double min_train_frac = 0.5);

std::string format_ci(double lo, double hi);

}  // namespace stats
}  // namespace arbiter