#pragma once
// Internal failure injection for core tests; never installed as public API.
#include "arbiter/errors.hpp"
namespace arbiter::journal_test {
enum class JournalFailurePoint : u8 { Append, Sync };
using FailureHook = Status (*)(JournalFailurePoint);
void set_failure_hook_for_tests(FailureHook hook) noexcept;
}
