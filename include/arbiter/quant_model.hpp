#pragma once
// Predictive models: simple baselines FIRST, complexity only if justified.
//
// Hierarchy (docs/RESEARCH.md): heuristic -> linear/logistic ->
// stump/ensemble, each compared on the SAME out-of-sample folds with the
// SAME execution accounting. If logistic beats the ensemble after costs, the
// report says so, that is a result, not a failure.
//
// All models are deterministic: zero init, fixed iteration counts, no RNG.
// Model versions are pinned (kModelVersion) and recorded in manifests.

#include "arbiter/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace arbiter {
namespace models {

inline constexpr const char* kModelVersion = "model_v1";

// Row-major design matrix with fixed column order (features::feature_names_v1
// for v1 models, or documented subsets for ablations).
struct Dataset {
    std::vector<std::vector<double>> X;  // [n][d], standardized inside linear models
    std::vector<int> y;                  // directional labels {-1,0,+1}; models use sign(y)
    std::vector<double> yret;            // raw returns (for conditional-return stats)
    std::vector<std::string> columns;    // width-d names
};

// ---- baselines (no training) ----

inline int predict_zero(const std::vector<double>&) { return 0; }           // Baseline 1: no movement
int predict_prev_dir(double prev_ret);                                // Baseline 2: previous direction
int predict_spread_rule(double micro_dev_ticks, double thresh = 0.0);  // Baseline 3
int predict_imbalance_rule(double imb, double thresh);                // Baseline 4/5/6 family
inline double predict_imbalance_score(double imb) { return imb; }            // raw score for AUC/IC

// ---- logistic regression (batch gradient descent, L2, deterministic) ----

struct LogisticConfig {
    double lr = 0.5;
    size_t iters = 500;
    double l2 = 1e-4;
};

class LogisticModel {
public:
    explicit LogisticModel(LogisticConfig c = {}) : cfg_(c) {}
    void train(const Dataset& ds);            // y mapped to {0,1} via sign (0 -> 0)
    double predict_proba(const std::vector<double>& x) const;
    int predict(const std::vector<double>& x) const { return predict_proba(x) >= 0.5 ? 1 : -1; }
    const std::vector<double>& weights() const { return w_; }
    double bias() const { return b_; }

private:
    LogisticConfig cfg_;
    std::vector<double> mean_, sd_;
    std::vector<double> w_;
    double b_ = 0.0;
};

// ---- decision stump (one split, exhaustive scan, the honest nonlinear baseline) ----

struct Stump {
    size_t col = 0;
    double thresh = 0.0;
    int left = -1, right = 1;  // predictions
    std::string col_name;
    void train(const Dataset& ds);  // maximizes train accuracy (ties -> smallest col, then thresh)
    int predict(const std::vector<double>& x) const { return x[col] <= thresh ? left : right; }
};

// ---- evaluation (shared by all models; identical folds, identical metrics) ----

struct ModelMetrics {
    size_t n = 0;
    double accuracy = 0.0;
    double auc = 0.0;          // rank AUC on score vs (y>0); 0.5 when degenerate
    double mse = 0.0, mae = 0.0;  // on sign error where applicable
    double mean_cond_ret = 0.0;   // mean yret where pred != 0
    double hit_rate = 0.0;        // P(sign(pred)==sign(ret) | pred!=0, ret!=0)
    double coverage = 0.0;        // P(pred != 0)
    double ic = 0.0;              // Pearson(score, yret)
};

ModelMetrics evaluate(const std::vector<int>& pred, const std::vector<double>& score,
                      const Dataset& ds);
double rank_auc(const std::vector<double>& score, const std::vector<int>& y01);

// Permutation null: shuffle labels with a seeded RNG, re-evaluate the SCORE
// function, return the null distribution of a chosen statistic (accuracy).
// Deterministic given seed. If the real statistic sits inside the null mass,
// the report must say FAILED.
std::vector<double> permutation_null_accuracy(const Dataset& ds,
                                              const std::vector<double>& score, u64 seed,
                                              size_t reps = 500);

}  // namespace models
}  // namespace arbiter