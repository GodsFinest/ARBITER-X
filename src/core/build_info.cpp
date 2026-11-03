#include "arbiter/build_info.hpp"
#include "arbiter_build_info_config.hpp"
#include <chrono>
#include <ctime>
#if defined(__clang__)
#define AX_COMPILER_ID "clang " __clang_version__
#elif defined(__GNUC__)
#define AX_COMPILER_ID "gcc " __VERSION__
#else
#define AX_COMPILER_ID "unknown-compiler"
#endif
#if defined(__aarch64__) || defined(__arm64__)
#define AX_PLATFORM_ID "arm64"
#elif defined(__x86_64__)
#define AX_PLATFORM_ID "x86_64"
#else
#define AX_PLATFORM_ID "unknown-arch"
#endif

namespace arbiter::build {
std::string compiler_id() { return AX_COMPILER_ID; }
std::string platform_id() {
#if defined(__APPLE__)
    return std::string(AX_PLATFORM_ID) + "-apple";
#elif defined(__linux__)
    return std::string(AX_PLATFORM_ID) + "-linux";
#else
    return AX_PLATFORM_ID;
#endif
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    char result[32]{};
    std::strftime(result, sizeof(result), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return result;
}
const char* version() noexcept { return AX_BUILD_VERSION; }
const char* revision() noexcept { return AX_BUILD_REVISION; }
const char* source_state() noexcept { return AX_BUILD_SOURCE_STATE; }
}
