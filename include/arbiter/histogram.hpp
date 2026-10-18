#pragma once
// Latency/throughput histograms with percentile reporting.
// Wall-clock measurement ONLY, never part of the deterministic core.
// Fixed-capacity reservoir (Vitter's algorithm R) keeps memory bounded while
// remaining unbiased for arbitrarily long runs.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "arbiter/types.hpp"

namespace arbiter {

class Histogram {
public:
    // Default reservoir: 64k samples (512KB). Percentiles of latency-shaped
    // distributions are stable at this size; the previous 4M default made
    // every RunStats reserve ~224MB across its 7 histograms (and copies in
    // ExperimentResult vectors doubled that) for no statistical benefit.
    explicit Histogram(size_t reservoir_cap = 1u << 16);

    void record(double v) noexcept;   // v is expected >= 0; no filtering
    void reserve(size_t n);
    size_t count() const noexcept { return count_; }
    double min() const noexcept { return min_; }
    double max() const noexcept { return max_; }
    double mean() const noexcept { return count_ == 0 ? 0.0 : sum_ / static_cast<double>(count_); }

    // p in [0,1]; 0 for empty histogram. Sorts the reservoir on demand.
    double percentile(double p) const;

    struct Summary {
        size_t count = 0;
        double mean = 0, mn = 0, p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0, p9999 = 0, mx = 0;
    };
    // Observation-only: summarize() may lazily sort the reservoir cache,
    // which does not change any observed value, const by design so
    // reporting code never needs const_cast on RunStats.
    Summary summarize() const;

    void reset() noexcept;

private:
    // samples_/sorted_ carry a lazy-sort cache: percentile() may sort them,
    // which does not change any OBSERVED value (order of the reservoir is
    // not part of the recorded statistics). const-correct reporting code
    // never needs const_cast on RunStats. reservoir sampling order is
    // deterministic regardless (rng_state_ is seeded per-Histogram).
    mutable std::vector<double> samples_;
    size_t capacity_;
    size_t count_ = 0;   // total records seen
    u64 rng_state_ = 0x853c49e6748fea9bull;
    double sum_ = 0, min_ = 0, max_ = 0;
    mutable bool sorted_ = false;
};

}  // namespace arbiter