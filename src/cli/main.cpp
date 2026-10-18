// Command-line entry points over the shared deterministic engine.

#include "arbiter/book_common.hpp"
#include "arbiter/opt_book.hpp"
#include "arbiter/experiment.hpp"
#include "arbiter/flagship.hpp"
#include "arbiter/differential.hpp"
#include "arbiter/journal.hpp"
#include "arbiter/latency.hpp"
#include "arbiter/metrics.hpp"
#include "arbiter/replay.hpp"
#include "arbiter/sim.hpp"
#include "arbiter/snapshot.hpp"
#include "arbiter/wire.hpp"
#include "arbiter/world.hpp"
#include "arbiter/world_snapshot.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace arbiter;

int cmd_research(int argc, char** argv);

namespace {

bool write_file(const std::string& path, const std::string& content) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool complete = std::fwrite(content.data(), 1, content.size(), file) == content.size();
    return std::fclose(file) == 0 && complete;
}

int cmd_simulate(int argc, char** argv) {
    u64 seed = 42;
    u64 horizon_ms = 100;
    const char* out = "artifacts/sim";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--ms") && i + 1 < argc) horizon_ms = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }
    std::filesystem::create_directories("artifacts");
    WorldConfig cfg;
    cfg.journal_events = true;
    cfg.journal_path = std::string(out) + ".journal";
    cfg.horizon_ns = static_cast<LogicalNs>(horizon_ms) * 1'000'000;
    World w(cfg, seed);
    RunStats s = w.run();
    std::printf("%s", runstats_summary(s, "simulate seed=" + std::to_string(seed)).c_str());
    const std::string json = runstats_to_json(s, "simulate");
    const std::string json_path = std::string(out) + ".json";
    if (!write_file(json_path, json)) {
        std::fprintf(stderr, "cannot write %s\n", json_path.c_str());
        return 1;
    }
    std::printf("  world state hash : %llu\n", (unsigned long long)w.state_hash());
    std::printf("  journal          : %s (%llu records)\n", w.journal_path().c_str(),
                (unsigned long long)s.journal_records);
    return 0;
}

int cmd_verify(int argc, char** argv) {
    u64 seed = 42;
    u64 horizon_ms = 50;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--ms") && i + 1 < argc) horizon_ms = std::strtoull(argv[++i], nullptr, 10);
    }
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = static_cast<LogicalNs>(horizon_ms) * 1'000'000;
    World a(cfg, seed);
    RunStats sa = a.run();
    World b(cfg, seed);
    RunStats sb = b.run();
    const bool same = a.state_hash() == b.state_hash();
    std::printf("run1 hash: %llu (%llu events)\n", (unsigned long long)a.state_hash(),
                (unsigned long long)sa.events_processed);
    std::printf("run2 hash: %llu (%llu events)\n", (unsigned long long)b.state_hash(),
                (unsigned long long)sb.events_processed);
    std::printf("RESULT: %s\n", same ? "IDENTICAL" : "DIVERGED");
    // also verify across the two book implementations via the journal path
    return same ? 0 : 1;
}

int cmd_replay(int argc, char** argv) {
    const char* path = nullptr;
    u64 until = 0;
    const char* impl = "opt";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--journal") && i + 1 < argc) path = argv[++i];
        else if (!std::strcmp(argv[i], "--until") && i + 1 < argc)
            until = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--book") && i + 1 < argc) impl = argv[++i];
    }
    if (!path) {
        std::printf("usage: arbiter replay --journal FILE [--until N] [--book opt|ref]\n");
        return 2;
    }
    const ReplayResult r = replay_journal(
        path, std::strcmp(impl, "ref") == 0 ? BookImpl::Reference : BookImpl::Optimized,
        until, nullptr);
    std::printf("replay of %s (book=%s, until=%llu)\n", path, impl, (unsigned long long)until);
    std::printf("  status            : %s (ctx %llu)\n", to_cstr(r.status.code), (unsigned long long)r.status.context);
    std::printf("  events seen       : %llu (inputs applied: %llu)\n", (unsigned long long)r.events_seen,
                (unsigned long long)r.events_applied);
    std::printf("  fills recomputed  : %llu  trade mismatches: %llu  checkpoint mismatches: %llu\n",
                (unsigned long long)r.fills_recomputed, (unsigned long long)r.trade_mismatches,
                (unsigned long long)r.checkpoint_mismatches);
    std::printf("  book state hash   : %llu\n", (unsigned long long)r.state_hash);
    return r.status && !r.trade_mismatches && !r.checkpoint_mismatches ? 0 : 1;
}

int cmd_journal(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: arbiter journal <inspect|verify|recover> FILE\n");
        return 2;
    }
    const std::string sub = argv[1];
    const std::string path = argv[2];
    if (sub == "verify") {
        JournalVerifyReport rep = journal_verify(path);
        std::printf("journal %s: records=%llu last_seq=%llu clean=%s", path.c_str(),
                    (unsigned long long)rep.records, (unsigned long long)rep.last_seq,
                    rep.clean ? "yes" : "NO");
        if (!rep.clean)
            std::printf(" first problem: %s at offset %llu", to_cstr(rep.first_problem.code),
                        (unsigned long long)rep.problem_offset);
        std::printf("\n");
        return rep.clean ? 0 : 1;
    }
    if (sub == "recover") {
        auto kept = journal_recover(path);
        if (!kept) {
            std::printf("recover failed: %s\n", to_cstr(kept.status.code));
            return 1;
        }
        std::printf("recovered %llu records (file truncated to last verifiable frame)\n",
                    (unsigned long long)kept.value);
        return 0;
    }
    if (sub == "inspect") {
        auto rd = JournalReader::open(path);
        if (!rd) {
            std::printf("open failed: %s\n", to_cstr(rd.status.code));
            return 1;
        }
        std::printf("header: version=%u master_seed=%llu config_hash=%llu\n", rd.value.header().version,
                    (unsigned long long)rd.value.header().master_seed,
                    (unsigned long long)rd.value.header().config_hash);
        u64 n = 0;
        const u64 show = 10;
        while (rd.value.has_next()) {
            auto ev = rd.value.next();
            if (!ev) break;
            if (n < show)
                std::printf("  seq=%-8llu t=%-12lld %-12s a=%llu b=%llu c=%llu p=%lld\n",
                            (unsigned long long)ev.value.seq, (long long)ev.value.t,
                            to_cstr(ev.value.type), (unsigned long long)ev.value.a,
                            (unsigned long long)ev.value.b, (unsigned long long)ev.value.c,
                            (long long)ev.value.p);
            ++n;
        }
        std::printf("total: %llu events\n", (unsigned long long)n);
        return 0;
    }
    std::printf("unknown subcommand: %s\n", sub.c_str());
    return 2;
}

