// Wall-clock benchmark driver. Simulation behavior never depends on these
// measurements; see docs/PERFORMANCE.md for interpretation rules.

#include "arbiter/book_common.hpp"
#include "arbiter/data.hpp"
#include "arbiter/differential.hpp"
#include "arbiter/features.hpp"
#include "arbiter/histogram.hpp"
#include "arbiter/journal.hpp"
#include "arbiter/opt_book.hpp"
#include "arbiter/pipeline.hpp"
#include "arbiter/ref_book.hpp"
#include "arbiter/rng.hpp"
#include "arbiter/spsc.hpp"
#include "arbiter/wire.hpp"
#include "arbiter/world.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

using namespace arbiter;
using Clock = std::chrono::steady_clock;

namespace {

bool g_quick = false;

// ---- allocation probe: counts operator new calls in scoped sections ----
std::atomic<u64> g_alloc_count{0};

struct AllocSection {
    const char* name;
    u64 start;
    explicit AllocSection(const char* n) : name(n), start(g_alloc_count.load()) {}
    ~AllocSection() {
        const u64 delta = g_alloc_count.load() - start;
        std::printf("    allocations during %-34s : %llu\n", name, (unsigned long long)delta);
    }
};

u64 now_ns() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

#ifdef __APPLE__
std::string apple_sysctl_string(const char* name) {
    char buf[256];
    size_t len = sizeof buf;
    if (sysctlbyname(name, buf, &len, nullptr, 0) == 0) {
        if (len == sizeof(int)) {
            int value = 0;
            std::memcpy(&value, buf, sizeof value);
            return std::to_string(value);
        }
        if (len == sizeof(long long)) {
            long long value = 0;
            std::memcpy(&value, buf, sizeof value);
            return std::to_string(value);
        }
        return std::string(buf, len - 1);
    }
    return "unknown";
}
#endif

std::string cpu_model() {
#ifdef __APPLE__
    return apple_sysctl_string("machdep.cpu.brand_string");
#elif defined(__linux__)
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        if (key.find("model name") == std::string::npos && key.find("Hardware") == std::string::npos)
            continue;
        const size_t value = line.find_first_not_of(" \t", colon + 1);
        return value == std::string::npos ? "unknown" : line.substr(value);
    }
#endif
    return "unknown";
}

std::string ram_bytes() {
#ifdef __APPLE__
    return apple_sysctl_string("hw.memsize");
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0)
        return std::to_string(static_cast<unsigned long long>(pages) *
                              static_cast<unsigned long long>(page_size));
#endif
    return "unknown";
}

void print_env() {
    std::printf("=== environment ===\n");
    std::printf("  cpu          : %s\n", cpu_model().c_str());
    std::printf("  cores        : %u\n", std::thread::hardware_concurrency());
    std::printf("  ram_bytes    : %s\n", ram_bytes().c_str());
#if defined(__clang__)
    constexpr const char* compiler = "clang " __clang_version__;
#elif defined(__GNUC__)
    constexpr const char* compiler = "gcc " __VERSION__;
#else
    constexpr const char* compiler = "unknown compiler";
#endif
#if __cplusplus >= 202100L
    constexpr const char* language = "C++23";
#elif __cplusplus >= 202002L
    constexpr const char* language = "C++20";
#else
    constexpr const char* language = "pre-C++20";
#endif
#if defined(NDEBUG)
    std::printf("  build        : Release (%s; %s, __cplusplus=%ld)\n", compiler, language,
                static_cast<long>(__cplusplus));
#else
    std::printf("  build        : non-Release (%s; %s, __cplusplus=%ld) — NUMBERS NOT COMPARABLE\n",
                compiler, language, static_cast<long>(__cplusplus));
#endif
    std::printf("  arch         : %s\n",
#if defined(__aarch64__)
                "arm64"
#elif defined(__x86_64__)
                "x86_64"
#else
                "unknown"
#endif
    );
    std::printf("  note         : PMU counters are not collected; wall-clock only\n");
}

