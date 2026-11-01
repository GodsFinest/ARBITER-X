// Research manifests, artifact hashing, report rendering.
#include "arbiter/research.hpp"
#include "arbiter/hash.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>

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

namespace arbiter {
namespace research {

namespace {
int json_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_json_utf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool parse_json_string_at(const std::string& text, size_t opening, std::string& out,
                          size_t& after) {
    if (opening >= text.size() || text[opening] != '"') return false;
    out.clear();
    for (size_t i = opening + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            after = i + 1;
            return true;
        }
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i >= text.size()) return false;
        switch (text[i]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 >= text.size()) return false;
                unsigned cp = 0;
                for (size_t j = 0; j < 4; ++j) {
                    const int digit = json_hex_digit(text[i + 1 + j]);
                    if (digit < 0) return false;
                    cp = (cp << 4) | static_cast<unsigned>(digit);
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDFFF) return false;
                append_json_utf8(out, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool json_string_field(const std::string& json, const std::string& key, std::string& out) {
    const std::string marker = "\"" + key + "\":";
    const size_t p = json.find(marker);
    if (p == std::string::npos) return false;
    size_t value = p + marker.size();
    while (value < json.size() && std::isspace(static_cast<unsigned char>(json[value]))) ++value;
    size_t after = 0;
    return parse_json_string_at(json, value, out, after);
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string format_double(double value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", value);
    return buf;
}
}  // namespace

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
    // informational only (never hashed into artifact identity)
    return std::string(__DATE__) + " " + __TIME__ + " UTC";
}

u64 ResearchManifest::compute_hash() const {
    Hasher h;
    h.tag("ARBITERX/RESEARCH-MANIFEST/3");
    h.str(run_id);
    h.str(experiment_id);
    h.u64v(dataset_hash);
    h.str(dataset_name);
    h.str(dataset_kind);
    h.u64v(synth_seed);
    h.u64v(static_cast<u64>(synth_events));
    // doubles hashed via decimal rendering (portable; never raw bits)
    h.str(format_double(synth_bias));
    h.str(format_double(imbalance_thresh));
    h.i64v(exec_latency_ns);
    h.i64v(fee_per_lot);
    h.u64v(static_cast<u64>(sample_every));
    h.u64v(static_cast<u64>(max_samples));
    h.u64v(config_hash);
    h.u64v(seed);
    h.str(feature_version);
    h.str(model_version);
    h.str(code_version);
    return h.value();
}

u64 ResearchResult::compute_result_hash() const {
    Hasher h;
    h.tag("ARBITERX/RESEARCH-RESULT/1");
    h.u64v(manifest.manifest_hash);
    for (const auto& [k, v] : metrics) {
        h.str(k);
        // hash the decimal representation (portable; doubles never hashed raw)
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        h.str(buf);
    }
    for (const auto& [k, v] : counters) {
        h.str(k);
        h.i64v(v);
    }
    return h.value();
}

std::string ResearchManifest::to_json() const {
    std::string out = "{\n  \"run_id\": \"" + json_escape(run_id) +
                      "\",\n  \"experiment_id\": \"" + json_escape(experiment_id) +
                      "\",\n  \"git_commit\": \"" + json_escape(git_commit) +
                      "\",\n  \"git_dirty\": " + (git_dirty ? "true" : "false") +
                      ",\n  \"dataset_hash\": " + std::to_string(dataset_hash) +
                      ",\n  \"dataset_name\": \"" + json_escape(dataset_name) +
                      "\",\n  \"dataset_kind\": \"" + json_escape(dataset_kind) +
                      "\",\n  \"synth_seed\": " + std::to_string(synth_seed) +
                      ",\n  \"synth_events\": " + std::to_string(synth_events) +
                      ",\n  \"synth_bias\": " + format_double(synth_bias) +
                      ",\n  \"imbalance_thresh\": " + format_double(imbalance_thresh) +
                      ",\n  \"exec_latency_ns\": " + std::to_string(exec_latency_ns) +
                      ",\n  \"fee_per_lot\": " + std::to_string(fee_per_lot) +
                      ",\n  \"sample_every\": " + std::to_string(sample_every) +
                      ",\n  \"max_samples\": " + std::to_string(max_samples) +
                      ",\n  \"config_hash\": " + std::to_string(config_hash) +
                      ",\n  \"seed\": " + std::to_string(seed) +
                      ",\n  \"feature_version\": \"" + json_escape(feature_version) +
                      "\",\n  \"model_version\": \"" + json_escape(model_version) +
                      "\",\n  \"code_version\": \"" + json_escape(code_version) +
                      "\",\n  \"compiler\": \"" + json_escape(compiler) +
                      "\",\n  \"platform\": \"" + json_escape(platform) +
                      "\",\n  \"created_utc\": \"" + json_escape(created_utc) +
                      "\",\n  \"manifest_hash\": " + std::to_string(manifest_hash) + "\n}";
    return out;
}

std::string ResearchResult::metrics_json() const {
    std::string s = "{\n  \"manifest\": ";
    s += manifest.to_json();
    s += ",\n  \"metrics\": {\n";
    for (size_t i = 0; i < metrics.size(); ++i) {
        s += "    \"" + json_escape(metrics[i].first) + "\": " +
             format_double(metrics[i].second) + (i + 1 < metrics.size() ? ",\n" : "\n");
    }
    s += "  },\n  \"counters\": {\n";
    for (size_t i = 0; i < counters.size(); ++i) {
        s += "    \"" + json_escape(counters[i].first) + "\": " +
             std::to_string(counters[i].second) +
             (i + 1 < counters.size() ? ",\n" : "\n");
    }
    s += "  },\n  \"findings\": [\n";
    for (size_t i = 0; i < findings.size(); ++i) {
        s += "    \"" + json_escape(findings[i]) + "\"" +
             (i + 1 < findings.size() ? ",\n" : "\n");
    }
    s += "  ],\n  \"result_hash\": " + std::to_string(result_hash) + "\n}\n";
    return s;
}

std::string render_report(const std::string& title, const std::string& question,
                          const std::string& hypothesis, const ResearchResult& r,
                          const std::vector<std::string>& sections) {
    std::string md = "# " + title + "\n\n";
    md += "## Question\n\n" + question + "\n\n";
    md += "## Hypothesis\n\n" + hypothesis + "\n\n";
    for (const auto& s : sections) md += s + "\n\n";
    md += "## Reproducibility manifest\n\n```json\n" + r.manifest.to_json() + "\n```\n";
    char rh[128];
    std::snprintf(rh, sizeof rh, "\nresult_hash: %llu\n", (unsigned long long)r.result_hash);
    md += rh;
    return md;
}

namespace {
bool write_file(const std::string& path, const std::string& content, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    const size_t w = std::fwrite(content.data(), 1, content.size(), f);
    const int close_rc = std::fclose(f);
    if (w != content.size() || close_rc != 0) {
        err = "short write " + path;
        return false;
    }
    return true;
}
std::string read_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
// Extract a top-level integer field from our own JSON (writer-reader symmetry;
// the files are machine-written by metrics_json, never hand-edited).
// Unsigned: hashes routinely exceed INT64_MAX (strtoll would clamp).
unsigned long long json_uint_field(const std::string& json, const std::string& key) {
    const std::string pat = "\"" + key + "\":";
    const size_t p = json.find(pat);
    if (p == std::string::npos) return 0;
    const char* s = json.c_str() + p + pat.size();
    while (*s == ' ') ++s;
    if (*s == '-') return 0;  // missing/sentinel: caller treats 0 carefully
    return std::strtoull(s, nullptr, 10);
}

bool section_bounds(const std::string& json, const std::string& key, size_t& begin, size_t& end) {
    const std::string marker = "\"" + key + "\": {";
    const size_t section = json.find(marker);
    if (section == std::string::npos) return false;
    begin = section + marker.size();
    end = json.find("\n  }", begin);
    return end != std::string::npos;
}

bool valid_number_suffix(const char* suffix) {
    while (std::isspace(static_cast<unsigned char>(*suffix))) ++suffix;
    if (*suffix == ',') ++suffix;
    while (std::isspace(static_cast<unsigned char>(*suffix))) ++suffix;
    return *suffix == '\0';
}

bool parse_metrics(const std::string& json,
                   std::vector<std::pair<std::string, double>>& metrics,
                   std::string& err) {
    size_t begin = 0, end = 0;
    if (!section_bounds(json, "metrics", begin, end)) {
        err = "results.json has no metrics object";
        return false;
    }
    size_t pos = begin;
    while (pos < end) {
        const size_t line_end = std::min(json.find('\n', pos), end);
        const std::string line = json.substr(pos, line_end - pos);
            const size_t key_begin = line.find('"');
        if (key_begin != std::string::npos) {
            std::string key;
            size_t after_key = 0;
            if (!parse_json_string_at(line, key_begin, key, after_key)) {
                err = "malformed metric entry";
                return false;
            }
            const size_t colon = line.find(':', after_key);
            if (colon == std::string::npos) {
                err = "malformed metric entry";
                return false;
            }
            const char* number = line.c_str() + colon + 1;
            char* parsed_end = nullptr;
            errno = 0;
            const double value = std::strtod(number, &parsed_end);
            if (parsed_end == number || errno == ERANGE || !std::isfinite(value) ||
                !valid_number_suffix(parsed_end)) {
                err = "invalid metric value for '" + key + "'";
                return false;
            }
            metrics.push_back({std::move(key), value});
        }
        pos = line_end + 1;
    }
    return true;
}

bool parse_counters(const std::string& json,
                    std::vector<std::pair<std::string, long long>>& counters,
                    std::string& err) {
    size_t begin = 0, end = 0;
    if (!section_bounds(json, "counters", begin, end)) {
        err = "results.json has no counters object";
        return false;
    }
    size_t pos = begin;
    while (pos < end) {
        const size_t line_end = std::min(json.find('\n', pos), end);
        const std::string line = json.substr(pos, line_end - pos);
            const size_t key_begin = line.find('"');
        if (key_begin != std::string::npos) {
            std::string key;
            size_t after_key = 0;
            if (!parse_json_string_at(line, key_begin, key, after_key)) {
                err = "malformed counter entry";
                return false;
            }
            const size_t colon = line.find(':', after_key);
            if (colon == std::string::npos) {
                err = "malformed counter entry";
                return false;
            }
            const char* number = line.c_str() + colon + 1;
            char* parsed_end = nullptr;
            errno = 0;
            const long long value = std::strtoll(number, &parsed_end, 10);
            if (parsed_end == number || errno == ERANGE || !valid_number_suffix(parsed_end)) {
                err = "invalid counter value for '" + key + "'";
                return false;
            }
            counters.push_back({std::move(key), value});
        }
        pos = line_end + 1;
    }
    return true;
}
}  // namespace

bool write_artifact(const std::string& exp_dir, const ResearchResult& r,
                    const std::string& report_md, std::string& err) {
    std::error_code ec;
    std::filesystem::create_directories(exp_dir, ec);
    if (ec) {
        err = "cannot create " + exp_dir + ": " + ec.message();
        return false;
    }
    if (!write_file(exp_dir + "/manifest.json", r.manifest.to_json() + "\n", err)) return false;
    if (!write_file(exp_dir + "/results.json", r.metrics_json(), err)) return false;
    if (!write_file(exp_dir + "/report.md", report_md, err)) return false;
    return true;
}

bool read_manifest(const std::string& exp_dir, ResearchManifest& m, u64& result_hash_out,
                   std::string& err) {
    const std::string man = read_file(exp_dir + "/manifest.json");
    const std::string res = read_file(exp_dir + "/results.json");
    if (man.empty() || res.empty()) {
        err = "missing artifact files in " + exp_dir;
        return false;
    }
    auto str_field = [&](const std::string& key) -> std::string {
        std::string value;
        return json_string_field(man, key, value) ? value : std::string{};
    };
    auto dbl_field = [&](const std::string& key) -> double {
        const std::string pat = "\"" + key + "\":";
        const size_t p = man.find(pat);
        if (p == std::string::npos) return 0.0;
        return std::strtod(man.c_str() + p + pat.size(), nullptr);
    };
    auto int_field = [&](const std::string& key) -> long long {
        const std::string pat = "\"" + key + "\":";
        const size_t p = man.find(pat);
        if (p == std::string::npos) return 0;
        return std::strtoll(man.c_str() + p + pat.size(), nullptr, 10);
    };
    m.run_id = str_field("run_id");
    m.experiment_id = str_field("experiment_id");
    m.git_commit = str_field("git_commit");
    {
        const std::string pat = "\"git_dirty\": true";
        m.git_dirty = man.find(pat) != std::string::npos;
    }
    m.dataset_hash = static_cast<u64>(json_uint_field(man, "dataset_hash"));
    m.dataset_name = str_field("dataset_name");
    m.dataset_kind = str_field("dataset_kind");
    if (m.dataset_kind.empty()) {
        err = "manifest version 1 (superseded by v2); regenerate with `arbiter research run " +
              m.experiment_id + "`";
        return false;
    }
    if (m.dataset_kind != "synthetic" && m.dataset_kind != "csv") {
        err = "unknown dataset_kind '" + m.dataset_kind + "' (artifact corrupt?)";
        return false;
    }
    m.synth_seed = static_cast<u64>(json_uint_field(man, "synth_seed"));
    m.synth_events = static_cast<size_t>(json_uint_field(man, "synth_events"));
    m.synth_bias = dbl_field("synth_bias");
    m.imbalance_thresh = dbl_field("imbalance_thresh");
    m.exec_latency_ns = int_field("exec_latency_ns");
    m.fee_per_lot = int_field("fee_per_lot");
    m.sample_every = static_cast<size_t>(json_uint_field(man, "sample_every"));
    if (m.sample_every == 0) m.sample_every = 1;
    m.max_samples = static_cast<size_t>(json_uint_field(man, "max_samples"));
    if (m.max_samples == 0) m.max_samples = 20000;
    m.config_hash = static_cast<u64>(json_uint_field(man, "config_hash"));
    m.seed = static_cast<u64>(json_uint_field(man, "seed"));
    m.feature_version = str_field("feature_version");
    m.model_version = str_field("model_version");
    m.code_version = str_field("code_version");
    m.compiler = str_field("compiler");
    m.platform = str_field("platform");
    m.manifest_hash = static_cast<u64>(json_uint_field(man, "manifest_hash"));
    if (m.compute_hash() != m.manifest_hash) {
        err = "manifest_hash MISMATCH: artifact fields were modified after writing";
        return false;
    }
    result_hash_out = static_cast<u64>(json_uint_field(res, "result_hash"));
    return true;
}

bool read_result(const std::string& exp_dir, ResearchResult& result, std::string& err) {
    u64 stored_hash = 0;
    if (!read_manifest(exp_dir, result.manifest, stored_hash, err)) return false;
    const std::string json = read_file(exp_dir + "/results.json");
    result.metrics.clear();
    result.counters.clear();
    if (!parse_metrics(json, result.metrics, err) || !parse_counters(json, result.counters, err))
        return false;
    result.result_hash = stored_hash;
    return true;
}

ResultComparison compare_result_values(const ResearchResult& stored,
                                       const ResearchResult& rebuilt,
                                       double relative_tolerance) {
    ResultComparison comparison;
    auto fail = [&](const std::string& detail) {
        if (comparison.first_difference.empty()) comparison.first_difference = detail;
        comparison.first_mismatch = detail;
        return comparison;
    };
    if (stored.metrics.size() != rebuilt.metrics.size())
        return fail("metric count: stored=" + std::to_string(stored.metrics.size()) +
                    " rebuilt=" + std::to_string(rebuilt.metrics.size()));
    for (size_t i = 0; i < stored.metrics.size(); ++i) {
        const auto& [stored_name, stored_value] = stored.metrics[i];
        const auto& [rebuilt_name, rebuilt_value] = rebuilt.metrics[i];
        if (stored_name != rebuilt_name)
            return fail("metric name at index " + std::to_string(i) + ": stored='" + stored_name +
                        "' rebuilt='" + rebuilt_name + "'");
        const double scale = std::max(1.0, std::max(std::fabs(stored_value),
                                                    std::fabs(rebuilt_value)));
        const double delta = std::fabs(stored_value - rebuilt_value);
        if (delta != 0.0 && comparison.first_difference.empty()) {
            char detail[320];
            std::snprintf(detail, sizeof detail,
                          "metric '%s': stored=%.17g rebuilt=%.17g delta=%.17g",
                          stored_name.c_str(), stored_value, rebuilt_value, delta);
            comparison.first_difference = detail;
        }
        if (delta > relative_tolerance * scale) {
            char detail[320];
            std::snprintf(detail, sizeof detail,
                          "metric '%s': stored=%.17g rebuilt=%.17g delta=%.17g tolerance=%.3g",
                          stored_name.c_str(), stored_value, rebuilt_value, delta,
                          relative_tolerance * scale);
            return fail(detail);
        }
    }
    if (stored.counters.size() != rebuilt.counters.size())
        return fail("counter count: stored=" + std::to_string(stored.counters.size()) +
                    " rebuilt=" + std::to_string(rebuilt.counters.size()));
    for (size_t i = 0; i < stored.counters.size(); ++i) {
        const auto& [stored_name, stored_value] = stored.counters[i];
        const auto& [rebuilt_name, rebuilt_value] = rebuilt.counters[i];
        if (stored_name != rebuilt_name)
            return fail("counter name at index " + std::to_string(i) + ": stored='" + stored_name +
                        "' rebuilt='" + rebuilt_name + "'");
        if (stored_value != rebuilt_value)
            return fail("counter '" + stored_name + "': stored=" + std::to_string(stored_value) +
                        " rebuilt=" + std::to_string(rebuilt_value));
    }
    comparison.ok = true;
    return comparison;
}

}  // namespace research
}  // namespace arbiter
