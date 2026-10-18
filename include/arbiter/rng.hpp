#pragma once
// SplitMix64 RNG, the only randomness source in the deterministic core.
// All stochastic behavior derives from explicit (master_seed, stream_id) pairs
// so that every subsystem owns an independent, reproducible stream.

#include "arbiter/types.hpp"

namespace arbiter {

#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 u128_t;
#endif

struct Rng {
    u64 state;

    explicit Rng(u64 seed) noexcept : state(seed) {}

    static constexpr u64 mix(u64 z) noexcept {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Derive an independent stream from the experiment master seed.
    static Rng stream(u64 master_seed, u64 stream_id) noexcept {
        return Rng(mix(master_seed ^ mix(stream_id)));
    }

    u64 next() noexcept {
        u64 z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    u32 next_u32() noexcept { return static_cast<u32>(next() >> 32); }

    // Uniform in [0, n) for n >= 1. Lemire's method: the high 64 bits of the
    // 64x64->128 product x*n. Where __int128 exists it is used directly;
    // otherwise an exact 32-bit-split mulhi computes the same value, so the
    // two paths agree bit-for-bit (verified against __int128 in
    // tests/test_core on hosts that have it).
    static u64 mulhi64(u64 a, u64 b) noexcept {
#if defined(__SIZEOF_INT128__)
        return static_cast<u64>((static_cast<u128_t>(a) * b) >> 64);
#else
        const u64 alo = a & 0xFFFFFFFFull, ahi = a >> 32;
        const u64 blo = b & 0xFFFFFFFFull, bhi = b >> 32;
        const u64 p0 = alo * blo;
        const u64 p1 = alo * bhi;
        const u64 p2 = ahi * blo;
        const u64 p3 = ahi * bhi;
        u64 mid = p1 + (p0 >> 32);        // no overflow: p1 + (2^32-2) < 2^64
        mid += p2;                        // may wrap; a wrap is exactly +2^64 = +2^32 to the high word
        const u64 carry = mid < p2 ? (1ull << 32) : 0;
        return p3 + (mid >> 32) + carry;
#endif
    }

    u64 bounded(u64 n) noexcept { return mulhi64(next(), n); }

    // Uniform double in [0, 1). Draw value only, never stored in comparable state.
    double unit() noexcept {
        return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
    }

    bool chance(double p) noexcept { return unit() < p; }
};

}  // namespace arbiter