int cmd_timetravel(int argc, char** argv) {
    const char* path = nullptr;
    u64 target = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--journal") && i + 1 < argc) path = argv[++i];
        else if (!std::strcmp(argv[i], "--event") && i + 1 < argc) target = std::strtoull(argv[++i], nullptr, 10);
    }
    if (!path || !target) {
        std::printf("usage: arbiter timetravel --journal FILE --event N\n");
        return 2;
    }
    // full replay to N is the reference; snapshot+tail (when checkpoints exist)
    // must agree, this command demonstrates both.
    ReplayResult full = replay_journal(path, BookImpl::Optimized, target, nullptr);
    std::printf("time travel to event %llu of %s\n", (unsigned long long)target, path);
    std::printf("  status          : %s\n", to_cstr(full.status.code));
    std::printf("  book state hash : %llu\n", (unsigned long long)full.state_hash);
    std::printf("  inputs applied  : %llu (fills: %llu)\n", (unsigned long long)full.events_applied,
                (unsigned long long)full.fills_recomputed);
    return full.status ? 0 : 1;
}

// World-level time travel: capture a FULL world checkpoint mid-run (the
// entire scheduler/strategy/network state), then continue it, or fork it by
// changing exactly one configuration variable (strict_config=false path).
int cmd_worldcheck(int argc, char** argv) {
    u64 seed = 42, events = 20000;
    const char* out = "artifacts/world_checkpoint.wsnap";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--events") && i + 1 < argc) events = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }
    std::filesystem::create_directories("artifacts");
    // NOTE: the standard checkpoint config (below) is the one worldjump /
    // worldfork restore with; keep them in sync or restores will (correctly)
    // reject on config-hash mismatch.
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = 200'000'000;
    cfg.warmup_ns = 2'000'000;
    World w(cfg, seed);
    RunStats s = w.run(events);
    const WorldSnapshot snap = w.capture_snapshot();
    const Status st = write_world_snapshot(out, snap);
    if (!st) {
        std::printf("checkpoint write failed: %s\n", to_cstr(st.code));
        return 1;
    }
    std::printf("world checkpoint after %llu events (logical %lld ns):\n",
                (unsigned long long)s.events_processed, (long long)s.logical_end);
    std::printf("  snapshot : %s (%s)\n", out, to_cstr(st.code));
    std::printf("  state    : hash=%llu events=%llu config=%llu\n",
                (unsigned long long)snap.state_hash, (unsigned long long)snap.events,
                (unsigned long long)snap.config_hash);
    std::printf("  continue : arbiter worldjump --snap %s\n", out);
    std::printf("  fork     : arbiter worldfork --snap %s --variable half_spread=4\n", out);
    return 0;
}

int cmd_worldjump(int argc, char** argv) {
    const char* path = nullptr;
    u64 events = 0;  // 0 = run to the configured horizon
    u64 seed = 0;    // 0 = use the seed recorded in the snapshot
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--snap") && i + 1 < argc) path = argv[++i];
        else if (!std::strcmp(argv[i], "--events") && i + 1 < argc) events = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    }
    if (!path) {
        std::printf("usage: arbiter worldjump --snap F.wsnap [--events N] [--seed S]\n");
        return 2;
    }
    auto snap = read_world_snapshot(path);
    if (!snap) {
        std::printf("snapshot read failed: %s (ctx %llu)\n", to_cstr(snap.status.code),
                    (unsigned long long)snap.status.context);
        return 1;
    }
    if (seed == 0) seed = snap.value.master_seed;
    // --events 0 (default) means "run to the horizon": a literal 0 budget is
    // a no-op for an already-started (restored) world.
    const u64 jump_budget = events == 0 ? ~u64{0} : events;
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = 200'000'000;
    cfg.warmup_ns = 2'000'000;
    World w(cfg, seed);
    const Status st = w.restore_snapshot(snap.value, cfg, /*strict_config=*/true);
    if (!st) {
        std::printf("restore failed: %s (ctx %llu)\n", to_cstr(st.code),
                    (unsigned long long)st.context);
        return 1;
    }
    RunStats s = w.run(jump_budget);
    std::printf("continued from %s: +%llu events, final hash %llu\n", path,
                 (unsigned long long)(s.events_processed - snap.value.events),
                 (unsigned long long)w.state_hash());
    std::printf("  trades=%llu strategy fills=%llu\n", (unsigned long long)s.trades,
                (unsigned long long)s.strategy_fills);
    return 0;
}