void report(const char* name, const Histogram& h) {
    const auto s = h.summarize();
    std::printf("  %-34s n=%-9llu p50=%7.0fns p90=%7.0fns p99=%7.0fns p99.9=%9.0fns max=%9.0fns  ops/s=%9.0f\n",
                name, (unsigned long long)s.count, s.p50, s.p90, s.p99, s.p999, s.mx,
                s.mean > 0 ? 1e9 / s.mean : 0.0);
}

// ---- workload generators ----

enum class BookOp { Add, Cancel, Taker };

struct WorkloadOp {
    BookOp kind = BookOp::Add;
    Order order{};
    u64 target = 0;
};

struct Workload {
    std::vector<WorkloadOp> ops;
};

// Realistic two-sided churn inside a 100-tick window around 1500.
Workload make_workload(u64 seed, size_t n) {
    Rng rng = Rng::stream(seed, 0x1094);
    Workload w;
    w.ops.reserve(n);
    std::vector<u64> live;  // optimistically tracked ids
    u64 next_id = 1;
    for (size_t i = 0; i < n; ++i) {
        const u64 roll = rng.bounded(100);
        if (roll < 45 || live.empty()) {
            Order o;
            o.id = next_id++;
            o.account = 1;
            o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
            o.type = OrderType::Limit;
            o.tif = Tif::GTC;
            o.price = 1450 + static_cast<Price>(rng.bounded(101));
            o.qty = 1 + rng.bounded(20);
            o.leaves = o.qty;
            live.push_back(o.id);
            w.ops.push_back({BookOp::Add, o, 0});
        } else if (roll < 90) {
            const size_t idx = rng.bounded(live.size());
            w.ops.push_back({BookOp::Cancel, {}, live[idx]});
            live[idx] = live.back();
            live.pop_back();
        } else {
            Order o;
            o.id = next_id++;
            o.account = 2;
            o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
            o.type = OrderType::Market;
            o.tif = Tif::IOC;
            o.qty = 1 + rng.bounded(30);
            o.leaves = o.qty;
            w.ops.push_back({BookOp::Taker, o, 0});
        }
    }
    return w;
}

template <typename Book>
void run_book_workload(Book& book, const Workload& wl, u64 id_offset, Histogram* sample) {
    std::vector<Fill> fills;
    fills.reserve(16);
    for (size_t i = 0; i < wl.ops.size(); ++i) {
        const WorkloadOp& op = wl.ops[i];
        const u64 t0 = (sample && (i & 7) == 0) ? now_ns() : 0;
        switch (op.kind) {
            case BookOp::Add: {
                Order o = op.order;
                o.id += id_offset;
                book.add_order(o, fills, nullptr);
                break;
            }
            case BookOp::Cancel:
                book.cancel(op.target + id_offset);
                break;
            case BookOp::Taker: {
                Order o = op.order;
                o.id += id_offset;
                book.add_order(o, fills, nullptr);
                break;
            }
        }
        fills.clear();
        if (t0) sample->record(static_cast<double>(now_ns() - t0));
    }
}

void bench_books(const Workload& wl) {
    std::printf("=== order book micro (add/cancel/taker mix, %zu ops, sampled every 8th op) ===\n", wl.ops.size());
    {
        RefBook book{RefBook::Options{}};
        Histogram h(1u << 21);
        run_book_workload(book, wl, 0, nullptr);          // warm (grows nodes)
        const u64 t0 = now_ns();
        run_book_workload(book, wl, 10'000'000, &h);      // measured (steady state)
        const u64 total = now_ns() - t0;
        report("reference book (std::map/list)", h);
        std::printf("    aggregate: %.0f ops/s\n", static_cast<double>(wl.ops.size()) / (total * 1e-9));
    }
    {
        OptBook book{OptBook::Options{1, 2000, 1u << 15}};
        Histogram h(1u << 21);
        run_book_workload(book, wl, 0, nullptr);
        const u64 t0 = now_ns();
        run_book_workload(book, wl, 10'000'000, &h);
        const u64 total = now_ns() - t0;
        report("optimized book (ladder+pool)", h);
        std::printf("    aggregate: %.0f ops/s\n", static_cast<double>(wl.ops.size()) / (total * 1e-9));
    }
}

