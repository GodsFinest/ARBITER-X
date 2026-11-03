#pragma once
#include <string>
namespace arbiter::build {
std::string compiler_id();
std::string platform_id();
std::string utc_now();
const char* version() noexcept;
const char* revision() noexcept;
const char* source_state() noexcept;
}
