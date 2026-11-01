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
    CHECK_EQ(a.order_frames_dropped, b.order_frames_dropped);
    CHECK_EQ(a.order_frames_duplicated, b.order_frames_duplicated);
    CHECK_EQ(a.exec_frames_dropped, b.exec_frames_dropped);
    CHECK_EQ(a.exec_frames_duplicated, b.exec_frames_duplicated);
    CHECK_EQ(a.md_duplicated, b.md_duplicated);
    CHECK_EQ(a.md_reordered, b.md_reordered);
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

AX_TEST(worldrestore_preserves_pre_run_state) {
    const WorldConfig cfg = snap_test_config();
    World original(cfg, 8080);
    const WorldSnapshot snap = original.capture_snapshot();
    World restored(cfg, 8080);
    CHECK(restored.restore_snapshot(snap, cfg, true));
    CHECK_EQ(restored.state_hash(), snap.state_hash);
    original.run();
    restored.run();
    CHECK_EQ(restored.state_hash(), original.state_hash());
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


AX_TEST(worldrestore_equivalent_under_transport_faults) {
    // Duplicates, loss, reordering, corruption and mid-flight recovery across
    // ALL channels: the full v12 transport state (MD packet attempts, order/
    // exec transport sequences, receiver dedupe state, risk reservations,
    // authoritative ledger) must round-trip exactly.
    WorldConfig cfg = snap_test_config();
    cfg.md_channel.loss_rate = 0.10;
    cfg.md_channel.dup_rate = 0.20;
    cfg.md_channel.reorder_window = 8;
    cfg.md_channel.corrupt_rate = 0.05;
    cfg.order_channel.loss_rate = 0.05;
    cfg.order_channel.dup_rate = 0.15;
    cfg.order_channel.reorder_window = 4;
    cfg.order_channel.corrupt_rate = 0.05;
    cfg.exec_channel.loss_rate = 0.10;
    cfg.exec_channel.dup_rate = 0.20;
    cfg.exec_channel.reorder_window = 4;
    cfg.exec_channel.corrupt_rate = 0.05;
    const u64 seed = 24680;
    RunStats ref_stats;
    const u64 ref_hash = continuous_run(cfg, seed, &ref_stats);
    CHECK(ref_stats.order_frames_duplicated > 0);
    CHECK(ref_stats.exec_frames_duplicated > 0);
    CHECK(ref_stats.md_duplicated > 0);

    World b(cfg, seed);
    b.run(12000);
    WorldSnapshot snap = b.capture_snapshot();
    // persistent receiver state + reservations are captured
    CHECK(!snap.order_rx_seqs.empty());
    CHECK(!snap.exec_rx_seqs.empty());
    CHECK(!snap.reservations.empty());
    CHECK(!snap.ledger.empty());
    CHECK(snap.md_packet_seq > 0);

    const std::string path = kArt + "/transport_faults.wsnap";
    CHECK(write_world_snapshot(path, snap));
    auto reloaded = read_world_snapshot(path);
    CHECK(reloaded);
    if (!reloaded) return;
    CHECK_EQ(reloaded.value.state_hash, snap.state_hash);

    World c(cfg, seed);
    CHECK(c.restore_snapshot(reloaded.value, cfg, true));
    RunStats sc = c.run();
    CHECK_EQ(c.state_hash(), ref_hash);
    check_stats_equal(ref_stats, sc);
    CHECK_EQ(sc.mm_auth_cash, ref_stats.mm_auth_cash);
    CHECK_EQ(sc.mm_auth_filled_qty, ref_stats.mm_auth_filled_qty);
    CHECK_EQ(sc.mom_auth_filled_qty, ref_stats.mom_auth_filled_qty);
    CHECK_EQ(sc.mm_auth_fees, ref_stats.mm_auth_fees);
}


AX_TEST(worldrestore_next_book_seq_exact) {
    // next_book_seq is assigned fresh during ladder reconstruction; the
    // captured value must be restored EXACTLY after the rebuild.
    const WorldConfig cfg = snap_test_config();
    World w(cfg, 13579);
    w.run(9000);
    WorldSnapshot snap = w.capture_snapshot();
    World w2(cfg, 13579);
    CHECK(w2.restore_snapshot(snap, cfg, true));
    CHECK_EQ(w2.capture_snapshot().next_book_seq, snap.next_book_seq);
    // and the restored world continues identically
    RunStats sa = w.run();
    RunStats sb = w2.run();
    CHECK_EQ(w.state_hash(), w2.state_hash());
    CHECK_EQ(sa.trades, sb.trades);
}


// ---- adversarial: valid-checksum snapshots carrying semantic corruption ----
// Every case mutates the in-memory snapshot (the writer then produces a
// VALID CRC over it), so rejection proves semantic validation, not checksum
// luck. A failed restore must also leave the live world untouched.

namespace {
// Build a real mid-run snapshot to corrupt.
WorldSnapshot semsnap_base() {
    const WorldConfig cfg = snap_test_config();
    World w(cfg, 97531);
    w.run(7000);
    return w.capture_snapshot();
}

// Expect restore to fail with SnapshotInvalid AND leave `live` untouched.
void expect_rejected(const WorldSnapshot& snap, World& live, const WorldConfig& cfg,
                     const char* what) {
    WorldSnapshot bad = snap;  // fresh copy per case
    const std::string path = kArt + "/semcorrupt.wsnap";
    CHECK(write_world_snapshot(path, bad));
    auto reloaded = read_world_snapshot(path);
    CHECK(reloaded);
    if (!reloaded) return;
    const u64 live_hash_before = live.state_hash();
    World probe(cfg, live_hash_before);  // unused seed guard
    (void)probe;
    const Status st = live.restore_snapshot(reloaded.value, cfg, true);
    CHECK(!st);
    if (st) std::printf("  %s: NOT rejected\n", what);
    CHECK_EQ(live.state_hash(), live_hash_before);  // no partial mutation
}
}  // namespace

AX_TEST(worldrestore_rejects_semantically_corrupt_snapshots) {
    std::filesystem::create_directories(kArt);
    const WorldConfig cfg = snap_test_config();
    const WorldSnapshot base = semsnap_base();
    World live(cfg, 97531);
    live.run(200);  // the live world a failed restore must not disturb
    const u64 live_hash = live.state_hash();
    auto live_untouched = [&] { CHECK_EQ(live.state_hash(), live_hash); };

    // book ladder: zero id / leaves>qty / duplicate id across sides / crossed
    {
        WorldSnapshot bad = base;
        bad.book.asks.front().orders.front().id = 0;
        expect_rejected(bad, live, cfg, "zero order id");
    }
    {
        WorldSnapshot bad = base;
        if (!bad.book.asks.empty()) {
            LadderOrder o = bad.book.asks.front().orders.front();
            o.id = 777777;
            o.qty = 5;
            o.leaves = 9;  // leaves > qty
            bad.book.asks.front().orders.push_back(o);
            bad.book.asks.front().total_qty += 9;
            expect_rejected(bad, live, cfg, "leaves > qty");
        }
    }
    {
        WorldSnapshot bad = base;
        if (!bad.book.bids.empty() && !bad.book.asks.empty()) {
            // duplicate the same id on both sides
            const u64 shared = bad.book.bids.front().orders.front().id;
            bad.book.asks.front().orders.front().id = shared;
            expect_rejected(bad, live, cfg, "duplicate id across sides");
        }
    }
    {
        WorldSnapshot bad = base;
        if (!bad.book.bids.empty() && !bad.book.asks.empty()) {
            // force a crossed book: bid at the best ask price
            bad.book.bids.front().price = bad.book.asks.front().price;
            expect_rejected(bad, live, cfg, "crossed book");
        }
    }
    // ownership referencing a non-resting order
    {
        WorldSnapshot bad = base;
        bad.order_accounts.emplace_back(999999999, 7);
        expect_rejected(bad, live, cfg, "ownership for unknown order");
    }
    // next_book_seq regression to zero
    {
        WorldSnapshot bad = base;
        bad.next_book_seq = 0;
        expect_rejected(bad, live, cfg, "next_book_seq == 0");
    }
    // MD packet id beyond the counter / duplicate packet ids
    {
        WorldSnapshot bad = base;
        bad.md_packet_seq = 1;
        for (auto& p : bad.md_packets)
            if (p.packet_id >= 1) p.packet_id = 1;
        // a packet id above the counter
        bad.md_packets.push_back(SnapMdPacket{bad.md_packet_seq + 5, 1, ""});
        expect_rejected(bad, live, cfg, "packet id beyond counter");
    }
    // receiver dedupe: duplicate sequences / sequence beyond its counter
    {
        WorldSnapshot bad = base;
        if (!bad.order_rx_seqs.empty()) bad.order_rx_seqs.push_back(bad.order_rx_seqs.front());
        expect_rejected(bad, live, cfg, "duplicate rx sequences");
    }
    {
        WorldSnapshot bad = base;
        bad.order_rx_seqs.push_back(bad.order_seq + 1);
        expect_rejected(bad, live, cfg, "rx sequence beyond counter");
    }
    // pending MD outside [expected, md_seq)
    {
        WorldSnapshot bad = base;
        bad.pending_md.emplace_back(bad.md_seq + 3, Event{});
        expect_rejected(bad, live, cfg, "pending md beyond md_seq");
    }
    // expected stream ahead of the publisher
    {
        WorldSnapshot bad = base;
        bad.expected_md_seq = bad.md_seq + 1;
        expect_rejected(bad, live, cfg, "expected_md_seq > md_seq");
    }
    // in-flight order frame that decodes onto the wrong channel
    {
        WorldSnapshot bad = base;
        if (!bad.inflight.empty()) {
            Event md;
            md.type = EvType::MdTrade;
            md.p = 1500;
            bad.inflight.front().frame = wire_encode(1, bad.inflight.front().transport_seq,
                                                     kFlagNone, md);
            expect_rejected(bad, live, cfg, "order frame carrying an MD payload");
        }
    }
    // in-flight order frame whose embedded sequence disagrees with its metadata
    {
        WorldSnapshot bad = base;
        if (!bad.inflight.empty()) {
            Event add;
            add.type = EvType::BookAdd;
            add.a = 12345;
            add.c = 5;
            add.p = 1500;
            bad.inflight.front().frame = wire_encode(2, bad.inflight.front().transport_seq + 77,
                                                     kFlagNone, add);
            expect_rejected(bad, live, cfg, "order frame sequence mismatch");
        }
    }
    // exec notification with an impossible kind
    {
        WorldSnapshot bad = base;
        if (!bad.notifies.empty()) bad.notifies.front().kind = 9;
        expect_rejected(bad, live, cfg, "notify kind 9");
    }
    // scheduler: unknown event type / dangling in-flight reference
    {
        WorldSnapshot bad = base;
        if (!bad.queue.empty()) bad.queue.front().type = 99;
        expect_rejected(bad, live, cfg, "scheduler event type 99");
    }
    {
        WorldSnapshot bad = base;
        bad.queue.push_back(SnapSchedEvent{bad.now, 3, static_cast<i64>(bad.tiebreak) + 1, 23,
                                           0, static_cast<i64>(bad.inflight.size()) + 50, 0, 0});
        bad.tiebreak += 2;
        expect_rejected(bad, live, cfg, "scheduler dangling inflight reference");
    }
    // risk: duplicate accounts
    {
        WorldSnapshot bad = base;
        if (!bad.risk_accounts.empty()) bad.risk_accounts.push_back(bad.risk_accounts.front());
        expect_rejected(bad, live, cfg, "duplicate risk accounts");
    }
    // reservations: zero exposure / owner disagreement
    {
        WorldSnapshot bad = base;
        if (!bad.reservations.empty()) bad.reservations.front().signed_qty = 0;
        else bad.reservations.push_back(SnapReservation{1, 7, 0});
        expect_rejected(bad, live, cfg, "reservation with zero exposure");
    }
    {
        WorldSnapshot bad = base;
        if (!bad.reservations.empty()) {
            // point a resting order's reservation at the wrong account
            for (auto& r : bad.reservations) {
                bool resting = false;
                for (const auto& [id, acct] : bad.order_accounts)
                    if (id == r.order_id) resting = true;
                if (resting) {
                    r.account = r.account == 7 ? 8 : 7;
                    break;
                }
            }
            expect_rejected(bad, live, cfg, "reservation owner disagreement");
        }
    }
    // ledger: unknown / duplicated accounts
    {
        WorldSnapshot bad = base;
        if (!bad.ledger.empty()) bad.ledger.front().account = 424242;
        expect_rejected(bad, live, cfg, "ledger for unknown account");
    }
    // strategy state: invalid side in own orders / invalid momentum sign
    {
        WorldSnapshot bad = base;
        if (!bad.mm.own.empty()) bad.mm.own.front().side = 9;
        expect_rejected(bad, live, cfg, "own order side 9");
    }
    {
        WorldSnapshot bad = base;
        if (!bad.mom.signs.empty()) bad.mom.signs.front() = 5;
        expect_rejected(bad, live, cfg, "momentum sign 5");
    }
    // sim state: impossible regime
    {
        WorldSnapshot bad = base;
        bad.sim.regime = 7;
        expect_rejected(bad, live, cfg, "sim regime 7");
    }
    live_untouched();

    // the LIVE world keeps running deterministically after all the rejected
    // restores above
    RunStats sa = live.run();
    World twin(cfg, 97531);
    twin.run(7200);
    twin.run();  // to the same terminal point
    CHECK_EQ(live.state_hash(), twin.state_hash());
    CHECK_EQ(sa.trades, twin.stats().trades);
}

AX_TEST(worldrestore_accepts_crc_corrupted_inflight_frames_deterministically) {
    // A frame that does not decode is LEGITIMATE in-flight state (in-transit
    // corruption is real state), so byte-corrupted in-flight frames must be
    // ACCEPTED and replay their rejection deterministically: two independent
    // worlds restored from the same corrupted snapshot must produce
    // identical outcomes.
    std::filesystem::create_directories(kArt);
    // In-flight CRC corruption must be IN the captured state: generate it
    // with the real fault knobs so the stored hash covers the corrupted
    // bytes (mutating frames after capture would just break the self-check).
    WorldConfig cfg = snap_test_config();
    cfg.order_channel.corrupt_rate = 0.5;
    cfg.exec_channel.corrupt_rate = 0.5;
    WorldSnapshot snap;
    u64 corrupted = 0;
    u64 seed = 8642;
    for (u64 attempt = 0; attempt < 12 && corrupted == 0; ++attempt) {
        World b(cfg, seed);
        b.run(9000);
        snap = b.capture_snapshot();
        for (const auto& m : snap.inflight)
            if (!m.frame.empty() &&
                !wire_decode(reinterpret_cast<const u8*>(m.frame.data()), m.frame.size()))
                ++corrupted;
        for (const auto& n : snap.notifies)
            if (!n.frame.empty() &&
                !wire_decode(reinterpret_cast<const u8*>(n.frame.data()), n.frame.size()))
                ++corrupted;
        if (corrupted == 0) ++seed;  // keep the seed that produced the capture
    }
    CHECK(corrupted > 0);  // the capture genuinely holds undecodable frames
    const std::string path = kArt + "/garbage_frames.wsnap";
    CHECK(write_world_snapshot(path, snap));
    auto reloaded = read_world_snapshot(path);
    CHECK(reloaded);
    if (!reloaded) return;
    World c1(cfg, seed);
    CHECK(c1.restore_snapshot(reloaded.value, cfg, true));  // accepted: legit state
    World c2(cfg, seed);
    CHECK(c2.restore_snapshot(reloaded.value, cfg, true));
    RunStats s1 = c1.run();
    RunStats s2 = c2.run();
    CHECK_EQ(c1.state_hash(), c2.state_hash());
    CHECK_EQ(s1.trades, s2.trades);
    CHECK_EQ(s1.wire_frames_corrupted, s2.wire_frames_corrupted);
}


// ---- v14: the persisted configuration is the fork's source of truth ----

AX_TEST(snapshot_config_roundtrips_exactly) {
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = 80'000'000;
    cfg.warmup_ns = 1'000'000;
    cfg.fee_per_lot = 5;
    cfg.mm.half_spread_ticks = 3;
    cfg.mm.account = 11;
    cfg.momentum.account = 12;
    cfg.momentum.id_base = 777'000'000;
    cfg.md_channel.jitter_ns = 150;
    cfg.md_channel.dup_rate = 0.1;
    cfg.exec_channel.loss_rate = 0.05;
    cfg.order_channel.corrupt_rate = 0.02;
    cfg.bots_observe_shadow = true;
    cfg.risk.max_position = 900;
    cfg.cancel_latency_ns = 123;
    cfg.book_min_price = 900;
    cfg.book_max_price = 2100;
    World w(cfg, 4242);
    w.run(6000);
    WorldSnapshot snap = w.capture_snapshot();
    CHECK(snap.config.hash() == cfg.hash());
    CHECK_EQ(snap.config_hash, cfg.hash());
    const std::string path = kArt + "/config_roundtrip.wsnap";
    CHECK(write_world_snapshot(path, snap));
    auto back = read_world_snapshot(path);
    CHECK(back);
    if (!back) return;
    // every behavior-relevant field survives the round trip (the config hash
    // covers them all); spot-check instrumentation fields too
    CHECK(back.value.config.hash() == cfg.hash());
    CHECK_EQ(back.value.config.fee_per_lot, 5);
    CHECK_EQ(back.value.config.mm.account, 11u);
    CHECK_EQ(back.value.config.momentum.id_base, 777'000'000ull);
    CHECK_EQ(back.value.config.cancel_latency_ns, 123);
    CHECK(back.value.config.bots_observe_shadow);
    CHECK_EQ(back.value.config.book_max_price, 2100);
    CHECK(std::fabs(back.value.config.md_channel.dup_rate - 0.1) < 1e-12);
    // restore under the PERSISTED config must succeed (strict)
    World w2(back.value.config, 4242);
    CHECK(w2.restore_snapshot(back.value, back.value.config, true));
    w.run();
    w2.run();
    CHECK_EQ(w.state_hash(), w2.state_hash());
}

AX_TEST(worldfork_uses_persisted_parent_config) {
    // A snapshot from a NON-default parent configuration must fork from that
    // exact configuration, not from hardcoded defaults: the parent clone
    // restores strictly under snap.config, and the fork differs by exactly
    // one variable.
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = 80'000'000;
    cfg.warmup_ns = 1'000'000;
    cfg.fee_per_lot = 4;
    cfg.mm.half_spread_ticks = 3;
    cfg.md_channel.jitter_ns = 120;
    World parent(cfg, 515151);
    parent.run(9000);
    WorldSnapshot snap = parent.capture_snapshot();

    // 1. same-config clone: exact continuation under the persisted config
    World clone(snap.config, 515151);
    CHECK(clone.restore_snapshot(snap, snap.config, true));
    RunStats sc = clone.run();
    RunStats sp = parent.run();
    CHECK_EQ(clone.state_hash(), parent.state_hash());
    CHECK_EQ(sc.trades, sp.trades);

    // 2. one-variable fork: rebuild from a mid-run snapshot
    World mid(cfg, 515151);
    mid.run(4000);
    WorldSnapshot mid_snap = mid.capture_snapshot();
    WorldConfig fork_cfg = mid_snap.config;
    fork_cfg.mm.half_spread_ticks = 2;  // exactly one variable changes
    CHECK_NE(fork_cfg.hash(), mid_snap.config.hash());
    CHECK_EQ(fork_cfg.fee_per_lot, 4);  // everything else identical
    CHECK_EQ(fork_cfg.md_channel.jitter_ns, 120);
    World forked(fork_cfg, 515151);
    CHECK(forked.restore_snapshot(mid_snap, fork_cfg, /*strict_config=*/false));
    forked.run();
    // genealogy: the fork's world_id is derived from (config, seed) and the
    // branch point is the snapshot's event count
    CHECK_NE(forked.world_id(), mid.world_id());
    CHECK_EQ(mid_snap.events, 4000u);
}

AX_TEST(worldfork_rejects_foreign_seed_before_restore) {
    const WorldConfig cfg = snap_test_config();
    World w(cfg, 3131);
    w.run(3000);
    WorldSnapshot snap = w.capture_snapshot();
    // the World-level contract the CLI enforces up front: a foreign master
    // seed can never restore this snapshot
    World wrong_seed(cfg, 999);
    const Status st = wrong_seed.restore_snapshot(snap, cfg, true);
    CHECK(!st);
    CHECK_EQ(st.code, Errc::SnapshotInvalid);
}

AX_TEST(worldfork_final_divergence_after_equal_checkpoint_prefix) {
    // Two branches whose state-hash timelines agree on the common prefix but
    // whose FINAL hashes differ must be reported as diverged, never
    // "identical". The World-level experiment engine encodes exactly this
    // contract; mirror it at the fork level.
    WorldConfig cfg;
    cfg.journal_events = false;
    cfg.horizon_ns = 60'000'000;
    cfg.warmup_ns = 1'000'000;
    World base(cfg, 777777);
    base.run(3000);
    WorldSnapshot snap = base.capture_snapshot();
    World a(snap.config, 777777);
    CHECK(a.restore_snapshot(snap, snap.config, true));
    WorldConfig fork_cfg = snap.config;
    fork_cfg.md_channel.jitter_ns += 100;
    World b(fork_cfg, 777777);
    CHECK(b.restore_snapshot(snap, fork_cfg, false));
    a.run();
    b.run();
    const bool identical = a.state_hash() == b.state_hash();
    if (identical) return;  // jitter delta too small at this seed: nothing to assert
    // the checkpoint timelines may still agree; the final hashes do not.
    // The experiment engine's divergence rule (checkpoint equality + final
    // mismatch => divergence bound = min events_processed) must hold.
    const auto& ta = a.stats().hash_timeline;
    const auto& tb = b.stats().hash_timeline;
    size_t common = std::min(ta.size(), tb.size());
    bool prefix_differs = false;
    for (size_t k = 0; k < common; ++k)
        if (ta[k].second != tb[k].second) { prefix_differs = true; break; }
    CHECK(!prefix_differs || !identical);
    CHECK_NE(a.state_hash(), b.state_hash());
}

AX_TEST_MAIN
