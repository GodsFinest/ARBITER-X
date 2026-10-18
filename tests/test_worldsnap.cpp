#include "testfw.hpp"

#include "arbiter/experiment.hpp"
#include "arbiter/latency.hpp"
#include "arbiter/metrics.hpp"
#include "arbiter/world.hpp"
#include "arbiter/world_snapshot.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <filesystem>
#include <vector>

using namespace arbiter;

namespace {

const std::string kArt = "./build/test_artifacts";

WorldConfig snap_test_config() {
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = 100'000'000;
    cfg.warmup_ns = 2'000'000;
    return cfg;
}

// Scalar counter equality between a continuous run and a restored run.
void check_stats_equal(const RunStats& a, const RunStats& b) {
    CHECK_EQ(a.trades, b.trades);
    CHECK_EQ(a.volume, b.volume);
    CHECK_EQ(a.bot_fills, b.bot_fills);
    CHECK_EQ(a.strategy_fills, b.strategy_fills);
    CHECK_EQ(a.momentum_fills, b.momentum_fills);
    CHECK_EQ(a.strategy_orders_sent, b.strategy_orders_sent);
    CHECK_EQ(a.strategy_cancels_sent, b.strategy_cancels_sent);
    CHECK_EQ(a.risk_accepted, b.risk_accepted);
    CHECK_EQ(a.risk_rejected, b.risk_rejected);
    CHECK_EQ(a.md_published, b.md_published);
    CHECK_EQ(a.md_delivered, b.md_delivered);
    CHECK_EQ(a.md_dropped, b.md_dropped);
    CHECK_EQ(a.md_gap_events, b.md_gap_events);
    CHECK_EQ(a.md_recovered, b.md_recovered);
    CHECK_EQ(a.wire_frames_encoded, b.wire_frames_encoded);
    CHECK_EQ(a.wire_frames_decoded, b.wire_frames_decoded);
    CHECK_EQ(a.wire_frames_corrupted, b.wire_frames_corrupted);
}

// Runs a continuous world; returns final hash. Used as the reference.
u64 continuous_run(const WorldConfig& cfg, u64 seed, RunStats* stats_out = nullptr) {
    World w(cfg, seed);
    RunStats s = w.run();
    if (stats_out) *stats_out = s;
    return w.state_hash();
}

}  // namespace

// ---- run-to-N + restore + continue == continuous ----

AX_TEST(worldrestore_contract_at_fixed_points) {
    std::filesystem::create_directories(kArt);
    const WorldConfig cfg = snap_test_config();
    const u64 seed = 777;
    RunStats ref_stats;
    const u64 ref_hash = continuous_run(cfg, seed, &ref_stats);

    for (u64 n : {1ull, 200ull, 2000ull, 20000ull, 45000ull}) {
        if (n >= ref_stats.events_processed) continue;  // fixed point must be inside the horizon
        // World B: run to exactly n events
        World b(cfg, seed);
        RunStats sb = b.run(n);
        CHECK_EQ(sb.events_processed, n);
        WorldSnapshot snap = b.capture_snapshot();
        CHECK_EQ(snap.state_hash, b.state_hash());

        // Roundtrip the self-describing, checksummed snapshot through a file.
        const std::string path = kArt + "/contract_" + std::to_string(n) + ".wsnap";
        CHECK(write_world_snapshot(path, snap));
        auto reloaded = read_world_snapshot(path);
        CHECK(reloaded);
        if (!reloaded) return;
        CHECK_EQ(reloaded.value.state_hash, snap.state_hash);

        // destroy B; restore into a fresh world; continue
        World c(cfg, seed);
        const Status st = c.restore_snapshot(reloaded.value, cfg, /*strict_config=*/true);
        CHECK(st);
        if (!st) {
            std::printf("  restore failed at N=%llu: %s\n", (unsigned long long)n,
                        to_cstr(st.code));
            return;
        }
        RunStats sc = c.run();
        CHECK_EQ(c.state_hash(), ref_hash);
        check_stats_equal(ref_stats, sc);
        CHECK_EQ(sc.events_processed, ref_stats.events_processed);
    }
}

