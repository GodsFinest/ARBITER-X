#pragma once
// Bounded single-producer/single-consumer ring buffer.
// Used for cross-thread message passing in the concurrency experiments and
// benchmarks. Cache-line padded indices avoid false sharing.
// SP/SC contract is the caller's responsibility; enforced sizes prevent
// unbounded growth. Relaxed stores on the local index, acquire/release on
// the published index (correct SPSC protocol).

#include "arbiter/types.hpp"

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace arbiter {

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity_pow2)
        : buf_(validated_capacity(capacity_pow2)), mask_(capacity_pow2 - 1) {}

    bool push(const T& v) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        if (h - t == buf_.size()) return false;  // full
        buf_[h & mask_] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;  // empty
        out = buf_[t & mask_];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    size_t capacity() const noexcept { return buf_.size(); }

private:
    static size_t validated_capacity(size_t capacity) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("SpscRing capacity must be a non-zero power of two");
        return capacity;
    }

    alignas(64) std::atomic<size_t> head_{0};  // producer-local
    alignas(64) std::atomic<size_t> tail_{0};  // consumer-local
    std::vector<T> buf_;
    size_t mask_;
};

}  // namespace arbiter