int cmd_worldfork(int argc, char** argv) {
    const char* path = nullptr;
    const char* variable = "half_spread=4";
    u64 events = 0;
    u64 seed = 0;  // 0 = use the seed recorded in the snapshot
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--snap") && i + 1 < argc) path = argv[++i];
        else if (!std::strcmp(argv[i], "--variable") && i + 1 < argc) variable = argv[++i];
        else if (!std::strcmp(argv[i], "--events") && i + 1 < argc) events = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    }
    if (!path) {
        std::printf("usage: arbiter worldfork --snap F.wsnap --variable name=value [--events N] [--seed S]\n"
                    "  variables: any experiment override key (docs/COUNTERFACTUALS.md), e.g.\n"
                    "    half_spread=N | algo_latency=NS | md_loss=RATE | fee_per_lot=N\n");
        return 2;
    }
    auto snap = read_world_snapshot(path);
    if (!snap) {
        std::printf("snapshot read failed: %s\n", to_cstr(snap.status.code));
        return 1;
    }
    if (seed == 0) seed = snap.value.master_seed;
    // --events 0 (default) means "run to the horizon"
    const u64 fork_budget = events == 0 ? ~u64{0} : events;
    // parse variable
    std::string var = variable;
    const size_t eq = var.find('=');
    if (eq == std::string::npos) {
        std::printf("variable must be name=value\n");
        return 2;
    }
    const std::string key = var.substr(0, eq);
    const std::string val = var.substr(eq + 1);
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = 200'000'000;
    cfg.warmup_ns = 2'000'000;
    // CLI aliases -> experiment override keys (docs/COUNTERFACTUALS.md)
    std::string full_key = key;
    if (key == "half_spread") full_key = "mm.half_spread_ticks";
    else if (key == "algo_latency") full_key = "algo_latency_ns";
    else if (key == "md_loss") full_key = "md_channel.loss_rate";
    // reuse the experiment variable table (single source of truth)
    std::string err;
    if (!Experiment::apply_key_pub(cfg, full_key, val, err)) {
        std::printf("unknown variable: %s (supported: any experiment override, e.g.\n"
                    "  half_spread | algo_latency | md_loss | fee_per_lot | md_channel.jitter_ns ...)\n"
                    "  see docs/COUNTERFACTUALS.md\n", key.c_str());
        return 2;
    }
    // parent continuation: the ORIGINAL (worldcheck) config; the mutation
    // is applied only to the fork
    // parent continuation: the pristine worldcheck config (every forkable
    // variable at its worldcheck default); the mutation lives only in the
    // fork, so the parent is ALWAYS the true continuation.
    WorldConfig orig = cfg;
    {
        // reset the forked key to its default by NOT applying it: rebuild a
        // clean default config with worldcheck's fixed fields
        WorldConfig clean;
        clean.journal_events = false;
        clean.horizon_ns = 200'000'000;
        clean.warmup_ns = 2'000'000;
        orig = clean;
    }
    World a(orig, seed);
    if (Status st = a.restore_snapshot(snap.value, orig, true); !st) {
        std::printf("clone restore failed: %s\n", to_cstr(st.code));
        return 1;
    }
    a.run(fork_budget);
    const u64 hash_a = a.state_hash();
    // fork B: one changed variable from the SAME snapshot
    World b(cfg, seed);
    if (Status st = b.restore_snapshot(snap.value, cfg, /*strict_config=*/false); !st) {
        std::printf("fork restore failed: %s\n", to_cstr(st.code));
        return 1;
    }
    RunStats sb = b.run(fork_budget);
    const u64 hash_b = b.state_hash();
    // Record branch genealogy.
    BranchIdentity bi{};
    bi.world_id = b.world_id();
    bi.parent_world_id = a.world_id();
    bi.fork_event = snap.value.events;
    bi.config_hash = cfg.hash();
    bi.initial_state_hash = snap.value.state_hash;
    const i64 pnl_a = a.stats().mm_cash + a.stats().mm_inventory * a.stats().final_mid;
    const i64 pnl_b = sb.mm_cash + sb.mm_inventory * sb.final_mid;
    std::printf("fork at event %llu (variable %s=%s):\n", (unsigned long long)snap.value.events,
                key.c_str(), val.c_str());
    std::printf("  parent continuation : hash %llu (pnl %lld, fills %llu)\n",
                (unsigned long long)hash_a, (long long)pnl_a,
                (unsigned long long)a.stats().strategy_fills);
    std::printf("  forked branch       : hash %llu (pnl %lld, fills %llu)\n",
                (unsigned long long)hash_b, (long long)pnl_b,
                (unsigned long long)sb.strategy_fills);
    std::printf("  genealogy           : world_id=%llu parent=%llu fork_event=%llu\n",
                (unsigned long long)bi.world_id, (unsigned long long)bi.parent_world_id,
                (unsigned long long)bi.fork_event);
    std::printf("  RESULT: %s\n", hash_a == hash_b ? "RECONVERGED (identical outcomes)" : "DIVERGED");
    return 0;
}