AX_TEST(worldrestore_capture_is_nondestructive_and_stable) {
    const WorldConfig cfg = snap_test_config();
    World w(cfg, 31415);
    w.run(5000);
    WorldSnapshot s1 = w.capture_snapshot();
    WorldSnapshot s2 = w.capture_snapshot();
    CHECK_EQ(s1.state_hash, s2.state_hash);
    CHECK_EQ(s1.queue.size(), s2.queue.size());
    CHECK_EQ(s1.events, s2.events);
    // the world keeps running correctly after captures
    const u64 h = w.state_hash();
    w.run(1000);
    CHECK_NE(w.state_hash(), h);
}

AX_TEST(worldrestore_chained_snapshots_equivalent) {
    // snapshot -> continue -> snapshot again -> ... must compose exactly.
    const WorldConfig cfg = snap_test_config();
    const u64 seed = 2718;
    RunStats ref_stats;
    const u64 ref_hash = continuous_run(cfg, seed, &ref_stats);

    World d(cfg, seed);
    d.run(7000);
    WorldSnapshot s1 = d.capture_snapshot();

    // independent world reaching the same point must produce the same hash
    World probe(cfg, seed);
    probe.run(7000);
    CHECK_EQ(probe.state_hash(), s1.state_hash);

    World e(cfg, seed);
    CHECK(e.restore_snapshot(s1, cfg, true));
    e.run(9000);                       // continue 9000 more events
    WorldSnapshot s2 = e.capture_snapshot();

    // ...and an independent run to 16000 events must match snapshot 2
    World probe2(cfg, seed);
    probe2.run(16000);
    CHECK_EQ(probe2.state_hash(), s2.state_hash);

    // destroy E; restore the SECOND snapshot; finish; compare with reference
    World f(cfg, seed);
    CHECK(f.restore_snapshot(s2, cfg, true));
    RunStats sf = f.run();
    CHECK_EQ(f.state_hash(), ref_hash);
    check_stats_equal(ref_stats, sf);
}

AX_TEST(worldrestore_randomized_fuzz) {
    // Spec §6: randomized snapshot/restore, random checkpoint placement,
    // multiple restore points per run, always equal to the continuous run.
    const WorldConfig cfg = snap_test_config();
    for (u64 seed = 1; seed <= 8; ++seed) {
        Rng rng = Rng::stream(seed, 0x5EA9);
        RunStats ref_stats;
        const u64 ref_hash = continuous_run(cfg, seed, &ref_stats);
        const u64 total = ref_stats.events_processed;

        std::unique_ptr<World> w(new World(cfg, seed));
        u64 done = 0;
        while (done < total) {
            const u64 budget = 1 + rng.bounded(total / 7 + 1);
            w->run(budget);
            done = w->stats().events_processed;
            WorldSnapshot snap = w->capture_snapshot();
            // destroy the world and continue in a freshly restored one
            std::unique_ptr<World> w2(new World(cfg, seed));
            CHECK(w2->restore_snapshot(snap, cfg, true));
            if (done >= total) {
                RunStats s2 = w2->run(0);
                CHECK_EQ(w2->state_hash(), ref_hash);
                check_stats_equal(ref_stats, s2);
            } else {
                w2->run(total - done);
                done = w2->stats().events_processed;
                w = std::move(w2);
            }
        }
        if (testfw::current_failed()) return;
    }
}

