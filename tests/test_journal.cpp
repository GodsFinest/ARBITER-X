#include "testfw.hpp"

#include "arbiter/book_common.hpp"
#include "arbiter/differential.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/event.hpp"
#include "arbiter/journal.hpp"
#include "arbiter/replay.hpp"
#include "arbiter/snapshot.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace arbiter;

namespace {

const std::string kArtDir = "./build/test_artifacts";

struct RunResult {
    u64 final_hash = 0;
    std::vector<u64> hash_after_op;   // [i] = book hash after op i
    std::vector<u64> seq_after_op;    // [i] = journal seq after op i (incl. prints)
    u64 total_events = 0;
};

// Applies ops to a reference book while journaling inputs and outputs, taking
// periodic snapshots + Checkpoint verification events. Returns hashes needed
// by the recovery/time-travel tests.
RunResult run_and_journal(const std::vector<diff::Op>& ops, const std::string& path,
                          u64 checkpoint_every_ops, CheckpointStore* cps) {
    // HYGIENE: a CheckpointStore keeps whatever files are in its directory;
    // stale checkpoints from a previous run would be loaded by
    // load_at_or_before and corrupt the snapshot+tail comparison. Clear it.
    if (cps) std::filesystem::remove_all(cps->dir());
    RunResult run;
    RefBook book{RefBook::Options{}};
    auto w = JournalWriter::create(path, 42, 123);
    if (!w) {
        testfw::current_failed() = true;
        return run;
    }
    std::vector<Fill> fills;
    u64 next_id = 1;
    for (size_t i = 0; i < ops.size(); ++i) {
        diff::Op op = ops[i];
        if (op.kind == diff::Op::Kind::Add) op.ord.id = next_id++;
        if (op.kind == diff::Op::Kind::Replace) op.ord.id = next_id++;
        const LogicalNs t = static_cast<LogicalNs>(i) * 1000;
        fills.clear();
        // NOTE: FOK rejections (InsufficientLiquidity) and cancels of
        // already-filled orders (NotFound) are legitimate outcomes the
        // generator can produce; they are journalled like any other event and
        // replay must reproduce them identically.
        if (op.kind == diff::Op::Kind::Add) {
            Order o = op.ord;
            o.arrival = t;
            book.add_order(o, fills, nullptr);
            w.value.append(ev_book_add(0, t, o));
        } else if (op.kind == diff::Op::Kind::Cancel) {
            book.cancel(op.target);
            w.value.append(ev_book_cancel(0, t, op.target));
        } else {
            Order o = op.ord;
            o.arrival = t;
            book.replace(op.target, o, fills, nullptr);
            w.value.append(ev_book_replace(0, t, op.target, o));
        }
        for (const Fill& f : fills) w.value.append(ev_trade_print(0, t, f));

        run.hash_after_op.push_back(book.state_hash());
        w.value.flush();
        run.seq_after_op.push_back(w.value.records());
        if (cps && checkpoint_every_ops && (i + 1) % checkpoint_every_ops == 0) {
            SnapshotData snap;
            snap.config_hash = 123;
            snap.last_event_seq = w.value.records();
            snap.logical_t = t;
            snap.next_order_id = next_id;
            snap.next_book_seq = book.next_book_seq();
            snap.ladder = book.to_ladder();
            CHECK(cps->save(snap));
            w.value.append(Event{0, t, EvType::Checkpoint, 0, Side::Buy, OrderType::Limit, Tif::GTC,
                                 0, book.state_hash(), 0, 0});
        }
    }
    run.final_hash = book.state_hash();
    run.total_events = w.value.records();
    w.value.sync();
    return run;
}

}  // namespace