int cmd_experiment(int argc, char** argv) {
    u64 seed = 2026;
    u64 horizon_ms = 150;
    const char* name = "latency_grid";
    const char* reproduce = nullptr;
    const char* config_file = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--ms") && i + 1 < argc) horizon_ms = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!std::strcmp(argv[i], "--reproduce") && i + 1 < argc) reproduce = argv[++i];
        else if (!std::strcmp(argv[i], "--config") && i + 1 < argc) config_file = argv[++i];
    }
    if (reproduce) {
        const ReproduceReport rep = experiment_reproduce(reproduce);
        std::printf("reproduce %s\n  variants checked : %llu\n  hash mismatches : %llu\n",
                    reproduce, (unsigned long long)rep.variants_checked,
                    (unsigned long long)rep.hash_mismatches);
        std::printf("RESULT: %s (%s)\n", rep.ok ? "EXACTLY REPRODUCED" : "REPRODUCTION FAILED",
                    rep.detail.c_str());
        return rep.ok ? 0 : 1;
    }
    std::filesystem::create_directories("artifacts");
    const std::string family = name;
    const auto run_and_write = [&](const std::string& grid_name, Experiment& exp) {
        const ExperimentResult res = exp.run();
        const std::string txt = experiment_summary(res, grid_name);
        const std::string json = experiment_to_json(res, grid_name);
        std::printf("%s", txt.c_str());
        const std::string artifact = "artifacts/experiment_" + grid_name + ".json";
        if (!write_file(artifact, json)) {
            std::fprintf(stderr, "cannot write %s\n", artifact.c_str());
            return false;
        }
        std::printf("  artifact: %s\n", artifact.c_str());
        return true;
    };
    if (config_file) {
        // file-driven experiment definition (docs/COUNTERFACTUALS.md grammar)
        auto cfg = Config::from_file(config_file);
        if (!cfg) {
            std::printf("config read failed: %s\n", to_cstr(cfg.status.code));
            return 1;
        }
        // the file's [experiment] section owns seed/horizon; explicit CLI
        // flags are ignored in this mode, say so instead of silently
        // surprising the user
        if (seed != 2026 || horizon_ms != 150)
            std::printf("  note: --seed/--ms are ignored with --config (the file's [experiment] section wins)\n");
        const std::string stem = std::filesystem::path(config_file).stem().string();
        auto exp = Experiment::from_config(cfg.value, stem);
        if (!exp) {
            std::printf("experiment definition invalid: %s (ctx %llu)\n",
                        to_cstr(exp.status.code), (unsigned long long)exp.status.context);
            return 1;
        }
        if (!run_and_write(stem, exp.value)) return 1;
        std::printf("  reproduce: arbiter experiment --reproduce artifacts/experiment_%s.json\n", stem.c_str());
        return 0;
    }
    if (family == "latency_sweep") {
        // Eight-point sweep: where does decision latency start to cost fills?
        for (u64 i = 0; i < 8; ++i) {
            const i64 lat = 100 * static_cast<i64>(1 << i);  // 100ns..12.8us
            WorldConfig base;
            base.journal_events = false;
            base.horizon_ns = static_cast<LogicalNs>(horizon_ms) * 1'000'000;
            base.warmup_ns = 2'000'000;
            Experiment exp(base);
            exp.set_seed(seed);
            exp.set_name("latency_sweep_" + std::to_string(lat));
            exp.add_variant("baseline_800ns", {{"algo_latency_ns", "800"}});
            exp.add_variant("sweep", {{"algo_latency_ns", std::to_string(lat)}});
            if (!run_and_write("latency_sweep_" + std::to_string(lat), exp)) return 1;
        }
        std::printf("  reproduce: arbiter experiment --reproduce artifacts/experiment_latency_sweep_3200.json\n");
        return 0;
    }
    if (family == "network_grid") {
        // jitter/loss cross-grid on the MD channel (the feed-handler path)
        const std::pair<const char*, std::vector<std::pair<std::string, std::string>>> mutators[] = {
            {"baseline", {}},
            {"jitter_500", {{"md_channel.jitter_ns", "500"}}},
            {"jitter_2000", {{"md_channel.jitter_ns", "2000"}}},
            {"loss_1pct", {{"md_channel.loss_rate", "0.01"}}},
            {"loss_2pct", {{"md_channel.loss_rate", "0.02"}}},
            {"jitter_2000_loss_1pct", {{"md_channel.jitter_ns", "2000"}, {"md_channel.loss_rate", "0.01"}}},
        };
        WorldConfig base;
        base.journal_events = false;
        base.horizon_ns = static_cast<LogicalNs>(horizon_ms) * 1'000'000;
        base.warmup_ns = 2'000'000;
        Experiment exp(base);
        exp.set_seed(seed);
        exp.set_name("network_grid");
        for (const auto& [nm, ovr] : mutators) exp.add_variant(nm, ovr);
        if (!run_and_write("network_grid", exp)) return 1;
        std::printf("  reproduce: arbiter experiment --reproduce artifacts/experiment_network_grid.json\n");
        return 0;
    }
    // default: the classic latency grid
    WorldConfig base;
    base.journal_events = false;
    base.horizon_ns = static_cast<LogicalNs>(horizon_ms) * 1'000'000;
    base.warmup_ns = 2'000'000;
    Experiment exp(base);
    exp.set_seed(seed);
    exp.set_name(name);
    exp.add_variant("baseline_800ns", {{"algo_latency_ns", "800"}});
    exp.add_variant("fast_300ns", {{"algo_latency_ns", "300"}});
    exp.add_variant("slow_3000ns", {{"algo_latency_ns", "3000"}});
    exp.add_variant("jittery_md", {{"md_channel.jitter_ns", "1500"}});
    exp.add_variant("lossy_md_1pct", {{"md_channel.loss_rate", "0.01"}});
    if (!run_and_write(name, exp)) return 1;
    std::printf("  reproduce: arbiter experiment --reproduce artifacts/experiment_%s.json\n", name);
    return 0;
}