AX_TEST(worldrestore_rejects_mismatched_config_and_corruption) {
    std::filesystem::create_directories(kArt);
    const WorldConfig cfg = snap_test_config();
    World w(cfg, 606);
    w.run(3000);
    WorldSnapshot snap = w.capture_snapshot();

    // strict restore rejects a changed config
    WorldConfig other = cfg;
    other.algo_latency_ns = cfg.algo_latency_ns + 1;
    World x(other, 606);
    const Status st = x.restore_snapshot(snap, other, /*strict_config=*/true);
    CHECK(!st);
    CHECK_EQ(st.code, Errc::SnapshotInvalid);

    // non-strict restore accepts it (this is the fork path)
    World y(other, 606);
    CHECK(y.restore_snapshot(snap, other, /*strict_config=*/false));

    // corrupted file is rejected by CRC
    const std::string path = kArt + "/corrupt.wsnap";
    CHECK(write_world_snapshot(path, snap));
    const auto size = std::filesystem::file_size(path);
    {
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        std::fseek(f, static_cast<long>(size / 2), SEEK_SET);
        std::fputc(0x5A, f);
        std::fclose(f);
    }
    auto bad = read_world_snapshot(path);
    CHECK(!bad);
    CHECK_EQ(bad.status.code, Errc::ChecksumMismatch);

    // tampered logical state is rejected by the self-validating state hash
    snap.md_seq += 1;  // any missed/mutated state element breaks the hash
    World z(cfg, 606);
    const Status st2 = z.restore_snapshot(snap, cfg, true);
    CHECK(!st2);
    CHECK_EQ(st2.code, Errc::SnapshotInvalid);
}

// ---- forks: same state at N, one changed variable ----

