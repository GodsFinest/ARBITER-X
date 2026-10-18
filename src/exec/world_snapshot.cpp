#include "arbiter/world_snapshot.hpp"
#include "arbiter/crc32.hpp"
#include "arbiter/errors.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>

namespace arbiter {

namespace {
constexpr char kMagic[4] = {'A', 'R', 'X', 'W'};
constexpr size_t kHeaderSize = 16;  // magic 4 + version 4 + body_len 4 + crc 4
// NOTE: format v3 adds sim.last_bb/last_ba/now (touch used by provider bots).

// ---- helpers ----
void w_order(ByteWriter& w, const SnapOrder& o) {
    w.put_u64(o.id);
    w.put_u64(o.book_seq);
    w.put_u32(o.account);
    w.put_u8(o.side);
    w.put_u8(o.otype);
    w.put_u8(o.tif);
    w.put_i64(o.price);
    w.put_u64(o.qty);
    w.put_u64(o.leaves);
    w.put_i64(o.arrival);
    w.put_i64(o.expiry);
}

bool r_order(ByteReader& r, SnapOrder& o) {
    o.id = r.get_u64();
    o.book_seq = r.get_u64();
    o.account = r.get_u32();
    o.side = r.get_u8();
    o.otype = r.get_u8();
    o.tif = r.get_u8();
    o.price = r.get_i64();
    o.qty = r.get_u64();
    o.leaves = r.get_u64();
    o.arrival = r.get_i64();
    o.expiry = r.get_i64();
    return r.ok();
}

bool w_event_field(ByteWriter& w, const Event& e) {
    ByteWriter ev;
    encode_event(ev, e);
    const std::string& bytes = ev.data();
    if (bytes.size() != kEncodedEventSize) return false;
    w.put_bytes(bytes.data(), bytes.size());
    return true;
}

bool r_event_field(ByteReader& r, Event& e) {
    u8 buf[kEncodedEventSize];
    if (!r.get_bytes(buf, sizeof buf)) return false;
    auto ev = decode_event(buf, sizeof buf);
    if (!ev) return false;
    e = ev.value;
    return true;
}

void w_ladder(ByteWriter& w, const LadderView& v) { encode_ladder(w, v); }

bool r_ladder(ByteReader& r, LadderView& v) { return decode_ladder(r, v); }

}  // namespace

Status write_world_snapshot(const std::string& path, const WorldSnapshot& s) {
    ByteWriter w;
    w.put_u64(s.config_hash);
    w.put_u64(s.master_seed);
    w.put_u64(s.state_hash);
    w.put_u64(s.events);
    w.put_u64(s.tiebreak);
    w.put_i64(s.now);
    w.put_u64(s.md_seq);
    w.put_u64(s.expected_md_seq);
    w.put_u8(s.gap_pending);
    w.put_u8(s.halted);
    w.put_u64(s.next_book_seq);

    w.put_u64(s.queue.size());
    for (const auto& e : s.queue) {
        w.put_i64(e.t);
        w.put_i64(e.rank);
        w.put_i64(e.tiebreak);
        w.put_i64(e.type);
        w.put_i64(e.a);
        w.put_i64(e.b);
        w.put_i64(e.c);
        w.put_i64(e.p);
    }

    w_ladder(w, s.book);

    w.put_u64(s.order_accounts.size());
    for (const auto& [id, acct] : s.order_accounts) {
        w.put_u64(id);
        w.put_u32(acct);
    }

    // sim
    w.put_u64(s.sim.provider_rng);
    w.put_u64(s.sim.taker_rng);
    w.put_u64(s.sim.regime_rng);
    w.put_u64(s.sim.price_rng);
    w.put_i64(s.sim.next_provider_t);
    w.put_i64(s.sim.next_taker_t);
    w.put_i64(s.sim.regime);
    w.put_i64(s.sim.regime_until);
    w.put_u64(s.sim.actions);
    w.put_i64(s.sim.bias);
    w.put_i64(s.sim.last_trade_px);
    w.put_u64(s.sim.next_bot_id);
    w.put_i64(s.sim.last_bb);
    w.put_i64(s.sim.last_ba);
    w.put_i64(s.sim.now);
    w.put_u8(s.sim.taker_boost_active ? 1 : 0);  // v9: self-excitation memory
    w.put_u8(s.sim.last_taker_side);               // v9: sign-persistence memory
    w.put_u64(s.sim.live_provider_orders.size());
    for (u64 id : s.sim.live_provider_orders) w.put_u64(id);

    // mm
    w.put_i64(s.mm.inventory);
    w.put_i64(s.mm.cash);
    w.put_i64(s.mm.fair);
    w.put_u8(s.mm.have_md);
    w.put_u64(s.mm.next_order_id);
    w.put_u64(s.mm.inference_fills);
    w.put_u64(s.mm.real_fills);
    w.put_u64(s.mm.real_filled_qty);
    w.put_u64(s.mm.ambiguous_qty);
    w.put_u64(s.mm.quote_updates);
    w.put_u64(s.mm.confirm_seq_counter);
    w.put_u64(s.mm.own.size());
    for (const auto& o : s.mm.own) {
        w.put_u64(o.id);
        w.put_u8(o.side);
        w.put_i64(o.price);
        w.put_u64(o.leaves);
        w.put_u64(o.ahead);
        w.put_u8(o.confirmed);
        w.put_u64(o.confirm_seq);
    }
    w_ladder(w, s.mm.view);

    // mom
    w.put_i64(s.mom.inventory);
    w.put_i64(s.mom.cash);
    w.put_i64(s.mom.last_px);
    w.put_u64(s.mom.next_order_id);
    w.put_u64(s.mom.filled_qty);  // v9
    w.put_u64(s.mom.signs.size());
    for (i64 v : s.mom.signs) w.put_i64(v);

    // risk (per account; v6 isolation)
    w.put_u64(s.risk_accounts.size());
    for (const auto& ra : s.risk_accounts) {
        w.put_u32(ra.account);
        w.put_i64(ra.position);
        w.put_u64(ra.accepted);
        w.put_u64(ra.rejected);
        for (u64 v : ra.by_reason) w.put_u64(v);
        w.put_u64(ra.window.size());
        for (i64 v : ra.window) w.put_i64(v);
    }

    // gateway known ids (sorted for canonical form)
    w.put_u64(s.gateway_known_ids.size());
    for (u64 id : s.gateway_known_ids) w.put_u64(id);

    for (u64 v : s.channel_rng) w.put_u64(v);
    for (u32 v : s.channel_burst) w.put_u32(v);
    w.put_u64(s.corrupt_rng);

    auto w_evmap = [&](const std::vector<std::pair<u64, Event>>& m) {
        w.put_u64(m.size());
        for (const auto& [seq, ev] : m) {
            w.put_u64(seq);
            if (!w_event_field(w, ev)) return fail(Errc::Internal, seq);
        }
        return good();
    };
    if (Status st = w_evmap(s.md_buffer); !st) return st;
    if (Status st = w_evmap(s.pending_md); !st) return st;

    w.put_u64(s.inflight.size());
    for (const auto& m : s.inflight) {
        w.put_u8(m.is_cancel);
        w_order(w, m.ord);
        w.put_u64(m.frame.size());
        w.put_bytes(m.frame.data(), m.frame.size());
    }
    w.put_u64(s.notifies.size());
    for (const auto& n : s.notifies) {
        w.put_u8(n.kind);
        // fills share the SnapOrder-style explicit encoding
        w.put_u64(n.fill.taker_id);
        w.put_u64(n.fill.maker_id);
        w.put_u32(n.fill.maker_acct);
        w.put_u32(n.fill.taker_acct);
        w.put_i64(n.fill.price);
        w.put_u64(n.fill.qty);
        w.put_i64(n.fill.t);
        w.put_u8(n.fill.maker_liq);
        w.put_u8(n.fill.taker_side);
        w.put_u8(n.fill.maker_side);
        w.put_u64(n.frame.size());
        w.put_bytes(n.frame.data(), n.frame.size());
    }
    w.put_u64(s.markouts.size());
    for (const auto& m : s.markouts) {
        w.put_i64(m.fill_t);
        w.put_i64(m.fill_px);
        w.put_u8(m.side);
        w.put_u32(m.account);
    }
    w.put_u64(s.order_sent_t.size());
    for (const auto& [id, t] : s.order_sent_t) {
        w.put_u64(id);
        w.put_i64(t);
    }

    w.put_u64(s.stats.trades);
    w.put_u64(s.stats.volume);
    w.put_u64(s.stats.bot_fills);
    w.put_u64(s.stats.strategy_fills);
    w.put_u64(s.stats.momentum_fills);
    w.put_u64(s.stats.strategy_orders_sent);
    w.put_u64(s.stats.strategy_cancels_sent);
    w.put_u64(s.stats.order_frames_dropped);
    w.put_u64(s.stats.risk_accepted);
    w.put_u64(s.stats.risk_rejected);
    w.put_u64(s.stats.md_published);
    w.put_u64(s.stats.md_delivered);
    w.put_u64(s.stats.md_dropped);
    w.put_u64(s.stats.md_duplicated);
    w.put_u64(s.stats.md_reordered);
    w.put_u64(s.stats.md_gap_events);
    w.put_u64(s.stats.md_recovered);
    w.put_u64(s.stats.md_snapshot_recoveries);
    w.put_u64(s.stats.wire_frames_encoded);
    w.put_u64(s.stats.wire_frames_decoded);
    w.put_u64(s.stats.wire_frames_corrupted);

    // file framing
    ByteWriter head;
    head.put_bytes(kMagic, 4);
    head.put_u32(s.format_version);
    head.put_u32(static_cast<u32>(w.size()));
    head.put_u32(crc32(w.data().data(), w.size()));

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return fail(Errc::IoError, static_cast<u64>(errno));
    const std::string& h = head.data();
    const std::string& b = w.data();
    const bool ok = std::fwrite(h.data(), 1, h.size(), f) == h.size() &&
                    std::fwrite(b.data(), 1, b.size(), f) == b.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) return fail(Errc::IoError, 1);
    return good();
}

