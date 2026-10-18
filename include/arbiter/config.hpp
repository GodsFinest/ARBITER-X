#pragma once
// Minimal sectioned key=value configuration parser (deterministic, dependency
// free). Deliberately NOT YAML: the experiment format needs to be parseable by
// 200 lines of auditable C++ with no external dependencies. See
// docs/COUNTERFACTUALS.md for the experiment config grammar.
//
//   # comment
//   [experiment]
//   name = latency_sensitivity
//   seed = 42
//   execution.algo_latency_ns = 800        # sections namespace their keys:
//                                          # this is experiment.execution.algo_latency_ns
//
// Values are stored as strings; typed getters parse. Unknown keys are ignored
// by the parser but detectable via keys() for strict validation.

#include "arbiter/errors.hpp"
#include "arbiter/types.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arbiter {

class Config {
public:
    static Config from_string(std::string_view text, Status* st = nullptr);
    static Result<Config> from_file(const std::string& path);

    bool get_bool(std::string_view dotted, bool dflt) const noexcept;
    i64 get_i64(std::string_view dotted, i64 dflt) const noexcept;
    u64 get_u64(std::string_view dotted, u64 dflt) const noexcept;
    double get_f64(std::string_view dotted, double dflt) const noexcept;
    std::string get_str(std::string_view dotted, std::string dflt) const noexcept;
    bool has(std::string_view dotted) const noexcept;

    const std::vector<std::pair<std::string, std::string>>& entries() const noexcept { return entries_; }

    // Canonical text rendering (sorted) for artifacts.
    std::string render() const;
    // Hash of the canonical rendering, config identity for experiments.
    u64 hash() const;

private:
    std::vector<std::pair<std::string, std::string>> entries_;  // insertion order
    std::unordered_map<std::string, std::string> index_;
};

}  // namespace arbiter