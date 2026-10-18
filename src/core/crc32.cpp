#include "arbiter/crc32.hpp"

namespace arbiter {

namespace {

struct CrcTable {
    u32 t[256];
    constexpr CrcTable() noexcept : t{} {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
constexpr CrcTable kCrc{};

}  // namespace

u32 crc32(const void* data, size_t len, u32 seed) noexcept {
    const u8* p = static_cast<const u8*>(data);
    u32 c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = kCrc.t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

u32 crc32_two(const void* a, size_t alen, const void* b, size_t blen) noexcept {
    u32 c = 0xFFFFFFFFu;
    const u8* pa = static_cast<const u8*>(a);
    const u8* pb = static_cast<const u8*>(b);
    for (size_t i = 0; i < alen; ++i) c = kCrc.t[(c ^ pa[i]) & 0xFFu] ^ (c >> 8);
    for (size_t i = 0; i < blen; ++i) c = kCrc.t[(c ^ pb[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace arbiter