Result<WorldSnapshot> read_world_snapshot(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return ErrT<WorldSnapshot>(Errc::NotFound, static_cast<u64>(errno));
    std::string all;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    std::fclose(f);

    if (all.size() < kHeaderSize) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 0);
    const u8* p = reinterpret_cast<const u8*>(all.data());
    if (std::memcmp(p, kMagic, 4) != 0) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 0);
    ByteReader hr(p + 4, 12);
    const u32 version = hr.get_u32();
    const u32 body_len = hr.get_u32();
    const u32 crc = hr.get_u32();
    if (!hr.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 1);
    if (version != kWorldSnapshotVersion) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, version);
    if (all.size() != kHeaderSize + body_len) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 2);
    if (crc32(p + kHeaderSize, body_len) != crc)
        return ErrT<WorldSnapshot>(Errc::ChecksumMismatch, 0);

    ByteReader r(p + kHeaderSize, body_len);
    WorldSnapshot s;
    s.format_version = version;
    s.config_hash = r.get_u64();
    s.master_seed = r.get_u64();
    s.state_hash = r.get_u64();
    s.events = r.get_u64();
    s.tiebreak = r.get_u64();
    s.now = r.get_i64();
    s.md_seq = r.get_u64();
    s.expected_md_seq = r.get_u64();
    s.gap_pending = r.get_u8();
    s.halted = r.get_u8();
    s.next_book_seq = r.get_u64();
    if (!r.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 3);

    const u64 qn = r.get_u64();
    if (qn > (1u << 26)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 4);
    s.queue.reserve(static_cast<size_t>(qn));
    for (u64 i = 0; i < qn; ++i) {
        SnapSchedEvent e;
        e.t = r.get_i64();
        e.rank = r.get_i64();
        e.tiebreak = r.get_i64();
        e.type = r.get_i64();
        e.a = r.get_i64();
        e.b = r.get_i64();
        e.c = r.get_i64();
        e.p = r.get_i64();
        if (!r.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 5);
        s.queue.push_back(e);
    }

    if (!r_ladder(r, s.book)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 6);

    const u64 oa_n = r.get_u64();
    if (oa_n > (1u << 26)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 25);
    s.order_accounts.resize(static_cast<size_t>(oa_n));
    for (u64 i = 0; i < oa_n; ++i) {
        const u64 id = r.get_u64();
        const u32 acct = r.get_u32();
        if (!r.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 26);
        s.order_accounts[static_cast<size_t>(i)] = {id, acct};
    }

    s.sim.provider_rng = r.get_u64();
    s.sim.taker_rng = r.get_u64();
    s.sim.regime_rng = r.get_u64();
    s.sim.price_rng = r.get_u64();
    s.sim.next_provider_t = r.get_i64();
    s.sim.next_taker_t = r.get_i64();
    s.sim.regime = static_cast<int>(r.get_i64());
    s.sim.regime_until = r.get_i64();
    s.sim.actions = r.get_u64();
    s.sim.bias = r.get_i64();
    s.sim.last_trade_px = r.get_i64();
    s.sim.next_bot_id = r.get_u64();
    s.sim.last_bb = r.get_i64();
    s.sim.last_ba = r.get_i64();
    s.sim.now = r.get_i64();
    s.sim.taker_boost_active = r.get_u8() != 0;  // v9
    s.sim.last_taker_side = r.get_u8();           // v9
    const u64 live_n = r.get_u64();
    if (live_n > (1u << 26)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 7);
    s.sim.live_provider_orders.resize(static_cast<size_t>(live_n));
    for (u64 i = 0; i < live_n; ++i) s.sim.live_provider_orders[static_cast<size_t>(i)] = r.get_u64();
    if (!r.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 8);

    s.mm.inventory = r.get_i64();
    s.mm.cash = r.get_i64();
    s.mm.fair = r.get_i64();
    s.mm.have_md = r.get_u8();
    s.mm.next_order_id = r.get_u64();
    s.mm.inference_fills = r.get_u64();
    s.mm.real_fills = r.get_u64();
    s.mm.real_filled_qty = r.get_u64();
    s.mm.ambiguous_qty = r.get_u64();
    s.mm.quote_updates = r.get_u64();
    s.mm.confirm_seq_counter = r.get_u64();
    const u64 own_n = r.get_u64();
    if (own_n > (1u << 26)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 9);
    s.mm.own.resize(static_cast<size_t>(own_n));
    for (u64 i = 0; i < own_n; ++i) {
        SnapOwn& o = s.mm.own[static_cast<size_t>(i)];
        o.id = r.get_u64();
        o.side = r.get_u8();
        o.price = r.get_i64();
        o.leaves = r.get_u64();
        o.ahead = r.get_u64();
        o.confirmed = r.get_u8();
        o.confirm_seq = r.get_u64();
    }
    if (!r.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 10);
    if (!r_ladder(r, s.mm.view)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 11);

    s.mom.inventory = r.get_i64();
    s.mom.cash = r.get_i64();
    s.mom.last_px = r.get_i64();
    s.mom.next_order_id = r.get_u64();
    s.mom.filled_qty = r.get_u64();  // v9
    const u64 signs_n = r.get_u64();
    if (signs_n > (1u << 20)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 12);
    s.mom.signs.resize(static_cast<size_t>(signs_n));
    for (u64 i = 0; i < signs_n; ++i) s.mom.signs[static_cast<size_t>(i)] = r.get_i64();

    // risk (per account; v6 isolation)
    const u64 rk_n = r.get_u64();
    if (rk_n > (1u << 16)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 13);
    s.risk_accounts.resize(static_cast<size_t>(rk_n));
    for (u64 i = 0; i < rk_n; ++i) {
        SnapRiskAccount& ra = s.risk_accounts[static_cast<size_t>(i)];
        ra.account = r.get_u32();
        ra.position = r.get_i64();
        ra.accepted = r.get_u64();
        ra.rejected = r.get_u64();
        for (u64& v : ra.by_reason) v = r.get_u64();
        const u64 win_n = r.get_u64();
        if (!r.ok() || win_n > (1u << 24)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 13);
        ra.window.resize(static_cast<size_t>(win_n));
        for (u64 k = 0; k < win_n; ++k) ra.window[static_cast<size_t>(k)] = r.get_i64();
    }
    if (!r.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 14);

    const u64 ids_n = r.get_u64();
    if (ids_n > (1u << 26)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 15);
    s.gateway_known_ids.resize(static_cast<size_t>(ids_n));
    for (u64 i = 0; i < ids_n; ++i) s.gateway_known_ids[static_cast<size_t>(i)] = r.get_u64();
    for (u64& v : s.channel_rng) v = r.get_u64();
    for (u32& v : s.channel_burst) v = r.get_u32();
    s.corrupt_rng = r.get_u64();
    if (!r.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 16);

    auto r_evmap = [&](std::vector<std::pair<u64, Event>>& m) -> bool {
        const u64 count = r.get_u64();
        if (!r.ok() || count > (1u << 26)) return false;
        m.reserve(static_cast<size_t>(count));
        for (u64 i = 0; i < count; ++i) {
            const u64 seq = r.get_u64();
            Event e;
            if (!r_event_field(r, e)) return false;
            m.emplace_back(seq, e);
        }
        return true;
    };
    if (!r_evmap(s.md_buffer)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 17);
    if (!r_evmap(s.pending_md)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 18);

    const u64 infl_n = r.get_u64();
    if (infl_n > (1u << 26)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 19);
    s.inflight.resize(static_cast<size_t>(infl_n));
    for (u64 i = 0; i < infl_n; ++i) {
        SnapMsg& m = s.inflight[static_cast<size_t>(i)];
        m.is_cancel = r.get_u8();
        if (!r_order(r, m.ord)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 20);
        const u64 flen = r.get_u64();
        if (!r.ok() || flen > (1u << 20)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 27);
        m.frame.resize(static_cast<size_t>(flen));
        if (flen && !r.get_bytes(m.frame.data(), static_cast<size_t>(flen)))
            return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 27);
    }
    const u64 not_n = r.get_u64();
    if (not_n > (1u << 26)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 21);
    s.notifies.resize(static_cast<size_t>(not_n));
    for (u64 i = 0; i < not_n; ++i) {
        SnapNotify& nf = s.notifies[static_cast<size_t>(i)];
        nf.kind = r.get_u8();
        nf.fill.taker_id = r.get_u64();
        nf.fill.maker_id = r.get_u64();
        nf.fill.maker_acct = r.get_u32();
        nf.fill.taker_acct = r.get_u32();
        nf.fill.price = r.get_i64();
        nf.fill.qty = r.get_u64();
        nf.fill.t = r.get_i64();
        nf.fill.maker_liq = r.get_u8();
        nf.fill.taker_side = r.get_u8();
        nf.fill.maker_side = r.get_u8();
        const u64 flen = r.get_u64();
        if (!r.ok() || flen > (1u << 20)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 28);
        nf.frame.resize(static_cast<size_t>(flen));
        if (flen && !r.get_bytes(nf.frame.data(), static_cast<size_t>(flen)))
            return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 28);
        if (!r.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 22);
    }
    const u64 mk_n = r.get_u64();
    if (!r.ok() || mk_n > (1u << 24)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 29);
    s.markouts.resize(static_cast<size_t>(mk_n));
    for (u64 i = 0; i < mk_n; ++i) {
        SnapMarkout& m = s.markouts[static_cast<size_t>(i)];
        m.fill_t = r.get_i64();
        m.fill_px = r.get_i64();
        m.side = r.get_u8();
        m.account = r.get_u32();
    }
    if (!r.ok()) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 29);
    const u64 ost_n = r.get_u64();
    if (ost_n > (1u << 26)) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 23);
    s.order_sent_t.resize(static_cast<size_t>(ost_n));
    for (u64 i = 0; i < ost_n; ++i) {
        const u64 id = r.get_u64();
        const i64 t = r.get_i64();
        s.order_sent_t[static_cast<size_t>(i)] = {id, t};
    }

    s.stats.trades = r.get_u64();
    s.stats.volume = r.get_u64();
    s.stats.bot_fills = r.get_u64();
    s.stats.strategy_fills = r.get_u64();
    s.stats.momentum_fills = r.get_u64();
    s.stats.strategy_orders_sent = r.get_u64();
    s.stats.strategy_cancels_sent = r.get_u64();
    s.stats.order_frames_dropped = r.get_u64();
    s.stats.risk_accepted = r.get_u64();
    s.stats.risk_rejected = r.get_u64();
    s.stats.md_published = r.get_u64();
    s.stats.md_delivered = r.get_u64();
    s.stats.md_dropped = r.get_u64();
    s.stats.md_duplicated = r.get_u64();
    s.stats.md_reordered = r.get_u64();
    s.stats.md_gap_events = r.get_u64();
    s.stats.md_recovered = r.get_u64();
    s.stats.md_snapshot_recoveries = r.get_u64();
    s.stats.wire_frames_encoded = r.get_u64();
    s.stats.wire_frames_decoded = r.get_u64();
    s.stats.wire_frames_corrupted = r.get_u64();

    if (!r.ok() || r.remaining() != 0) return ErrT<WorldSnapshot>(Errc::SnapshotInvalid, 24);
    return OkT(std::move(s));
}

}  // namespace arbiter