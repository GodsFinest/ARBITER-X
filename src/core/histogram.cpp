#include "arbiter/histogram.hpp"

#include <algorithm>
#include <cmath>

namespace arbiter {

Histogram::Histogram(size_t reservoir_cap) : capacity_(reservoir_cap) {
    samples_.reserve(capacity_);
}

void Histogram::record(double v) noexcept {
    if (count_ == 0 || v < min_) min_ = v;
    if (count_ == 0 || v > max_) max_ = v;
    sum_ += v;
    ++count_;
    sorted_ = false;
    if (samples_.size() < capacity_) {
        samples_.push_back(v);
        return;
    }
    // Reservoir sampling (algorithm R): replace with probability cap/count.
    u64 z = (rng_state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    u64 slot = z % count_;
    if (slot < capacity_) samples_[static_cast<size_t>(slot)] = v;
}

void Histogram::reserve(size_t n) {
    if (n < capacity_) samples_.reserve(n);
}

double Histogram::percentile(double p) const {
    if (count_ == 0) return 0.0;
    if (!sorted_) {
        std::sort(samples_.begin(), samples_.end());
        sorted_ = true;
    }
    double idx = p * static_cast<double>(samples_.size() - 1);
    double lo = std::floor(idx), hi = std::ceil(idx);
    double frac = idx - lo;
    double a = samples_[static_cast<size_t>(lo)];
    double b = samples_[static_cast<size_t>(hi)];
    return a + (b - a) * frac;
}

Histogram::Summary Histogram::summarize() const {
    Summary s;
    s.count = count_;
    if (count_ == 0) return s;
    s.mean = mean();
    s.mn = min_;
    s.mx = max_;
    s.p50 = percentile(0.50);
    s.p90 = percentile(0.90);
    s.p95 = percentile(0.95);
    s.p99 = percentile(0.99);
    s.p999 = percentile(0.999);
    s.p9999 = percentile(0.9999);
    return s;
}

void Histogram::reset() noexcept {
    samples_.clear();
    count_ = 0;
    sum_ = 0;
    min_ = 0;
    max_ = 0;
    sorted_ = false;
}

}  // namespace arbiter