void bench_protocol() {
    std::printf("=== wire protocol ===\n");
    Event e;
    e.type = EvType::MdNew;
    e.a = 77;
    e.c = 25;
    e.p = 1501;
    e.side = Side::Sell;
    const std::string frame = wire_encode(1, 1, kFlagNone, e);
    Histogram h_enc(1u << 20), h_dec(1u << 20);
    const u64 iters = g_quick ? 2'000 : 300'000;
    for (u64 i = 0; i < iters; ++i) {
        const u64 a = now_ns();
        volatile std::string f = wire_encode(1, i, kFlagNone, e);
        (void)f;
        h_enc.record(static_cast<double>(now_ns() - a));
    }
    for (u64 i = 0; i < iters; ++i) {
        const u64 a = now_ns();
        volatile auto r = wire_decode(reinterpret_cast<const u8*>(frame.data()), frame.size());
        (void)r;
        h_dec.record(static_cast<double>(now_ns() - a));
    }
    report("encode frame", h_enc);
    report("decode frame", h_dec);
}

void bench_journal() {
    std::printf("=== journal ===\n");
    const std::string path = "artifacts/bench.journal";
    std::filesystem::create_directories("artifacts");
    std::remove(path.c_str());
    Event e;
    e.type = EvType::MdTrade;
    e.p = 1500;
    e.a = 100;
    const u64 n = g_quick ? 2'000 : 200'000;
    Histogram h_write(1u << 20);
    {
        auto w = JournalWriter::create(path, 1, 1);
        for (u64 i = 0; i < n; ++i) {
            const u64 a = now_ns();
            w.value.append(e);
            h_write.record(static_cast<double>(now_ns() - a));
        }
        w.value.sync();
    }
    report("append+buffer event", h_write);
    Histogram h_read(1u << 20);
    {
        auto r = JournalReader::open(path);
        u64 count = 0;
        while (r.value.has_next()) {
            const u64 a = now_ns();
            auto ev = r.value.next();
            h_read.record(static_cast<double>(now_ns() - a));
            if (!ev) break;
            ++count;
        }
        std::printf("    replayed %llu events\n", (unsigned long long)count);
    }
    report("read+verify event", h_read);
    const auto size = std::filesystem::file_size(path);
    std::printf("    file: %llu bytes (%.1f bytes/event)\n", (unsigned long long)size,
                static_cast<double>(size) / static_cast<double>(n));
}

void bench_spsc_roundtrip() {
    std::printf("=== spsc ring cross-thread ping-pong latency ===\n");
    SpscRing<u64> a2b{1024}, b2a{1024};
    Histogram h(1u << 20);
    const u64 n = g_quick ? 5'000 : 200'000;
    std::thread other([&] {
        u64 v;
        for (u64 i = 0; i < n; ++i) {
            while (!a2b.pop(v)) {}
            while (!b2a.push(v)) {}
        }
    });
    for (u64 i = 0; i < n; ++i) {
        const u64 t0 = now_ns();
        while (!a2b.push(i)) {}
        u64 v;
        while (!b2a.pop(v)) {}
        h.record(static_cast<double>(now_ns() - t0));
    }
    other.join();
    report("roundtrip (2 threads)", h);
}