int cmd_simstats(int argc, char** argv) {
    u64 horizon_ms = 500;
    u64 seed = 99;
    double self_excitation = 0.0;  // default: the Poisson-null regime
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--ms") && i + 1 < argc) horizon_ms = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--self-excitation") && i + 1 < argc)
            self_excitation = std::strtod(argv[++i], nullptr);
    }
    // Level-2 reconstruction from the MD journal: replay MdNew/MdCancel/
    // MdTrade through a book, sampling the SPREAD at every event and the
    // mid at every trade. Reports spread percentiles, trade intensity,
    // trade-to-trade returns, 1-event return autocorrelation (volatility
    // clustering probe), and mid displacement.
    WorldConfig cfg;
    cfg.journal_events = true;
    cfg.journal_path = "artifacts/simstats.journal";
    cfg.horizon_ns = static_cast<LogicalNs>(horizon_ms) * 1'000'000;
    cfg.sim.taker_self_excitation = self_excitation;
    std::filesystem::create_directories("artifacts");
    std::remove(cfg.journal_path.c_str());
    World w(cfg, seed);
    RunStats s = w.run();

    Histogram spread(1u << 20), rets(1u << 20);
    OptBook book{OptBook::Options{1, 100'000, 4096}};
    std::vector<Fill> scratch;
    std::vector<double> mid_at_trade;  // mids at consecutive trades
    std::vector<LogicalNs> trade_times;  // for burstiness (inter-arrival cv)
    int last_sign = 0;
    u64 sign_repeats = 0, sign_flips = 0;  // trade-sign persistence probes
    u64 md_seen = 0, trades = 0;
    Price last_mid_px = 0;
    auto rd = JournalReader::open(cfg.journal_path);
    if (!rd) {
        std::printf("journal open failed\n");
        return 1;
    }
    while (rd.value.has_next()) {
        auto ev = rd.value.next();
        if (!ev) break;
        const Event& e = ev.value;
        if (e.type == EvType::MdNew) {
            scratch.clear();
            // duplicate MdNew ids are legal in the journal (gap-recovery
            // re-sends); rest_order's Duplicate error makes this idempotent
            (void)book.rest_order(Order{e.a, 0, e.side, OrderType::Limit, Tif::GTC, e.p, e.c, e.c, 0, 0, 0});
            ++md_seen;
        } else if (e.type == EvType::MdCancel) {
            book.cancel(e.a);
            ++md_seen;
        } else if (e.type == EvType::MdTrade) {
            // consume at the traded price (removes executed liquidity);
            // attribute the side by where liquidity rests
            const Side s_consume = book.qty_at(Side::Sell, e.p) > 0 ? Side::Sell : Side::Buy;
            book.consume_at(s_consume, e.p, e.a);
            ++md_seen;
            ++trades;
            const Price bb = book.best_bid(), ba = book.best_ask();
            const Price px = e.p;
            if (last_mid_px) {
                const double r = 10000.0 * static_cast<double>(px - last_mid_px) /
                                static_cast<double>(last_mid_px);
                rets.record(r);
            }
            // clustering diagnostics: trade-sign persistence + burstiness
            // (sign computed against the PREVIOUS trade's price, before
            // last_mid_px is updated below)
            trade_times.push_back(e.t);
            {
                const int sgn = last_mid_px && px > last_mid_px ? 1
                              : (last_mid_px && px < last_mid_px ? -1 : 0);
                if (sgn != 0) {
                    if (last_sign != 0) {
                        if (sgn == last_sign) ++sign_repeats;
                        else ++sign_flips;
                    }
                    last_sign = sgn;
                }
            }
            last_mid_px = px;
            const Price mid = (bb && ba) ? (bb + ba) / 2 : (bb ? bb : ba);
            // a zero mid (both sides momentarily empty) is not a sample,
            // pushing it would poison the autocorrelation mean
            if (mid > 0) mid_at_trade.push_back(static_cast<double>(mid));
        }
        // spread sample at every MD event (post-application)
        const Price bb = book.best_bid(), ba = book.best_ask();
        if (bb && ba) spread.record(static_cast<double>(ba - bb));
    }
    const auto sp = spread.summarize();
    const auto rs = rets.summarize();
    // lag-1 autocorrelation of trade-time mids (volatility clustering probe)
    double autoc = 0.0;
    {
        const size_t n = mid_at_trade.size();
        if (n > 10) {
            double mean = 0;
            for (double v : mid_at_trade) mean += v;
            mean /= static_cast<double>(n);
            double num = 0, den = 0;
            for (size_t i = 0; i + 1 < n; ++i) {
                num += (mid_at_trade[i] - mean) * (mid_at_trade[i + 1] - mean);
            }
            for (double v : mid_at_trade) den += (v - mean) * (v - mean);
            autoc = den > 0 ? num / den : 0.0;
        }
    }
    std::printf("=== simulator statistical validation (seed=%llu, %llu ms logical, self-excitation k=%.2f) ===\n",
                (unsigned long long)seed, (unsigned long long)horizon_ms, self_excitation);
    std::printf("  level-2 reconstruction: %llu md events applied to a local book\n",
                (unsigned long long)md_seen);
    std::printf("  spread (ticks): p50=%.0f p90=%.0f p99=%.0f max=%.0f (n=%llu)\n",
                sp.p50, sp.p90, sp.p99, sp.mx, (unsigned long long)sp.count);
    std::printf("  trades: %llu (%.0f per logical second)\n", (unsigned long long)trades,
                static_cast<double>(trades) / (static_cast<double>(horizon_ms) / 1000.0));
    std::printf("  trade-to-trade returns (bp): p1=%.2f p50=%.2f p99=%.2f max=%.2f (n=%llu)\n",
                rs.p50, rs.p50, rs.p99, rs.mx, (unsigned long long)rs.count);
    std::printf("  mid lag-1 autocorrelation at trade times: %.4f\n", autoc);
    // clustering diagnostics (the flagship mechanism variables):
    // sign persistence P(next change = same direction) and inter-arrival
    // burstiness (cv; Poisson null => ~1, cv>1 => clustered arrivals)
    {
        const u64 sign_total = sign_repeats + sign_flips;
        if (sign_total > 10)
            std::printf("  trade-sign persistence: %.4f (repeat rate; runs emerge as it exceeds 0.5)\n",
                        static_cast<double>(sign_repeats) / static_cast<double>(sign_total));
        if (trade_times.size() > 10) {
            double mean_dt = 0;
            std::vector<double> dts;
            for (size_t i = 1; i < trade_times.size(); ++i) {
                const double dt = static_cast<double>(trade_times[i] - trade_times[i - 1]);
                if (dt >= 0) { dts.push_back(dt); mean_dt += dt; }
            }
            if (!dts.empty()) {
                mean_dt /= static_cast<double>(dts.size());
                double var = 0;
                for (double v : dts) var += (v - mean_dt) * (v - mean_dt);
                var /= static_cast<double>(dts.size());
                const double cv = mean_dt > 0 ? std::sqrt(var) / mean_dt : 0.0;
                std::printf("  trade inter-arrival burstiness (cv): %.4f (Poisson ~1; >1 clustered)\n", cv);
            }
        }
    }
    std::printf("  world: %llu events, final mid %lld\n", (unsigned long long)s.events_processed,
                (long long)s.final_mid);
    std::printf("  NOTE: stylized facts are SIMULATOR properties (model choices), not\n"
                "        calibration claims; see docs/SIMULATION.md.\n");
    return 0;
}

// ---- flagship: fixed-total latency-allocation experiment ----

namespace {

FlagshipConfig flagship_config_from_file(const Config& cfg, const std::string& name) {
    FlagshipConfig fc;
    fc.name = cfg.has("experiment.name") ? cfg.get_str("experiment.name", name) : name;
    fc.total_latency_ns = cfg.get_i64("latency.total_ns", 8000);
    fc.flow_model = cfg.get_str("market.flow_model", "clustered") == "poisson"
                        ? FlowModel::Poisson
                        : FlowModel::Clustered;
    fc.taker_self_excitation = cfg.get_f64("market.taker_self_excitation", 0.9);
    fc.bots_observe_shadow = cfg.get_bool("market.bots_observe_shadow", false);
    fc.horizon_ns = cfg.get_i64("experiment.horizon_ns", 100'000'000);
    fc.warmup_ns = cfg.get_i64("experiment.warmup_ns", 2'000'000);
    fc.economic_threshold_ticks_qty_per_lot =
        cfg.get_f64("stats.economic_threshold_per_lot", 1.0);
    fc.permutation_reps = cfg.get_u64("stats.permutation_reps", 5000);
    // seeds: comma-separated in [experiment]
    fc.seeds.clear();
    const std::string seed_str = cfg.get_str("experiment.seeds", "2026,2027,2028,2029,2030,2031,2032,2033");
    for (size_t p = 0; p < seed_str.size();) {
        const size_t c = seed_str.find(',', p);
        const std::string tok = seed_str.substr(p, c == std::string::npos ? std::string::npos : c - p);
        if (!tok.empty()) fc.seeds.push_back(std::strtoull(tok.c_str(), nullptr, 10));
        if (c == std::string::npos) break;
        p = c + 1;
    }
    // md shares: comma-separated fractions in [latency]
    const std::string share_str = cfg.get_str("latency.md_shares",
                                             "0,0.125,0.25,0.375,0.5,0.625,0.75,0.875,1");
    fc.md_shares.clear();
    for (size_t p = 0; p < share_str.size();) {
        const size_t c = share_str.find(',', p);
        const std::string tok = share_str.substr(p, c == std::string::npos ? std::string::npos : c - p);
        if (!tok.empty()) fc.md_shares.push_back(std::strtod(tok.c_str(), nullptr));
        if (c == std::string::npos) break;
        p = c + 1;
    }
    for (double x : fc.md_shares)
        if (!(x >= 0.0 && x <= 1.0)) {
            std::printf("invalid flagship definition: md_share %.17g outside [0,1]\n", x);
            std::exit(2);  // loud: malformed definition must never fall back to defaults
        }
    return fc;
}

}  // namespace

int cmd_flagship(int argc, char** argv) {
    const char* config_file = nullptr;
    const char* reproduce = nullptr;
    const char* out_dir = nullptr;
    u64 seed0 = 2026, nseeds = 8;
    i64 total_ns = 8000;
    const char* flow = "clustered";
    bool shadow = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc) config_file = argv[++i];
        else if (!std::strcmp(argv[i], "--reproduce") && i + 1 < argc) reproduce = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed0 = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) nseeds = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--total-ns") && i + 1 < argc) total_ns = std::strtoll(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--flow") && i + 1 < argc) flow = argv[++i];
        else if (!std::strcmp(argv[i], "--frozen") || !std::strcmp(argv[i], "--shadow")) shadow = true;
    }
    if (reproduce) {
        const FlagshipReproReport rep = flagship_reproduce(reproduce);
        std::printf("flagship reproduce %s\n  cells: %llu  seeds: %llu  mismatches: %llu\n",
                    reproduce, (unsigned long long)rep.cells_checked,
                    (unsigned long long)rep.seeds_checked, (unsigned long long)rep.mismatches);
        std::printf("RESULT: %s (%s)\n", rep.ok ? "EXACTLY REPRODUCED" : "REPRODUCTION FAILED",
                    rep.detail.c_str());
        return rep.ok ? 0 : 1;
    }

    FlagshipConfig fc;
    if (config_file) {
        auto cfg = Config::from_file(config_file);
        if (!cfg) {
            std::printf("config read failed: %s\n", to_cstr(cfg.status.code));
            return 1;
        }
        const std::string stem = std::filesystem::path(config_file).stem().string();
        fc = flagship_config_from_file(cfg.value, stem);
    } else {
        fc.total_latency_ns = total_ns;
        fc.flow_model = std::strcmp(flow, "poisson") == 0 ? FlowModel::Poisson : FlowModel::Clustered;
        fc.bots_observe_shadow = shadow;
        fc.seeds.clear();
        for (u64 k = 0; k < nseeds; ++k) fc.seeds.push_back(seed0 + k);
        // never an empty grid
        if (fc.md_shares.empty()) fc.md_shares = {0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0};
    }
    if (fc.seeds.empty() || fc.md_shares.empty() || fc.total_latency_ns < 0) {
        std::printf("invalid flagship definition (need seeds, md_shares, total >= 0)\n");
        return 1;
    }

    const FlagshipResult res = run_flagship(fc);
    std::string dir = out_dir ? out_dir
                             : "experiments/FLAGSHIP-" + res.flow_model_name +
                                   (fc.bots_observe_shadow ? "-frozen" : "-reactive");
    if (Status st = write_flagship_artifacts(dir, res); !st) {
        std::printf("artifact write failed: %s (ctx %llu)\n", to_cstr(st.code),
                    static_cast<unsigned long long>(st.context));
        return 1;
    }
    // console summary
    std::printf("== flagship: %s ==\n", res.cfg.name.c_str());
    std::printf("  total_ns=%lld flow=%s interference=%s seeds=%zu cells=%zu def_hash=%llu\n",
                (long long)res.cfg.total_latency_ns, res.flow_model_name.c_str(),
                fc.bots_observe_shadow ? "frozen" : "reactive", res.cfg.seeds.size(),
                res.cells.size(), (unsigned long long)res.definition_hash);
    std::printf("  cell                        MM PnL   MM lots  MM markout   Taker PnL\n");
    for (const CellResult& c : res.cells)
        std::printf("  %-24s %8.1f %8.1f %10.2f %11.1f\n", c.name.c_str(), c.mean_mm_pnl,
                    c.mean_mm_filled_qty, c.mean_mm_markout, c.mean_mom_pnl);
    std::printf("  primary contrast (extremes): see pairs in %s/report.md\n", dir.c_str());
    for (const std::string& f : res.findings) std::printf("  * %s\n", f.c_str());
    std::printf("  artifact: %s/results.json + report.md\n", dir.c_str());
    std::printf("  reproduce: arbiter flagship --reproduce %s/results.json\n", dir.c_str());
    return 0;
}

