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

#include <limits>
#include <stdexcept>
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
        auto threshold = [](size_t value) {
            return (value / 10) * 7 + ((value % 10) * 7) / 10;
        };
        while (threshold(cap) < n) {
            if (cap > std::numeric_limits<size_t>::max() / 2)
                throw std::length_error("FlatIdMap capacity overflow");
            cap *= 2;
        }
        if (cap != mask_ + 1) rehash(cap);
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

    // Capacity-managed upsert: grows the table when the load factor would be
    // exceeded (may allocate and throw). NOT for the hot path — callers that
    // must not allocate call reserve() first and use insert_prechecked().
    void insert(u64 key, u64 val) {
        if (key == 0) return;
        {
            u64* existing = find(key);
            if (existing) {
                *existing = val;
                return;
            }
        }
        const size_t cap = mask_ + 1;
        const size_t threshold = (cap / 10) * 7 + ((cap % 10) * 7) / 10;
        if (size_ + 1 > threshold) grow();
        insert_prechecked(key, val);
    }

    // Upsert with the caller-held capacity contract: the caller must have
    // ensured load-factor headroom (reserve(size() + 1)) or the key must
    // already exist. GENUINELY non-allocating and non-throwing: no growth
    // path exists here, which is what makes the book's commit phase
    // infallible after its preflight. (The old single insert() was noexcept
    // yet could allocate inside grow() — an escaping bad_alloc would have
    // called std::terminate.)
    void insert_prechecked(u64 key, u64 val) noexcept {
        if (key == 0) return;  // 0 is the reserved empty marker
        size_t i = Rng::mix(key) & mask_;
        while (keys_[i] != 0) {
            if (keys_[i] == key) {
                vals_[i] = val;
                return;
            }
            i = (i + 1) & mask_;
        }
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
    size_t capacity() const noexcept { return mask_ + 1; }

    // Iterate live entries (unordered). Callback: fn(key, value).
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (size_t i = 0; i <= mask_; ++i)
            if (keys_[i] != 0) fn(keys_[i], vals_[i]);
    }

private:
    void grow() {
        if (mask_ + 1 > std::numeric_limits<size_t>::max() / 2)
            throw std::length_error("FlatIdMap capacity overflow");
        rehash((mask_ + 1) * 2);
    }

    void rehash(size_t cap) {
        // Build the replacement table before touching the live one.  A
        // failed allocation during reserve/growth must leave the index fully
        // usable; the book calls reserve as an exception-prone preflight.
        std::vector<u64> new_keys(cap, 0);
        std::vector<u64> new_vals(cap, 0);
        const size_t new_mask = cap - 1;
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == 0) continue;
            size_t j = Rng::mix(keys_[i]) & new_mask;
            while (new_keys[j] != 0) j = (j + 1) & new_mask;
            new_keys[j] = keys_[i];
            new_vals[j] = vals_[i];
        }
        keys_.swap(new_keys);
        vals_.swap(new_vals);
        mask_ = new_mask;
    }

    std::vector<u64> keys_;
    std::vector<u64> vals_;
    size_t mask_ = 0;
    size_t size_ = 0;
};

}  // namespace arbiter