void bench_world_e2e() {
    std::printf("=== end-to-end world (full pipeline, journal off) ===\n");
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = g_quick ? 10'000'000 : 2'000'000'000;
    // warmup
    {
        World w(cfg, 1);
        RunStats s = w.run();
        (void)s;
    }
    const u64 t0 = now_ns();
    World w(cfg, 12345);
    RunStats s = w.run();
    const u64 wall = now_ns() - t0;
    std::printf("    events=%llu trades=%llu logical=%lldns wall=%.1fms -> %s\n",
                (unsigned long long)s.events_processed, (unsigned long long)s.trades,
                (long long)s.logical_end, wall / 1e6,
                "events/sec below");
    std::printf("    throughput: %.0f events/s, %.0f trades/s (single thread)\n",
                static_cast<double>(s.events_processed) / (wall * 1e-9),
                static_cast<double>(s.trades) / (wall * 1e-9));
}

void bench_instrumentation_overhead() {
    // How much does the deterministic instrumentation cost? Journaling
    // (CRC-framed append per event) and the hash timeline (state hash every
    // `hash_every` events) are the two always-available observation layers.
    // Same world, three configurations, identical logical work.
    std::printf("=== instrumentation overhead (same world, observation layers on/off) ===\n");
    const LogicalNs horizon = g_quick ? 5'000'000 : 500'000'000;
    auto run_one = [&](const char* label, bool journal, u64 hash_every) {
        WorldConfig cfg;
        cfg.journal_events = journal;
        cfg.journal_path = journal ? "artifacts/bench_instr.journal" : "";
        cfg.horizon_ns = horizon;
        cfg.hash_every_events = hash_every;
        std::remove(cfg.journal_path.c_str());
        World w(cfg, 31415);
        const u64 t0 = now_ns();
        RunStats s = w.run();
        const u64 wall = now_ns() - t0;
        std::printf("  %-28s events=%-8llu hashpts=%-6llu records=%-8llu wall=%7.1fms  %8.0f events/s\n",
                    label, (unsigned long long)s.events_processed,
                    (unsigned long long)s.hash_timeline.size(),
                    (unsigned long long)s.journal_records, wall / 1e6,
                    static_cast<double>(s.events_processed) / (wall * 1e-9));
        (void)s;
    };
    {  // warmup both paths before measuring
        WorldConfig cfg;
        cfg.journal_events = false;
        cfg.horizon_ns = g_quick ? 1'000'000 : 50'000'000;
        World w(cfg, 1);
        w.run();
        cfg.journal_events = true;
        cfg.journal_path = "artifacts/bench_instr.journal";
        World w2(cfg, 1);
        w2.run();
    }
    run_one("baseline (no journal, no hash)", false, 0);
    run_one("hash timeline only (every 256)", false, 256);
    run_one("journal only", true, 0);
    run_one("journal + hash timeline", true, 256);
    std::remove("artifacts/bench_instr.journal");
    std::printf("  note: measurement is wall-clock on one host; relative ordering is the signal\n");
}