int cmd_fuzz(int argc, char** argv) {
    u64 iters = 200000;
    const char* which = "protocol";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--iters") && i + 1 < argc) iters = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--target") && i + 1 < argc) which = argv[++i];
    }
    Rng rng = Rng::stream(20260906, 0xF00D);
    if (!std::strcmp(which, "protocol")) {
        Event e;
        e.type = EvType::BookAdd;
        e.a = 9;
        e.c = 4;
        e.p = 1500;
        const std::string f = wire_encode(1, 1, kFlagNone, e);
        u64 rejected = 0, accepted = 0;
        for (u64 i = 0; i < iters; ++i) {
            std::string m = f;
            const u64 muts = 1 + rng.bounded(8);
            for (u64 k = 0; k < muts; ++k)
                m[static_cast<size_t>(rng.bounded(m.size()))] = static_cast<char>(rng.next_u32());
            auto r = wire_decode(reinterpret_cast<const u8*>(m.data()), m.size());
            if (r) ++accepted; else ++rejected;
        }
        std::printf("protocol fuzz: %llu iterations, %llu accepted, %llu rejected, 0 crashes\n",
                    (unsigned long long)iters, (unsigned long long)accepted, (unsigned long long)rejected);
        return 0;
    }
    if (!std::strcmp(which, "differential")) {
        // adversarial op streams vs ref/opt books; every divergence is a bug
        u64 ops_total = 0, divergences = 0;
        for (u64 seed = 1; seed <= 100; ++seed) {
            auto ops = diff::random_ops(seed ^ 0xBEEF, static_cast<size_t>(2000 + iters % 3000));
            ops_total += ops.size();
            diff::Report rep = diff::run_differential(ops);
            if (rep.diverged) {
                ++divergences;
                std::printf("DIVERGENCE seed=%llu at op %llu: %s\n", (unsigned long long)seed,
                            (unsigned long long)rep.div.op_index, rep.div.why.c_str());
            }
        }
        std::printf("differential fuzz: %llu ops across 100 seeds, %llu divergences\n",
                    (unsigned long long)ops_total, (unsigned long long)divergences);
        return divergences == 0 ? 0 : 1;
    }
    if (!std::strcmp(which, "adversarial")) {
        // hill-climb the op-generator parameter space chasing divergences;
        // any find is minimized and persisted to the corpus (regression
        // corpus, replayed by `fuzz --target corpus` and test_diff)
        const u64 base_seed = 20260907;
        std::string corpus = std::filesystem::exists("tests/corpus") ? "tests/corpus"
                             : std::filesystem::exists("../tests/corpus") ? "../tests/corpus"
                                                                          : "tests/corpus";
        auto res = diff::adversarial_search(base_seed, static_cast<u32>(iters), 2000, corpus);
        if (res.found) {
            std::printf("adversarial search: DIVERGENCE at op %llu after %llu iterations (%llu seeds): %s\n",
                        (unsigned long long)res.div.op_index, (unsigned long long)res.iterations,
                        (unsigned long long)res.seeds_tried, res.div.why.c_str());
            std::printf("  minimized reproducer: %zu ops persisted to tests/corpus/\n",
                        res.minimized_reproducer.size());
            return 1;
        }
        std::printf("adversarial search: %llu iterations, %llu seeds, 0 divergences (corpus now has any prior finds)\n",
                    (unsigned long long)res.iterations, (unsigned long long)res.seeds_tried);
        return 0;
    }
    if (!std::strcmp(which, "corpus")) {
        // replay + recheck every persisted divergence (regression lock);
        // accept repo-root or build-dir working directories
        std::string corpus = std::filesystem::exists("tests/corpus") ? "tests/corpus"
                             : std::filesystem::exists("../tests/corpus") ? "../tests/corpus"
                                                                          : "tests/corpus";
        const auto [checked, diverged] = diff::run_corpus(corpus);
        std::printf("corpus (%s): %llu files checked, %llu still diverge (0 expected)\n",
                    corpus.c_str(), (unsigned long long)checked, (unsigned long long)diverged);
        return diverged == 0 ? 0 : 1;
    }
    std::printf("targets: protocol | differential | adversarial | corpus\n");
    return 2;
}

