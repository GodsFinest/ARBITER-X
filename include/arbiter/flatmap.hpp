#pragma once
// Fixed-capacity open-addressing hash map (u64 key -> u64 value) with linear
// probing and backward-shift deletion. Preallocated: inserts and erases
// perform NO allocation once constructed. Key 0 is reserved as the empty
// marker (order ids are guaranteed >= 1 by the domain bounds).
//
// Used for the optimized book's order-id index so the matching hot path is
// allocation-free in steady state, verified by the allocation probe in the
// benchmark suite.

#include "arbiter/rng.hpp"
#include "arbiter/types.hpp"

#include <vector>

namespace arbiter {

class FlatIdMap {
public:
    explicit FlatIdMap(size_t cap_pow2 = 1u << 15) { reset(cap_pow2); }

    void reset(size_t cap_pow2) {
        // the probe math requires a power-of-two capacity; round up so a
        // non-pow2 argument cannot silently corrupt the mask
        size_t cap = 1;
        while (cap < cap_pow2 && cap < (size_t{1} << 62)) cap <<= 1;
        keys_.assign(cap, 0);
        vals_.assign(cap, 0);
        mask_ = cap - 1;
        size_ = 0;
    }

    // Ensure capacity for at least n elements (rounds up to a power of two).
    void reserve(size_t n) {
        size_t cap = mask_ + 1;
        while (cap * 7 / 10 < n) cap *= 2;
        if (cap != mask_ + 1) reset(cap);
    }

    u64* find(u64 key) noexcept {
        if (key == 0) return nullptr;
        size_t i = Rng::mix(key) & mask_;
        while (true) {
            if (keys_[i] == 0) return nullptr;
            if (keys_[i] == key) return &vals_[i];
            i = (i + 1) & mask_;
        }
    }
    const u64* find(u64 key) const noexcept {
        return const_cast<FlatIdMap*>(this)->find(key);
    }

    bool contains(u64 key) const noexcept { return find(key) != nullptr; }

    // Upsert: overwrites the value if the key exists, inserts otherwise.
    // Allocation-free once the table is grown to steady-state capacity.
    void insert(u64 key, u64 val) noexcept {
        if (key == 0) return;
        {
            u64* existing = find(key);
            if (existing) {
                *existing = val;
                return;
            }
        }
        if ((size_ + 1) * 10 >= (mask_ + 1) * 7) grow();
        size_t i = Rng::mix(key) & mask_;
        while (keys_[i] != 0) i = (i + 1) & mask_;
        keys_[i] = key;
        vals_[i] = val;
        ++size_;
    }

    bool erase(u64 key) noexcept {
        if (key == 0) return false;
        size_t i = Rng::mix(key) & mask_;
        while (true) {
            if (keys_[i] == 0) return false;
            if (keys_[i] == key) break;
            i = (i + 1) & mask_;
        }
        // Backward-shift deletion: every entry after the hole whose probe
        // path passes through the hole (strictly earlier than its current
        // slot) is shifted back; scanning continues across moved entries so
        // the cluster stays contiguous.
        size_t hole = i;
        size_t j = (i + 1) & mask_;
        while (keys_[j] != 0) {
            const size_t home = Rng::mix(keys_[j]) & mask_;
            const size_t d_hole = (hole - home) & mask_;
            const size_t d_j = (j - home) & mask_;
            if (d_hole < d_j) {
                keys_[hole] = keys_[j];
                vals_[hole] = vals_[j];
                hole = j;
            }
            j = (j + 1) & mask_;
        }
        keys_[hole] = 0;
        vals_[hole] = 0;
        --size_;
        return true;
    }

    size_t size() const noexcept { return size_; }

    // Iterate live entries (unordered). Callback: fn(key, value).
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (size_t i = 0; i <= mask_; ++i)
            if (keys_[i] != 0) fn(keys_[i], vals_[i]);
    }

private:
    void grow() {
        std::vector<u64> ok = std::move(keys_);
        std::vector<u64> ov = std::move(vals_);
        const size_t cap = (mask_ + 1) * 2;
        keys_.assign(cap, 0);
        vals_.assign(cap, 0);
        mask_ = cap - 1;
        size_ = 0;
        for (size_t i = 0; i < ok.size(); ++i) {
            if (ok[i] != 0) insert(ok[i], ov[i]);
        }
    }

    std::vector<u64> keys_;
    std::vector<u64> vals_;
    size_t mask_ = 0;
    size_t size_ = 0;
};

}  // namespace arbiter