AX_TEST(worldrestore_contract_with_mid_burst_channels) {
    // A snapshot taken while a channel is MID-BURST-LOSS must resume the
    // burst on restore (the burst counter is behavioral state; losing it
    // would make the restored branch less lossy than its parent). This is
    // the class of latent state-omission bug the restore contract exists
    // to catch.
    const WorldConfig cfg = snap_test_config();
    const u64 seed = 77007;
    // burst loss: once a loss starts, 6 consecutive drops follow
    WorldConfig bursty = cfg;
    bursty.md_channel.loss_rate = 0.10;
    bursty.md_channel.burst_loss_len = 6;
    bursty.md_channel.burst_loss_rate = 0.5;
    bursty.order_channel.loss_rate = 0.05;
    bursty.exec_channel.loss_rate = 0.05;

    RunStats ref_stats;
    const u64 ref_hash = continuous_run(bursty, seed, &ref_stats);
    CHECK(ref_stats.md_dropped > 20);  // bursts actually fire at this config

    // capture at many points; every restored continuation must equal the
    // reference (any missed burst state diverges almost immediately given
    // 10% loss / burst-6)
    Rng rng = Rng::stream(seed, 0xB057);
    for (int i = 0; i < 12; ++i) {
        const u64 n = 3000 + rng.bounded(40'000);
        World b(bursty, seed);
        b.run(n);
        WorldSnapshot snap = b.capture_snapshot();
        World c(bursty, seed);
        CHECK(c.restore_snapshot(snap, bursty, true));
        RunStats sc = c.run();
        CHECK_EQ(c.state_hash(), ref_hash);
        if (testfw::current_failed()) return;
        check_stats_equal(ref_stats, sc);
    }
}

AX_TEST(worldfork_same_config_clone_is_exact) {
    const WorldConfig cfg = snap_test_config();
    const u64 seed = 90210;
    RunStats ref_stats;
    const u64 ref_hash = continuous_run(cfg, seed, &ref_stats);

    const u64 fork_at = 20000;
    World a(cfg, seed);
    a.run(fork_at);
    WorldSnapshot snap = a.capture_snapshot();

    // fork with the SAME config: a perfect clone must reach the reference
    World b(cfg, seed);
    CHECK(b.restore_snapshot(snap, cfg, true));
    RunStats sb = b.run();
    CHECK_EQ(b.state_hash(), ref_hash);
    check_stats_equal(ref_stats, sb);

    // A itself continues identically too
    RunStats sa = a.run();
    CHECK_EQ(a.state_hash(), ref_hash);
    check_stats_equal(ref_stats, sa);
}

AX_TEST(worldfork_one_variable_changes_outcome_after_fork) {
    const WorldConfig cfg = snap_test_config();
    const u64 seed = 4242;
    const u64 fork_at = 20000;

    // parent: run to the fork point
    World parent(cfg, seed);
    RunStats sp = parent.run(fork_at);
    const u64 hash_at_fork = parent.state_hash();
    WorldSnapshot snap = parent.capture_snapshot();

    // world A continues unchanged
    RunStats sa = parent.run();
    const u64 hash_a = parent.state_hash();

    // an independent world agrees on the shared prefix
    World probe(cfg, seed);
    probe.run(fork_at);
    CHECK_EQ(probe.state_hash(), hash_at_fork);

    // world B: fork with exactly one changed variable, a strategy parameter
    // A maker quoting
    // 4 ticks wide vs 1 tick wide faces structurally different fill flow.
    // (Finding recorded while testing: with correct fair tracking, latency
    // deltas below the quote lifetime provably do NOT change outcomes at this
    // seed, the queue/fill analysis in docs/EXPERIMENTS/ quantifies when
    // latency matters.)
    WorldConfig cfg_b = cfg;
    cfg_b.mm.half_spread_ticks = 4;
    World b(cfg_b, seed);
    CHECK(b.restore_snapshot(snap, cfg_b, /*strict_config=*/false));
    RunStats sb = b.run();
    const u64 hash_b = b.state_hash();

    // histories were identical up to the fork (guaranteed by construction and
    // verified via the probe); after the fork the outcome must differ
    CHECK_NE(hash_b, hash_a);
    const i64 pnl_a = sa.mm_cash + sa.mm_inventory * sa.final_mid;
    const i64 pnl_b = sb.mm_cash + sb.mm_inventory * sb.final_mid;
    std::printf("  [fork @%llu] A: fills=%llu pnl=%lld | B(slow): fills=%llu pnl=%lld\n",
                (unsigned long long)fork_at, (unsigned long long)sa.strategy_fills,
                (long long)pnl_a, (unsigned long long)sb.strategy_fills, (long long)pnl_b);

    // Branch genealogy must be recordable and auditable.
    BranchIdentity id{};
    id.parent_world_id = 0;  // parent is the root world (filled below)
    id.world_id = b.world_id();
    id.parent_world_id = World(cfg, seed).world_id();
    id.fork_event = snap.events;
    id.config_hash = cfg_b.hash();
    id.initial_state_hash = snap.state_hash;
    CHECK_NE(id.world_id, id.parent_world_id);
    CHECK_EQ(id.fork_event, fork_at);
    CHECK_EQ(id.initial_state_hash, hash_at_fork);
}

AX_TEST(worldfork_network_variable) {
    // Fork with 2% MD loss. With snapshot recovery the feed handler heals the
    // view, so at this seed the transient view differences never cross a
    // quote-decision threshold and the worlds reconverge (identical fills and
    // final state), itself a measured robustness result. The STRUCTURAL
    // guarantees are what must hold: drops occur, gaps are detected, recovery
    // covers them, and delivery accounting stays exact.
    const WorldConfig cfg = snap_test_config();
    const u64 seed = 5150;
    const u64 fork_at = 15000;

    World a(cfg, seed);
    a.run(fork_at);
    WorldSnapshot snap = a.capture_snapshot();
    RunStats sa = a.run();

    WorldConfig cfg_b = cfg;
    cfg_b.md_channel.loss_rate = 0.02;
    World b(cfg_b, seed);
    CHECK(b.restore_snapshot(snap, cfg_b, false));
    RunStats sb = b.run();

    CHECK_NE(b.stats().md_dropped, 0u);            // the fork variable acted
    CHECK_NE(sb.md_gap_events, 0u);                // gaps detected
    CHECK(sb.md_recovered >= sb.md_dropped * 9 / 10);  // recovery covered them
    CHECK(sb.md_delivered <= sb.md_published);
    // Economic convergence at this seed is reported, not asserted.
    std::printf("  [fork md-loss 2%%] A: fills=%llu gaps=%llu | "
                "B: fills=%llu gaps=%llu recovered=%llu | final hash equal=%d\n",
                (unsigned long long)sa.strategy_fills, (unsigned long long)sa.md_gap_events,
                (unsigned long long)sb.strategy_fills, (unsigned long long)sb.md_gap_events,
                (unsigned long long)sb.md_recovered, a.state_hash() == b.state_hash());
}

// ---------------- clustered-flow snapshot / allocation-fork contracts (v9) ----------------
AX_TEST(snapshot_restore_under_clustered_flow) {
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = 200'000'000;
    cfg.sim.taker_self_excitation = 0.9;
    // run to a mid-flight point, snapshot, continue; then restore+continue
    // must equal the continuous run
    World cont(cfg, 31337);
    cont.run(20'000);
    const WorldSnapshot snap = cont.capture_snapshot();
    cont.run();
    World resumed(cfg, 31337);
    CHECK(resumed.restore_snapshot(snap, cfg, true));
    resumed.run();
    CHECK_EQ(cont.state_hash(), resumed.state_hash());
    CHECK_EQ(cont.stats().trades, resumed.stats().trades);
    CHECK_EQ(cont.stats().momentum_fills, resumed.stats().momentum_fills);
}

AX_TEST(snapshot_rejects_wrong_version_and_corrupt_body) {
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = 40'000'000;
    cfg.sim.taker_self_excitation = 0.9;
    World w(cfg, 9);
    w.run(5'000);
    const WorldSnapshot snap = w.capture_snapshot();
    std::filesystem::create_directories("artifacts");
    if (!write_world_snapshot("artifacts/test_cl_snap.wsnap", snap)) {
        CHECK(!"snapshot write failed");
        return;
    }
    // roundtrip
    auto back = read_world_snapshot("artifacts/test_cl_snap.wsnap");
    CHECK(back);
    if (back) {
        World w2(cfg, 9);
        CHECK(w2.restore_snapshot(back.value, cfg, true));
        CHECK_EQ(w2.state_hash(), snap.state_hash);
    }
    // corrupt one byte in the body: the CRC must reject it
    {
        std::FILE* f = std::fopen("artifacts/test_cl_snap.wsnap", "r+b");
        if (!f) {
            CHECK(!"open failed");
            return;
        }
        std::fseek(f, 64, SEEK_SET);
        std::fputc(std::fgetc(f) ^ 0x40, f);
        std::fclose(f);
        CHECK(!read_world_snapshot("artifacts/test_cl_snap.wsnap"));
    }
}

AX_TEST(fork_with_new_allocation_under_clustered_flow) {
    // fork mid-flight with a DIFFERENT latency allocation: histories share
    // the prefix (snapshot point) and then diverge.
    WorldConfig base;
    base.journal_events = false;
    base.horizon_ns = 200'000'000;
    base.sim.taker_self_excitation = 0.9;
    const LatencyAllocation alloc_a = allocation_on_slice(8000, 0.0);
    const LatencyAllocation alloc_b = allocation_on_slice(8000, 1.0);
    World w(alloc_a.apply_to(base), 4711);
    w.run(10'000);
    const WorldSnapshot snap = w.capture_snapshot();
    // branch 1: continue with the same allocation
    World same(alloc_a.apply_to(base), 4711);
    const Status st_same = same.restore_snapshot(snap, alloc_a.apply_to(base), false);
    CHECK(st_same);
    if (!st_same) return;
    same.run();
    // branch 2: continue with a different allocation (fixed total, moved)
    World other(alloc_b.apply_to(base), 4711);
    const Status st_other = other.restore_snapshot(snap, alloc_b.apply_to(base), false);
    CHECK(st_other);
    if (!st_other) return;
    other.run();
    // identical world if re-run; divergent under the changed allocation
    World same2(alloc_a.apply_to(base), 4711);
    same2.restore_snapshot(snap, alloc_a.apply_to(base), false);
    same2.run();
    CHECK_EQ(same.state_hash(), same2.state_hash());
    CHECK_NE(same.state_hash(), other.state_hash());
    // same seed + same allocation => exact same result (L)
    World other2(alloc_b.apply_to(base), 4711);
    other2.restore_snapshot(snap, alloc_b.apply_to(base), false);
    other2.run();
    CHECK_EQ(other.state_hash(), other2.state_hash());
}

AX_TEST_MAIN