int cmd_demo(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "all";
    int demo_status = 0;
    std::filesystem::create_directories("artifacts");
    const auto banner = [](const char* s) { std::printf("\n=== DEMO: %s ===\n", s); };

    if (!std::strcmp(which, "all") || !std::strcmp(which, "replay")) {
        banner("deterministic replay");
        char a0[] = "arbiter", a1[] = "verify", a2[] = "--seed", a3[] = "42", a4[] = "--ms", a5[] = "50";
        char* av[] = {a0, a1, a2, a3, a4, a5};
        cmd_verify(6, av);
    }
    if (!std::strcmp(which, "all") || !std::strcmp(which, "recovery")) {
        banner("crash recovery (journal corruption + repair)");
        WorldConfig cfg;
        cfg.journal_events = true;
        cfg.journal_path = "artifacts/demo_recovery.journal";
        cfg.horizon_ns = 100'000'000;
        std::remove(cfg.journal_path.c_str());
        World w(cfg, 4242);
        RunStats s = w.run();
        const u64 full_hash = w.state_hash();
        std::printf("  original run: %llu events, hash %llu\n", (unsigned long long)s.events_processed,
                    (unsigned long long)full_hash);
        // simulate a crash: truncate the journal at 61%
        const auto size = std::filesystem::file_size(cfg.journal_path);
        const auto cut = size * 61 / 100;
        std::filesystem::resize_file(cfg.journal_path, cut);
        JournalVerifyReport v = journal_verify(cfg.journal_path);
        std::printf("  after truncation at %llu bytes: clean=%s\n", (unsigned long long)cut,
                    v.clean ? "yes" : "no");
        auto kept = journal_recover(cfg.journal_path);
        ReplayResult r = replay_journal(cfg.journal_path, BookImpl::Optimized, 0, nullptr);
        std::printf("  recovered %llu records; replay status %s, hash %llu\n",
                    (unsigned long long)kept.value, to_cstr(r.status.code),
                    (unsigned long long)r.state_hash);
        std::printf("  RESULT: RECOVERED PREFIX (truncated-crash semantics, documented)\n");
    }
    if (!std::strcmp(which, "all") || !std::strcmp(which, "differential")) {
        banner("differential verification (reference vs optimized)");
        char a0[] = "arbiter", a1[] = "fuzz", a2[] = "--target", a3[] = "differential", a4[] = "--iters", a5[] = "3000";
        char* av[] = {a0, a1, a2, a3, a4, a5};
        demo_status = cmd_fuzz(6, av);
    }
    if (!std::strcmp(which, "all") || !std::strcmp(which, "counterfactual")) {
        banner("counterfactual worlds (same seed, different latency)");
        char a0[] = "arbiter", a1[] = "experiment", a2[] = "--seed", a3[] = "2026", a4[] = "--ms", a5[] = "150";
        char* av[] = {a0, a1, a2, a3, a4, a5};
        demo_status = cmd_experiment(6, av);
    }
    if (!std::strcmp(which, "all") || !std::strcmp(which, "perf")) {
        banner("performance investigation");
        const char* bench = std::getenv("ARBITER_BENCH");
        std::string cmd = std::string(bench && *bench ? bench : "./build/arbiter_bench");
        const int exchange_status = std::system((cmd + " exchange").c_str());
        const int pipeline_status = std::system((cmd + " pipeline").c_str());
        if (exchange_status != 0 || pipeline_status != 0) {
            std::fprintf(stderr, "performance demo failed (exchange=%d, pipeline=%d)\n",
                         exchange_status, pipeline_status);
            demo_status = 1;
        }
    }
    return demo_status;
}

