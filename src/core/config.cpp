#include "arbiter/config.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/hash.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

namespace arbiter {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

Config Config::from_string(std::string_view text, Status* st) {
    Config cfg;
    std::string section;
    size_t lineno = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        std::string_view line = text.substr(pos, eol == std::string_view::npos ? text.size() - pos : eol - pos);
        pos = (eol == std::string_view::npos) ? text.size() + 1 : eol + 1;
        ++lineno;
        // strip comments
        size_t hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        size_t semi = line.find(';');
        if (semi != std::string_view::npos) line = line.substr(0, semi);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') {
                if (st) *st = fail(Errc::ConfigInvalid, lineno);
                return cfg;
            }
            section = std::string(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (st) *st = fail(Errc::ConfigInvalid, lineno);
            return cfg;
        }
        std::string key(trim(line.substr(0, eq)));
        std::string value(trim(line.substr(eq + 1)));
        // A section is a namespace: ALL its keys are prefixed (dotted or
        // not). Fully-qualified keys are written at top level (no section).
        if (!section.empty()) key = section + "." + key;
        cfg.entries_.emplace_back(key, value);
        cfg.index_[key] = value;
    }
    if (st) *st = good();
    return cfg;
}

Result<Config> Config::from_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return ErrT<Config>(Errc::NotFound, static_cast<u64>(errno));
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) return ErrT<Config>(Errc::IoError, 0);
    Status st;
    Config cfg = Config::from_string(text, &st);
    if (!st) return ErrT<Config>(st.code, st.context);
    return OkT(std::move(cfg));
}

bool Config::has(std::string_view dotted) const noexcept {
    return index_.count(std::string(dotted)) != 0;
}

bool Config::get_bool(std::string_view dotted, bool dflt) const noexcept {
    auto it = index_.find(std::string(dotted));
    if (it == index_.end()) return dflt;
    std::string_view v = it->second;
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return dflt;
}

i64 Config::get_i64(std::string_view dotted, i64 dflt) const noexcept {
    auto it = index_.find(std::string(dotted));
    if (it == index_.end()) return dflt;
    char* end = nullptr;
    long long v = std::strtoll(it->second.c_str(), &end, 10);
    if (end == it->second.c_str()) return dflt;
    return static_cast<i64>(v);
}

u64 Config::get_u64(std::string_view dotted, u64 dflt) const noexcept {
    auto it = index_.find(std::string(dotted));
    if (it == index_.end()) return dflt;
    char* end = nullptr;
    unsigned long long v = std::strtoull(it->second.c_str(), &end, 10);
    if (end == it->second.c_str()) return dflt;
    return static_cast<u64>(v);
}

double Config::get_f64(std::string_view dotted, double dflt) const noexcept {
    auto it = index_.find(std::string(dotted));
    if (it == index_.end()) return dflt;
    char* end = nullptr;
    double v = std::strtod(it->second.c_str(), &end);
    if (end == it->second.c_str()) return dflt;
    return v;
}

std::string Config::get_str(std::string_view dotted, std::string dflt) const noexcept {
    auto it = index_.find(std::string(dotted));
    if (it == index_.end()) return dflt;
    return it->second;
}

std::string Config::render() const {
    std::vector<std::pair<std::string, std::string>> sorted(entries_.begin(), entries_.end());
    std::sort(sorted.begin(), sorted.end());
    std::string out;
    for (auto& [k, v] : sorted) {
        out += k;
        out += " = ";
        out += v;
        out += '\n';
    }
    return out;
}

u64 Config::hash() const {
    Hasher h;
    h.tag("ARBITERX/CONFIG/1");
    h.str(render());
    return h.value();
}

}  // namespace arbiter