void bench_research() {
    // Research-layer throughput (deterministic synthetic input; wall-clock).
    // Never confuse these with trading performance, they measure analysis
    // cost per event, the latency-budget input for §70 (decay vs compute).
    std::printf("=== research layer (synthetic L2, requested events=%zu) ===\n",
                g_quick ? size_t{500} : size_t{20'000});
    pipeline::SyntheticConfig sc;
    sc.seed = 2026;
    sc.n_events = g_quick ? 500 : 20000;
    auto evs = pipeline::synthesize(sc);
    u64 t0 = now_ns();
    auto series = pipeline::build_series(evs);
    u64 t1 = now_ns();
    // feature pass (as-of, feat_v1)
    features::OfiAccumulator ofi;
    features::FlowWindows flow;
    features::MidSeries mids;
    Histogram fh(1u << 21);
    for (const auto& s : series) {
        features::BookDelta d{s.ladder.bids.empty() ? 0 : s.ladder.bids.front().price,
                              s.ladder.asks.empty() ? 0 : s.ladder.asks.front().price,
                              s.ladder.bids.empty() ? 0 : s.ladder.bids.front().total_qty,
                              s.ladder.asks.empty() ? 0 : s.ladder.asks.front().total_qty};
        ofi.observe(s.ts_ns, d);
        if (s.mid > 0) mids.observe(s.ts_ns, s.mid);
        if (s.mid <= 0 || s.spread_rel < 0) continue;
        const u64 a = now_ns();
        volatile double sink =
            features::compute_v1(s.ladder, ofi, flow, mids, s.ts_ns).get("imb_1");
        (void)sink;
        fh.record(static_cast<double>(now_ns() - a));
    }
    pipeline::StudyConfig cfg;
    cfg.experiment_id = "BENCH";
    cfg.seed = 2026;
    const u64 t3 = now_ns();
    pipeline::StudyResult st = pipeline::run_study(series, cfg);
    const u64 t4 = now_ns();
    (void)st;
    const auto fs = fh.summarize();
    std::printf("    series build : %llu events -> %llu samples in %.1fms (%.0f events/s)\n",
                (unsigned long long)evs.size(), (unsigned long long)series.size(),
                (t1 - t0) / 1e6, evs.size() / ((t1 - t0) * 1e-9));
    std::printf("    feature v1   : p50=%.0fns p99=%.0fns (%.0f features/s)\n", fs.p50, fs.p99,
                1e9 / (fs.mean > 0 ? fs.mean : 1));
    std::printf("    full study   : %.1fms end-to-end (features+labels+models+execution)\n",
                (t4 - t3) / 1e6);
}

void bench_allocations(size_t n) {
    std::printf("=== allocation probe (steady-state hot path) ===\n");
    {
        AllocSection sec("setup: build workload");
        make_workload(7, n);
    }
    Workload wl = make_workload(7, n);
    {
        OptBook book{OptBook::Options{1, 2000, 1u << 15}};
        run_book_workload(book, wl, 0, nullptr);  // warm pass: grows pool + index
        AllocSection sec("opt book pass2 (grown structures)");
        run_book_workload(book, wl, 10'000'000, nullptr);
    }
    {
        RefBook ref{RefBook::Options{}};
        run_book_workload(ref, wl, 0, nullptr);
        AllocSection sec("ref book pass2 (std::map/list)");
        run_book_workload(ref, wl, 10'000'000, nullptr);
    }
}

}  // namespace

// operator new/delete interception for the allocation probe
void* operator new(std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    const char* suite = argc > 1 ? argv[1] : "all";
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            g_quick = true;
        } else {
            std::fprintf(stderr, "unknown benchmark option: %s\n", argv[i]);
            return 2;
        }
    }
    if (std::strcmp(suite, "help") == 0 || std::strcmp(suite, "--help") == 0 ||
        std::strcmp(suite, "-h") == 0) {
        std::printf("usage: arbiter_bench [all|exchange|protocol|journal|pipeline] [--quick]\n");
        return 0;
    }
    if (std::strcmp(suite, "all") != 0 && std::strcmp(suite, "exchange") != 0 &&
        std::strcmp(suite, "protocol") != 0 && std::strcmp(suite, "journal") != 0 &&
        std::strcmp(suite, "pipeline") != 0) {
        std::fprintf(stderr, "unknown benchmark suite: %s\n", suite);
        return 2;
    }
    print_env();
    std::filesystem::create_directories("artifacts");
    if (std::strcmp(suite, "all") == 0 || std::strcmp(suite, "exchange") == 0) {
        Workload wl = make_workload(2026, g_quick ? 5'000 : 500'000);
        bench_books(wl);
        bench_allocations(g_quick ? 2'000 : 200'000);
    }
    if (std::strcmp(suite, "all") == 0 || std::strcmp(suite, "protocol") == 0) bench_protocol();
    if (std::strcmp(suite, "all") == 0 || std::strcmp(suite, "journal") == 0) bench_journal();
    if (std::strcmp(suite, "all") == 0 || std::strcmp(suite, "pipeline") == 0) {
        bench_world_e2e();
        bench_instrumentation_overhead();
        bench_research();
        bench_spsc_roundtrip();
    }
    std::printf("done.\n");
    return 0;
}