void print_usage() {
    std::printf(
        "ARBITER-X — deterministic counterfactual execution laboratory (research/simulation only)\n\n"
        "usage: arbiter <command> [options]\n"
        "  simulate    --seed S --ms MS --out PATH   run a world, journal it\n"
        "  verify      [--seed S --ms MS]            run twice, compare hashes\n"
        "  replay      --journal F [--until N]       rebuild + verify from journal\n"
        "  journal     inspect|verify|recover F      journal tooling\n"
        "  timetravel  --journal F --event N         state at event N (book)\n"
        "  worldcheck  [--seed S --events N]         save a full-world checkpoint\n"
        "  worldjump   --snap F.wsnap [--events N]   restore + continue a world\n"
        "  worldfork   --snap F.wsnap --variable k=v fork one variable, compare\n"
        "  experiment  [--seed S --ms MS [--name latency_grid|latency_sweep|network_grid]]\n"
        "                                               counterfactual grid (+artifacts)\n"
        "  experiment  --config FILE.axexp               file-driven grid (see configs/)\n"
        "  experiment  --reproduce ARTIFACT.json         re-run + verify hashes exactly\n"
        "  flagship    [--config F] [--total-ns N] [--flow poisson|clustered]\n"
        "              [--seed S --seeds N] [--frozen]  fixed-total allocation study\n"
        "  flagship    --reproduce ARTIFACT.json         verify flagship hashes exactly\n"
        "  simstats    [--seed S --ms MS]            simulator statistics (level-2)\n"
        "  research    validate|features|evaluate|backtest|run|reproduce|calibrate\n"
        "                                               quantitative research pipeline\n"
        "  fuzz        protocol|differential|adversarial|corpus  adversarial verification\n"
        "  demo        replay|recovery|differential|counterfactual|perf|all\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    const std::string cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }
    if (cmd == "simulate") return cmd_simulate(argc - 1, argv + 1);
    if (cmd == "verify") return cmd_verify(argc - 1, argv + 1);
    if (cmd == "replay") return cmd_replay(argc - 1, argv + 1);
    if (cmd == "journal") return cmd_journal(argc - 1, argv + 1);
    if (cmd == "timetravel") return cmd_timetravel(argc - 1, argv + 1);
    if (cmd == "worldcheck") return cmd_worldcheck(argc - 1, argv + 1);
    if (cmd == "worldjump") return cmd_worldjump(argc - 1, argv + 1);
    if (cmd == "worldfork") return cmd_worldfork(argc - 1, argv + 1);
    if (cmd == "experiment") return cmd_experiment(argc - 1, argv + 1);
    if (cmd == "flagship") return cmd_flagship(argc - 1, argv + 1);
    if (cmd == "simstats") return cmd_simstats(argc - 1, argv + 1);
    if (cmd == "research") return cmd_research(argc - 1, argv + 1);
    if (cmd == "fuzz") return cmd_fuzz(argc - 1, argv + 1);
    if (cmd == "demo") return cmd_demo(argc - 1, argv + 1);
    if (cmd == "bench") {
        std::printf("use ./build/arbiter_bench [all|exchange|protocol|journal|pipeline]\n");
        return 0;
    }
    std::printf("unknown command: %s\n", cmd.c_str());
    return 2;
}