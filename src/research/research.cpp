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
    {
        // doubles hashed via decimal rendering (portable; never raw bits)
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", synth_bias);
        h.str(buf);
        std::snprintf(buf, sizeof buf, "%.17g", imbalance_thresh);
        h.str(buf);
    }
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
    char buf[4096];
    std::snprintf(buf, sizeof buf,
                  "{\n  \"run_id\": \"%s\",\n  \"experiment_id\": \"%s\",\n  \"git_commit\": \"%s\",\n"
                  "  \"dataset_hash\": %llu,\n  \"dataset_name\": \"%s\",\n"
                  "  \"dataset_kind\": \"%s\",\n  \"synth_seed\": %llu,\n  \"synth_events\": %llu,\n"
                  "  \"synth_bias\": %.17g,\n  \"imbalance_thresh\": %.17g,\n"
                  "  \"exec_latency_ns\": %lld,\n  \"fee_per_lot\": %lld,\n"
                  "  \"sample_every\": %llu,\n  \"max_samples\": %llu,\n"
                  "  \"config_hash\": %llu,\n  \"seed\": %llu,\n  \"feature_version\": \"%s\",\n"
                  "  \"model_version\": \"%s\",\n  \"code_version\": \"%s\",\n"
                  "  \"compiler\": \"%s\",\n  \"platform\": \"%s\",\n  \"created_utc\": \"%s\",\n"
                  "  \"manifest_hash\": %llu\n}",
                  run_id.c_str(), experiment_id.c_str(), git_commit.c_str(),
                  (unsigned long long)dataset_hash, dataset_name.c_str(),
                  dataset_kind.c_str(), (unsigned long long)synth_seed,
                  (unsigned long long)synth_events, synth_bias, imbalance_thresh,
                  (long long)exec_latency_ns, (long long)fee_per_lot,
                  (unsigned long long)sample_every, (unsigned long long)max_samples,
                  (unsigned long long)config_hash, (unsigned long long)seed,
                  feature_version.c_str(), model_version.c_str(), code_version.c_str(),
                  compiler.c_str(), platform.c_str(), created_utc.c_str(),
                  (unsigned long long)manifest_hash);
    return std::string(buf);
}

std::string ResearchResult::metrics_json() const {
    std::string s = "{\n  \"manifest\": ";
    s += manifest.to_json();
    s += ",\n  \"metrics\": {\n";
    for (size_t i = 0; i < metrics.size(); ++i) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "    \"%s\": %.17g%s\n", metrics[i].first.c_str(),
                      metrics[i].second, i + 1 < metrics.size() ? "," : "");
        s += buf;
    }
    s += "  },\n  \"counters\": {\n";
    for (size_t i = 0; i < counters.size(); ++i) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "    \"%s\": %lld%s\n", counters[i].first.c_str(),
                      counters[i].second, i + 1 < counters.size() ? "," : "");
        s += buf;
    }
    s += "  },\n  \"findings\": [\n";
    for (size_t i = 0; i < findings.size(); ++i) {
        s += "    \"" + findings[i] + "\"" + (i + 1 < findings.size() ? ",\n" : "\n");
    }
    char tail[128];
    std::snprintf(tail, sizeof tail, "  ],\n  \"result_hash\": %llu\n}\n",
                  (unsigned long long)result_hash);
    s += tail;
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
    std::fclose(f);
    if (w != content.size()) {
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
            const size_t key_end = line.find('"', key_begin + 1);
            const size_t colon = line.find(':', key_end);
            if (key_end == std::string::npos || colon == std::string::npos) {
                err = "malformed metric entry";
                return false;
            }
            const char* number = line.c_str() + colon + 1;
            char* parsed_end = nullptr;
            errno = 0;
            const double value = std::strtod(number, &parsed_end);
            if (parsed_end == number || errno == ERANGE || !std::isfinite(value) ||
                !valid_number_suffix(parsed_end)) {
                err = "invalid metric value for '" + line.substr(key_begin + 1, key_end - key_begin - 1) +
                      "'";
                return false;
            }
            metrics.push_back({line.substr(key_begin + 1, key_end - key_begin - 1), value});
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
            const size_t key_end = line.find('"', key_begin + 1);
            const size_t colon = line.find(':', key_end);
            if (key_end == std::string::npos || colon == std::string::npos) {
                err = "malformed counter entry";
                return false;
            }
            const char* number = line.c_str() + colon + 1;
            char* parsed_end = nullptr;
            errno = 0;
            const long long value = std::strtoll(number, &parsed_end, 10);
            if (parsed_end == number || errno == ERANGE || !valid_number_suffix(parsed_end)) {
                err = "invalid counter value for '" +
                      line.substr(key_begin + 1, key_end - key_begin - 1) + "'";
                return false;
            }
            counters.push_back({line.substr(key_begin + 1, key_end - key_begin - 1), value});
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
        const std::string pat = "\"" + key + "\": \"";
        const size_t p = man.find(pat);
        if (p == std::string::npos) return "";
        const size_t q = man.find('"', p + pat.size());
        return man.substr(p + pat.size(), q - p - pat.size());
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