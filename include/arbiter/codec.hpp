#pragma once
// Bounds-checked byte codec shared by the wire protocol, the journal, and
// snapshots. Readers never throw and never read out of bounds; any underrun
// latches a failure flag the caller must check.
//
// ENDIANNESS: values are memcpy'd in NATIVE byte order, which is
// little-endian on every supported target (arm64, x86_64). All persisted
// formats (journal, snapshots, wire) therefore assume a little-endian host;
// a big-endian port would need explicit byte swaps here (documented
// limitation; same-arch determinism is the contract, docs/DETERMINISM.md).
//
// NOTE: methods are named put_*/get_*, naming them after the scalar types
// would shadow the type aliases inside the class scope.

#include "arbiter/errors.hpp"
#include "arbiter/types.hpp"

#include <cstring>
#include <string>
#include <string_view>

namespace arbiter {

class ByteWriter {
public:
    void put_u8(u8 v) { buf_.push_back(static_cast<char>(v)); }
    // NOTE: resize() zero-fills the appended region, so always resize FIRST
    // and memcpy into the reserved range afterwards.
    void put_u16(u16 v) { const size_t p = buf_.size(); buf_.resize(p + 2); std::memcpy(buf_.data() + p, &v, 2); }
    void put_u32(u32 v) { const size_t p = buf_.size(); buf_.resize(p + 4); std::memcpy(buf_.data() + p, &v, 4); }
    void put_u64(u64 v) { const size_t p = buf_.size(); buf_.resize(p + 8); std::memcpy(buf_.data() + p, &v, 8); }
    void put_i64(i64 v) { put_u64(static_cast<u64>(v)); }
    void put_bytes(const void* p, size_t n) {
        const size_t old = buf_.size();
        buf_.resize(old + n);
        std::memcpy(buf_.data() + old, p, n);
    }
    void put_str(std::string_view s) {
        put_u64(s.size());
        put_bytes(s.data(), s.size());
    }

    const std::string& data() const noexcept { return buf_; }
    std::string take() noexcept { return std::move(buf_); }
    size_t size() const noexcept { return buf_.size(); }
    void clear() noexcept { buf_.clear(); }

private:
    std::string buf_;
};

class ByteReader {
public:
    ByteReader(const u8* p, size_t n) noexcept : p_(p), n_(n) {}
    explicit ByteReader(const std::string& s) noexcept
        : p_(reinterpret_cast<const u8*>(s.data())), n_(s.size()) {}

    bool ok() const noexcept { return ok_; }
    size_t pos() const noexcept { return pos_; }
    size_t remaining() const noexcept { return n_ - pos_; }

    u8 get_u8() { return static_cast<u8>(get<1>()); }
    u16 get_u16() { return static_cast<u16>(get<2>()); }
    u32 get_u32() { return static_cast<u32>(get<4>()); }
    u64 get_u64() { return get<8>(); }
    i64 get_i64() { return static_cast<i64>(get<8>()); }

    std::string_view get_str() {
        const u64 n = get_u64();
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return {};
        }
        std::string_view s(reinterpret_cast<const char*>(p_ + pos_), static_cast<size_t>(n));
        pos_ += static_cast<size_t>(n);
        return s;
    }

    bool get_bytes(void* out, size_t n) {
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return false;
        }
        std::memcpy(out, p_ + pos_, n);
        pos_ += n;
        return true;
    }

private:
    template <size_t N>
    u64 get() {
        if (!ok_ || remaining() < N) {
            ok_ = false;
            return 0;
        }
        u64 v = 0;
        std::memcpy(&v, p_ + pos_, N);
        pos_ += N;
        return v;
    }
    const u8* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace arbiter