AX_TEST(journal_roundtrip_events) {
    std::filesystem::create_directories(kArtDir);
    const std::string path = kArtDir + "/roundtrip.journal";
    {
        auto w = JournalWriter::create(path, 7, 99);
        CHECK(w);
        for (u64 i = 0; i < 1000; ++i) {
            Event e;
            e.type = (i % 3 == 0) ? EvType::MdNew : (i % 3 == 1 ? EvType::MdTrade : EvType::BookCancel);
            e.t = static_cast<i64>(i) * 100;
            e.a = i;
            e.c = i * 2;
            e.p = static_cast<i64>(i) + 1000;
            CHECK(w.value.append(e));
        }
        CHECK_EQ(w.value.records(), 1000u);
        w.value.sync();
    }
    auto r = JournalReader::open(path);
    CHECK(r);
    if (!r) return;
    CHECK_EQ(r.value.header().master_seed, 7u);
    CHECK_EQ(r.value.header().config_hash, 99u);
    u64 n = 0;
    while (r.value.has_next()) {
        auto ev = r.value.next();
        CHECK(ev);
        if (!ev) break;
        CHECK_EQ(ev.value.seq, n + 1);
        CHECK_EQ(static_cast<u64>(ev.value.t), n * 100);
        ++n;
    }
    CHECK_EQ(n, 1000u);
    CHECK(r.value.eof());
    JournalVerifyReport rep = journal_verify(path);
    CHECK(rep.clean);
    CHECK_EQ(rep.records, 1000u);
    CHECK_EQ(rep.last_seq, 1000u);
}

