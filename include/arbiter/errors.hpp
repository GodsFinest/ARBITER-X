#pragma once
// Explicit error model. Core systems never throw across module boundaries and
// never swallow failures; every fallible operation returns a Status or Result.

#include "arbiter/types.hpp"
#include <string_view>
#include <utility>

namespace arbiter {

enum class Errc : u16 {
    Ok = 0,
    InvalidArgument,
    OutOfRange,            // price/qty outside domain bounds
    NotFound,              // unknown order id / missing file
    Duplicate,             // duplicate order id
    CrossedRejected,       // e.g. FOK limit impossible, post-only would cross
    InsufficientLiquidity, // FOK / fill-all impossible
    ChecksumMismatch,
    ProtocolInvalid,       // malformed wire message
    SequenceGap,
    JournalCorrupt,
    SnapshotInvalid,
    RiskRejected,
    RateLimited,
    Capacity,              // structural capacity exceeded
    Unsupported,           // not supported on this platform/build
    IoError,
    ConfigInvalid,
    Internal,
};

const char* to_cstr(Errc c) noexcept;

struct Status {
    Errc code = Errc::Ok;
    u64 context = 0;  // order id, sequence, byte offset — whatever localizes it

    explicit operator bool() const noexcept { return code == Errc::Ok; }
};

inline Status good() noexcept { return {}; }
inline Status fail(Errc c, u64 ctx = 0) noexcept { return Status{c, ctx}; }

// Minimal expected-like result for decode paths that must never throw.
template <class T>
struct Result {
    T value{};
    Status status{};

    explicit operator bool() const noexcept { return status.code == Errc::Ok; }
    T& operator*() noexcept { return value; }
    const T& operator*() const noexcept { return value; }
};

template <class T>
Result<T> OkT(T v) noexcept { return Result<T>{std::move(v), Status{}}; }
template <class T>
Result<T> ErrT(Errc c, u64 ctx = 0) noexcept { return Result<T>{T{}, Status{c, ctx}}; }

}  // namespace arbiter