#pragma once
// Canonical state hashing.
//
// FNV-1a 64 with explicit domain tags and fixed field order. This is NOT a
// cryptographic hash: it is the run-equality and first-divergence-detection
// mechanism. Any change to a hashed representation must bump the domain tag
// so old hashes are never silently compared against new ones.

#include "arbiter/types.hpp"
#include <string_view>

namespace arbiter {

struct Hasher {
    u64 h = 0xcbf29ce484222325ull;

    void raw(const void* p, size_t n) noexcept {
        const u8* b = static_cast<const u8*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 0x100000001b3ull;
        }
    }
    void u64v(u64 v) noexcept { raw(&v, sizeof v); }
    void i64v(i64 v) noexcept { u64v(static_cast<u64>(v)); }
    void u32v(u32 v) noexcept { raw(&v, sizeof v); }
    void u16v(u16 v) noexcept { raw(&v, sizeof v); }
    void u8v(u8 v) noexcept { raw(&v, 1); }
    void boolean(bool v) noexcept { u8v(v ? 1 : 0); }
    void str(std::string_view s) noexcept {
        u64v(static_cast<u64>(s.size()));
        raw(s.data(), s.size());
    }
    // Domain separator: fixed string, no length prefix.
    void tag(std::string_view s) noexcept { raw(s.data(), s.size()); }

    u64 value() const noexcept { return h; }
};

}  // namespace arbiter