AX_TEST(journal_detects_corruption) {
    std::filesystem::create_directories(kArtDir);
    const std::string path = kArtDir + "/corrupt.journal";
    {
        auto w = JournalWriter::create(path, 1, 1);
        for (u64 i = 0; i < 200; ++i) {
            Event e;
            e.type = EvType::MdTrade;
            e.t = static_cast<i64>(i);
            e.a = i;
            CHECK(w.value.append(e));
        }
        w.value.sync();
    }
    const auto fsize = std::filesystem::file_size(path);

    // (a) flip a payload byte -> checksum mismatch
    {
        const std::string p2 = kArtDir + "/corrupt2.journal";
        std::filesystem::copy_file(path, p2, std::filesystem::copy_options::overwrite_existing);
        std::FILE* f = std::fopen(p2.c_str(), "r+b");
        std::fseek(f, 20 + 8 + 30, SEEK_SET);  // inside first frame payload
        std::fputc(0xFF ^ 0x00, f);
        std::fclose(f);
        JournalVerifyReport rep = journal_verify(p2);
        CHECK(!rep.clean);
        CHECK_EQ(rep.first_problem.code, Errc::ChecksumMismatch);
    }
    // (b) truncate mid-frame -> JournalCorrupt
    {
        const std::string p3 = kArtDir + "/corrupt3.journal";
        std::filesystem::copy_file(path, p3, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::resize_file(p3, 20 + 7 * 64 + 10);  // partial 8th frame
        JournalVerifyReport rep = journal_verify(p3);
        CHECK(!rep.clean);
        CHECK_EQ(rep.first_problem.code, Errc::JournalCorrupt);
    }
    // (c) bad magic
    {
        const std::string p4 = kArtDir + "/corrupt4.journal";
        std::filesystem::copy_file(path, p4, std::filesystem::copy_options::overwrite_existing);
        std::FILE* f = std::fopen(p4.c_str(), "r+b");
        std::fseek(f, 0, SEEK_SET);
        std::fputc('X', f);
        std::fclose(f);
        auto rd = JournalReader::open(p4);
        CHECK(!rd);
        CHECK_EQ(rd.status.code, Errc::JournalCorrupt);
    }
    (void)fsize;
}

AX_TEST(journal_recover_truncates_to_good_prefix) {
    std::filesystem::create_directories(kArtDir);
    const std::string path = kArtDir + "/recover.journal";
    {
        auto w = JournalWriter::create(path, 1, 1);
        for (u64 i = 0; i < 500; ++i) {
            Event e;
            e.type = EvType::MdTrade;
            e.t = static_cast<i64>(i);
            CHECK(w.value.append(e));
        }
        w.value.sync();
    }
    const u64 frame = 8 + kEncodedEventSize;
    constexpr u64 kFileHeaderBytes = 24;
    for (u64 keep_frames : {1ull, 37ull, 250ull, 499ull}) {
        // rebuild the full journal each time: truncation is destructive
        {
            auto w = JournalWriter::create(path, 1, 1);
            for (u64 i = 0; i < 500; ++i) {
                Event e;
                e.type = EvType::MdTrade;
                e.t = static_cast<i64>(i);
                CHECK(w.value.append(e));
            }
            w.value.sync();
        }
        std::filesystem::resize_file(path, kFileHeaderBytes + keep_frames * frame);  // exact cut: clean
        auto kept = journal_recover(path);
        CHECK(kept);
        CHECK_EQ(kept.value, keep_frames);
        JournalVerifyReport rep = journal_verify(path);
        CHECK(rep.clean);
        CHECK_EQ(rep.records, keep_frames);
    }
    // ragged cut: partial trailing frame is discarded
    {
        auto w = JournalWriter::create(path, 1, 1);
        for (u64 i = 0; i < 500; ++i) {
            Event e;
            e.type = EvType::MdTrade;
            e.t = static_cast<i64>(i);
            CHECK(w.value.append(e));
        }
        w.value.sync();
    }
    std::filesystem::resize_file(path, kFileHeaderBytes + 100 * frame + 17);
    auto kept = journal_recover(path);
    CHECK(kept);
    CHECK_EQ(kept.value, 100u);
    JournalVerifyReport rep = journal_verify(path);
    CHECK(rep.clean);
    CHECK_EQ(rep.records, 100u);
}

AX_TEST(replay_reproduces_original_run_exactly) {
    std::filesystem::create_directories(kArtDir);
    const std::string path = kArtDir + "/replay.journal";
    auto ops = diff::random_ops(777, 3000);
    RunResult run = run_and_journal(ops, path, 500, nullptr);
    CHECK_EQ(run.hash_after_op.size(), 3000u);

    for (BookImpl impl : {BookImpl::Reference, BookImpl::Optimized}) {
        ReplayResult rep = replay_journal(path, impl, 0, nullptr);
        CHECK(rep.status);
        if (!rep.status)
            std::printf("  replay problem: %s (ctx %llu)\n", to_cstr(rep.status.code),
                        static_cast<unsigned long long>(rep.status.context));
        CHECK_EQ(rep.trade_mismatches, 0u);
        CHECK_EQ(rep.state_hash, run.final_hash);
        CHECK_EQ(rep.final_seq, run.total_events);
    }
}

AX_TEST(recovery_snapshot_plus_tail_equals_full_replay) {
    std::filesystem::create_directories(kArtDir);
    const std::string path = kArtDir + "/recovery.journal";
    const std::string ck_dir = kArtDir + "/cks";
    CheckpointStore cps(ck_dir, 64);
    auto ops = diff::random_ops(31415, 2000);
    RunResult run = run_and_journal(ops, path, 200, &cps);
    CHECK_EQ(run.hash_after_op.size(), 2000u);

    // Time travel / recovery: for sampled op indices K, reconstruct state at
    // the journal seq reached after op K from (snapshot <= seq) + tail replay,
    // and compare against the hash recorded during the original run.
    for (u64 k : {0ull, 137ull, 400ull, 701ull, 1200ull, 1999ull}) {
        const u64 target_seq = run.seq_after_op[k];
        auto base = cps.load_at_or_before(target_seq);
        if (!base) {
            // No checkpoint covers this point yet (targets before the first
            // checkpoint interval): full replay must still reproduce the hash.
            ReplayResult full = replay_journal(path, BookImpl::Optimized, target_seq, nullptr);
            CHECK(full.status);
            CHECK_EQ(full.state_hash, run.hash_after_op[k]);
            continue;
        }
        ReplayResult rep = replay_journal(path, BookImpl::Optimized, target_seq, &base.value);
        CHECK(rep.status);
        if (!rep.status)
            std::printf("  K=%llu replay problem: %s\n", static_cast<unsigned long long>(k),
                        to_cstr(rep.status.code));
        CHECK_EQ(rep.trade_mismatches, 0u);
        CHECK_EQ(rep.checkpoint_mismatches, 0u);
        CHECK_EQ(rep.state_hash, run.hash_after_op[k]);
    }
}

AX_TEST(replay_from_zero_equals_snapshot_restore_at_every_checkpoint) {
    // Scalability note (was O(N^2)): full replay is the ground truth at a
    // bounded sample of points; snapshot+tail must agree with it at EVERY
    // checkpoint-covered seq (the tail replay is the cheap operation being
    // verified exhaustively, full replay only samples it).
    std::filesystem::create_directories(kArtDir);
    const std::string path = kArtDir + "/tt.journal";
    const std::string ck_dir = kArtDir + "/cks2";
    CheckpointStore cps(ck_dir, 64);
    auto ops = diff::random_ops(2718281, 1200);
    RunResult run = run_and_journal(ops, path, 300, &cps);
    // ground truth: full replay at 5 sample points
    std::vector<u64> sample_seqs;
    for (size_t k : {0ull, 137ull, 400ull, 701ull, 1199ull})
        sample_seqs.push_back(run.seq_after_op[k]);
    std::vector<u64> sample_hashes;
    for (u64 seq : sample_seqs) {
        ReplayResult full = replay_journal(path, BookImpl::Optimized, seq, nullptr);
        CHECK(full.status);
        sample_hashes.push_back(full.state_hash);
    }
    // snapshot+tail must agree with the full replay at every checkpoint-
    // covered seq, including all sampled points
    size_t sample_i = 0;
    for (u64 seq : run.seq_after_op) {
        auto base = cps.load_at_or_before(seq);
        if (!base) continue;
        ReplayResult tail = replay_journal(path, BookImpl::Optimized, seq, &base.value);
        CHECK(tail.status);
        CHECK_EQ(tail.checkpoint_mismatches, 0u);
        CHECK_EQ(tail.trade_mismatches, 0u);
        while (sample_i < sample_seqs.size() && seq > sample_seqs[sample_i]) ++sample_i;
        if (sample_i < sample_seqs.size() && seq == sample_seqs[sample_i])
            CHECK_EQ(tail.state_hash, sample_hashes[sample_i]);
    }
}

AX_TEST(snapshot_roundtrip) {
    std::filesystem::create_directories(kArtDir);
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    for (u64 i = 1; i <= 30; ++i)
        b.add_order(Order{i, 1, i % 2 ? Side::Buy : Side::Sell, OrderType::Limit, Tif::GTC,
                          1000 + static_cast<Price>(i), 5, 5, static_cast<LogicalNs>(i), i * 10, 0},
                    fills, nullptr);
    SnapshotData snap;
    snap.config_hash = 5;
    snap.last_event_seq = 100;
    snap.logical_t = 9999;
    snap.next_order_id = 31;
    snap.next_book_seq = b.next_book_seq();
    snap.ladder = b.to_ladder();
    const std::string p = kArtDir + "/snap.snap";
    CHECK(write_snapshot(p, snap));
    auto back = read_snapshot(p);
    CHECK(back);
    if (!back) return;
    CHECK_EQ(back.value.ladder.bids.size(), snap.ladder.bids.size());
    CHECK_EQ(back.value.ladder.asks.size(), snap.ladder.asks.size());
    CHECK_EQ(canonical_book_hash(back.value.ladder), canonical_book_hash(snap.ladder));
    CHECK_EQ(back.value.last_event_seq, 100u);
    // corruption detection
    std::FILE* f = std::fopen(p.c_str(), "r+b");
    std::fseek(f, 40, SEEK_SET);
    std::fputc(0xAB, f);
    std::fclose(f);
    auto bad = read_snapshot(p);
    CHECK(!bad);
}

AX_TEST(event_decode_fuzz_never_crashes) {
    Rng rng = Rng::stream(2026, 5);
    Event e;
    e.type = EvType::BookAdd;
    e.a = 12;
    e.c = 5;
    e.p = 1500;
    ByteWriter w;
    encode_event(w, e);
    const std::string orig = w.data();
    CHECK_EQ(orig.size(), kEncodedEventSize);

    for (int i = 0; i < 20000; ++i) {
        std::string mut = orig;
        const u64 mutations = 1 + rng.bounded(4);
        for (u64 m = 0; m < mutations; ++m) {
            const size_t pos = static_cast<size_t>(rng.bounded(mut.size()));
            mut[pos] = static_cast<char>(rng.next_u32());
        }
        auto r = decode_event(reinterpret_cast<const u8*>(mut.data()), mut.size());
        (void)r;  // must be either a valid event or a clean error — never a crash
    }
    // random garbage of random lengths
    for (int i = 0; i < 20000; ++i) {
        std::string garbage(static_cast<size_t>(rng.bounded(200)), '\0');
        for (auto& c : garbage) c = static_cast<char>(rng.next_u32());
        auto r = decode_event(reinterpret_cast<const u8*>(garbage.data()), garbage.size());
        (void)r;
    }
}

AX_TEST_MAIN