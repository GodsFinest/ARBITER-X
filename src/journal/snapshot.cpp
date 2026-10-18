#include "arbiter/snapshot.hpp"
#include "arbiter/codec.hpp"
#include "arbiter/crc32.hpp"
#include "arbiter/errors.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace arbiter {

namespace {
constexpr char kMagic[4] = {'A', 'R', 'X', 'S'};
constexpr size_t kHeaderSize = 16;  // magic 4 + version 4 + body_len 4 + crc 4

void encode_ladder_side(ByteWriter& w, const std::vector<LadderLevel>& levels) {
    w.put_u64(levels.size());
    for (const auto& lvl : levels) {
        w.put_i64(lvl.price);
        w.put_u64(lvl.total_qty);
        w.put_u64(lvl.orders.size());
        for (const auto& o : lvl.orders) {
            w.put_u64(o.id);
            w.put_u64(o.leaves);
            w.put_u64(o.qty);
        }
    }
}

bool decode_ladder_side(ByteReader& r, std::vector<LadderLevel>& out) {
    const u64 n = r.get_u64();
    if (!r.ok() || n > (1u << 20)) return false;
    out.reserve(static_cast<size_t>(n));
    for (u64 i = 0; i < n; ++i) {
        LadderLevel lvl;
        lvl.price = r.get_i64();
        lvl.total_qty = r.get_u64();
        const u64 orders = r.get_u64();
        if (!r.ok() || orders > (1u << 24)) return false;
        lvl.orders.reserve(static_cast<size_t>(orders));
        for (u64 j = 0; j < orders; ++j) {
            LadderOrder o;
            o.id = r.get_u64();
            o.leaves = r.get_u64();
            o.qty = r.get_u64();
            if (!r.ok()) return false;
            lvl.orders.push_back(o);
        }
        out.push_back(std::move(lvl));
    }
    return true;
}

void ensure_dir(const std::string& dir) {
    ::mkdir(dir.c_str(), 0755);  // ok if exists
}

}  // namespace

Status write_snapshot(const std::string& path, const SnapshotData& snap) {
    ByteWriter body;
    body.put_u64(snap.config_hash);
    body.put_u64(snap.last_event_seq);
    body.put_i64(snap.logical_t);
    body.put_u64(snap.next_order_id);
    body.put_u64(snap.next_book_seq);
    encode_ladder_side(body, snap.ladder.bids);
    encode_ladder_side(body, snap.ladder.asks);

    ByteWriter head;
    head.put_bytes(kMagic, 4);
    head.put_u32(1);
    head.put_u32(static_cast<u32>(body.size()));
    head.put_u32(crc32(body.data().data(), body.size()));

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return fail(Errc::IoError, static_cast<u64>(errno));
    const std::string& h = head.data();
    const std::string& b = body.data();
    const bool ok = std::fwrite(h.data(), 1, h.size(), f) == h.size() &&
                    std::fwrite(b.data(), 1, b.size(), f) == b.size();
    const bool cerr_ = std::fclose(f) == 0;
    if (!ok || !cerr_) return fail(Errc::IoError, 1);
    return good();
}

Result<SnapshotData> read_snapshot(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return ErrT<SnapshotData>(Errc::NotFound, static_cast<u64>(errno));
    std::string all;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    std::fclose(f);

    if (all.size() < kHeaderSize) return ErrT<SnapshotData>(Errc::SnapshotInvalid, 0);
    const u8* p = reinterpret_cast<const u8*>(all.data());
    if (std::memcmp(p, kMagic, 4) != 0) return ErrT<SnapshotData>(Errc::SnapshotInvalid, 0);
    ByteReader hr(p + 4, 12);
    const u32 version = hr.get_u32();
    const u32 body_len = hr.get_u32();
    const u32 crc = hr.get_u32();
    if (!hr.ok() || version != 1) return ErrT<SnapshotData>(Errc::SnapshotInvalid, 1);
    if (all.size() != kHeaderSize + body_len) return ErrT<SnapshotData>(Errc::SnapshotInvalid, 2);
    if (crc32(p + kHeaderSize, body_len) != crc) return ErrT<SnapshotData>(Errc::ChecksumMismatch, 0);

    ByteReader r(p + kHeaderSize, body_len);
    SnapshotData snap;
    snap.config_hash = r.get_u64();
    snap.last_event_seq = r.get_u64();
    snap.logical_t = r.get_i64();
    snap.next_order_id = r.get_u64();
    snap.next_book_seq = r.get_u64();
    if (!r.ok()) return ErrT<SnapshotData>(Errc::SnapshotInvalid, 3);
    if (!decode_ladder_side(r, snap.ladder.bids)) return ErrT<SnapshotData>(Errc::SnapshotInvalid, 4);
    if (!decode_ladder_side(r, snap.ladder.asks)) return ErrT<SnapshotData>(Errc::SnapshotInvalid, 5);
    if (!r.ok() || r.remaining() != 0) return ErrT<SnapshotData>(Errc::SnapshotInvalid, 6);
    return OkT(std::move(snap));
}

CheckpointStore::CheckpointStore(std::string dir, size_t keep) : dir_(std::move(dir)), keep_(keep) {}

Status CheckpointStore::save(const SnapshotData& snap) {
    ensure_dir(dir_);
    const std::string path = dir_ + "/ck_" + std::to_string(snap.last_event_seq) + ".snap";
    if (Status s = write_snapshot(path, snap); !s) return s;

    // prune oldest beyond keep_
    std::vector<u64> seqs;
    if (DIR* d = ::opendir(dir_.c_str())) {
        while (struct dirent* de = ::readdir(d)) {
            std::string name = de->d_name;
            if (name.rfind("ck_", 0) == 0 && name.size() > 9 && name.substr(name.size() - 5) == ".snap") {
                u64 v = std::strtoull(name.c_str() + 3, nullptr, 10);
                seqs.push_back(v);
            }
        }
        ::closedir(d);
    }
    if (seqs.size() > keep_) {
        std::sort(seqs.begin(), seqs.end());
        const size_t excess = seqs.size() - keep_;
        for (size_t i = 0; i < excess; ++i)
            std::remove((dir_ + "/ck_" + std::to_string(seqs[i]) + ".snap").c_str());
    }
    return good();
}

std::vector<u64> CheckpointStore::available_seqs() const {
    std::vector<u64> seqs;
    if (DIR* d = ::opendir(dir_.c_str())) {
        while (struct dirent* de = ::readdir(d)) {
            std::string name = de->d_name;
            if (name.rfind("ck_", 0) == 0 && name.size() > 9 && name.substr(name.size() - 5) == ".snap") {
                u64 v = std::strtoull(name.c_str() + 3, nullptr, 10);
                seqs.push_back(v);
            }
        }
        ::closedir(d);
    }
    std::sort(seqs.begin(), seqs.end());
    return seqs;
}

Result<SnapshotData> CheckpointStore::load_at_or_before(u64 seq) const {
    u64 best = 0;
    bool found = false;
    for (u64 s : available_seqs()) {
        if (s <= seq && (!found || s > best)) {
            best = s;
            found = true;
        }
    }
    if (!found) return ErrT<SnapshotData>(Errc::NotFound, seq);
    return read_snapshot(dir_ + "/ck_" + std::to_string(best) + ".snap");
}

}  // namespace arbiter