#pragma once
// CRC32 (IEEE 802.3 polynomial, reflected), integrity checksums for the
// journal, wire protocol, and snapshots. Table-driven, generated at compile
// time so behavior is identical everywhere.

#include "arbiter/types.hpp"
#include <cstddef>

namespace arbiter {

u32 crc32(const void* data, size_t len, u32 seed = 0) noexcept;

// Convenience for checksumming header+payload without copying.
u32 crc32_two(const void* a, size_t alen, const void* b, size_t blen) noexcept;

}  // namespace arbiter