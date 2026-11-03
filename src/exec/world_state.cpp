#include "arbiter/world.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/rng.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace arbiter {
// State-hash contract (tag ARBITERX/WORLD/3; see docs/REPRODUCIBILITY.md):
// the hash is a deterministic identity of the world's EVENT-RELEVANT state,
// everything that can influence future events. Covered: logical clock and
// progress counters, exchange book ladder + per-order account ownership,
// full strategy state (incl. local view ladder and queue-ahead), per-account
// risk state + working-order exposure reservations, exchange-authoritative
// economics ledger, sim dynamics + RNG-relevant progress, network channel RNG
// + burst states + corruption stream, feed-handler progress + pending buffer,
// retransmit-ring progress, MD packet attempts in transit, transport sequence
// counters + receiver dedupe state, gateway id set, in-flight order/notify
// frames, pending markout probes, scheduler contents. Deliberately EXCLUDED:
//   * journal bytes, RunStats histograms/timelines, wall-clock;
//   * order_sent_t_ / stages_ / open_fill_reports_: per-order
//     latency-tracing tables that feed ONLY RunStats histograms (never
//     control flow). stages_ is additionally NOT snapshot-persisted (cleared
//     on restore by design), so hashing it would break the restore contract;
//     order_sent_t_ and open_fill_reports_ stay snapshot-persisted for stats
//     continuity but are not part of state identity.
//   * cfg_hash_: configuration identity belongs to config_hash()/world_id().
//     Binding it into state_hash would make fork-divergence assertions
//     vacuous (any fork would "diverge" trivially via the config tag).
// This is NOT cryptographic uniqueness, it is deterministic state identity
// under this documented serialization (FNV-1a 64, fixed field order, sorted
// map/set iteration).
u64 World::state_hash() const {
    Hasher h;
    h.tag("ARBITERX/WORLD/3");
    h.u64v(static_cast<u64>(now_));
    h.u64v(events_);
    h.u64v(tiebreak_);
    h.boolean(started_);
    h.boolean(halted_);
    canonical_book_hash(book_.to_ladder(), h);
    // Order-account ownership routes fills/cancels: identity matters.
    {
        auto accts = book_.order_accounts();
        std::sort(accts.begin(), accts.end(),
                  [](const auto& x, const auto& y) { return x.first < y.first; });
        h.tag("ACCT");
        h.u64v(accts.size());
        for (const auto& [id, acct] : accts) {
            h.u64v(id);
            h.u32v(acct);
        }
    }
    h.tag("MM");
    mm_.hash_into(h);
    h.tag("MOM");
    mom_.hash_into(h);
    // Per-account risk state (aggregates alone allow cross-account collision)
    // plus the working-order exposure reservations (they gate future
    // risk checks, so they are behavior-relevant state).
    h.tag("RISK");
    {
        u64 n = 0;
        risk_.for_each_state([&](u32, const RiskState&) { ++n; });
        h.u64v(n);
        risk_.for_each_state([&](u32 acct, const RiskState& st) {
            h.u32v(acct);
            h.i64v(st.position);
            h.u64v(st.accepted);
            h.u64v(st.rejected);
            for (size_t i = 0; i <= kRiskDuplicateId; ++i) h.u64v(st.rejects_by_reason[i]);
            h.u64v(st.window.size());
            for (LogicalNs t : st.window) h.i64v(t);
        });
        const auto& reservations = risk_.reservations();
        h.u64v(reservations.size());
        for (const auto& [id, r] : reservations) {
            h.u64v(id);
            h.u32v(r.account);
            h.i64v(r.signed_qty);
        }
    }
    // Exchange-authoritative economics ledger (reported outcomes must be
    // identical across continuous and restored runs even though the ledger
    // does not steer control flow).
    h.tag("LEDGER");
    h.u64v(ledger_.size());
    for (const auto& [acct, e] : ledger_) {
        h.u32v(acct);
        h.i64v(e.position);
        h.i64v(e.cash);
        h.i64v(e.fees);
        h.u64v(e.filled_qty);
    }
    // Simulator: regime + progress + RNG-relevant dynamics.
    h.tag("SIM");
    {
        const MarketSim::SimState ss = sim_.state();
        h.u64v(ss.provider_rng);
        h.u64v(ss.taker_rng);
        h.u64v(ss.regime_rng);
        h.u64v(ss.price_rng);
        h.i64v(ss.next_provider_t);
        h.i64v(ss.next_taker_t);
        h.i64v(ss.regime);
        h.i64v(ss.regime_until);
        h.u64v(ss.actions);
        h.i64v(ss.bias);
        h.i64v(ss.last_trade_px);
        h.u64v(ss.next_bot_id);
        h.i64v(ss.last_bb);
        h.i64v(ss.last_ba);
        h.i64v(ss.now);
        h.boolean(ss.taker_boost_active);  // self-excitation memory: behavior-relevant
        h.u8v(ss.last_taker_side);          // sign-persistence memory
        h.u64v(ss.live_provider_orders.size());
        for (u64 id : ss.live_provider_orders) h.u64v(id);
    }
    // Network: channel RNG + burst states + corruption stream.
    h.tag("NET");
    h.u64v(md_ch_.state());
    h.u64v(md_ch_.burst_state());
    h.u64v(order_ch_.state());
    h.u64v(order_ch_.burst_state());
    h.u64v(exec_ch_.state());
    h.u64v(exec_ch_.burst_state());
    h.u64v(corrupt_rng_.state);
    h.u64v(md_seq_);
    h.u64v(expected_md_seq_);
    h.boolean(gap_request_pending_);
    h.u64v(md_packet_seq_);
    h.u64v(md_packets_.size());
    for (const auto& [pid, pkt] : md_packets_) {
        h.u64v(pid);
        h.u64v(pkt.md_seq);
        h.str(pkt.frame);
    }
    // Transport sequence counters + receiver-side dedupe state (a duplicated
    // delivery must dedupe identically after a restore).
    h.u64v(order_seq_);
    h.u64v(exec_seq_);
    h.u64v(order_rx_seqs_.size());
    for (u64 s : order_rx_seqs_) h.u64v(s);
    h.u64v(exec_rx_seqs_.size());
    for (u64 s : exec_rx_seqs_) h.u64v(s);
    // Feed-handler pending buffer (sorted map: deterministic order).
    h.u64v(pending_md_.size());
    for (const auto& [seq, ev] : pending_md_) {
        h.u64v(seq);
        h.u64v(event_hash(ev));
    }
    // Retransmit ring progress: the snapshot persists the logical
    // oldest->newest CONTENTS (not the physical head index, which is a
    // rotation artifact of the circular buffer, restore rebuilds head=0
    // with identical logical order and identical eviction semantics). Hash
    // the logical contents in dump order.
    h.tag("MDRING");
    {
        const std::vector<std::pair<u64, Event>> dump = md_ring_dump();
        h.u64v(dump.size());
        for (const auto& [seq, ev] : dump) {
            h.u64v(seq);
            h.u64v(event_hash(ev));
        }
    }
    // Gateway id set (sorted: unordered_set iteration is not deterministic).
    {
        std::vector<u64> ids(gateway_known_ids_.begin(), gateway_known_ids_.end());
        std::sort(ids.begin(), ids.end());
        h.tag("GW");
        h.u64v(ids.size());
        for (u64 id : ids) h.u64v(id);
    }
    // In-flight order frames (authoritative bytes hashed).
    h.tag("FLIGHT");
    h.u64v(inflight_.size());
    for (const auto& m : inflight_) {
        h.boolean(m.is_cancel);
        h.boolean(m.delivered);
        h.u64v(m.transport_seq);
        h.u64v(m.ord.id);
        h.u32v(m.ord.account);
        h.u8v(static_cast<u8>(m.ord.side));
        h.u8v(static_cast<u8>(m.ord.type));
        h.u8v(static_cast<u8>(m.ord.tif));
        h.i64v(m.ord.price);
        h.u64v(m.ord.qty);
        h.u64v(m.ord.leaves);
        h.str(m.frame);
    }
    // In-flight execution notifications.
    h.tag("NOTIFY");
    h.u64v(notifies_.size());
    for (const auto& n : notifies_) {
        h.u8v(n.kind);
        h.boolean(n.delivered);
        h.u64v(n.transport_seq);
        h.u64v(n.fill.taker_order_id);
        h.u64v(n.fill.maker_order_id);
        h.u32v(n.fill.maker_account);
        h.u32v(n.fill.taker_account);
        h.i64v(n.fill.price);
        h.u64v(n.fill.qty);
        h.i64v(n.fill.t);
        h.u8v(static_cast<u8>(n.fill.maker_liq));
        h.u8v(static_cast<u8>(n.fill.taker_side));
        h.u8v(static_cast<u8>(n.fill.maker_side));
        h.str(n.frame);
    }
    // Pending markout probes (execution-research state).
    h.tag("MARKOUT");
    h.u64v(markouts_.size());
    for (const auto& m : markouts_) {
        h.i64v(m.fill_t);
        h.i64v(m.fill_px);
        h.u8v(static_cast<u8>(m.side));
    }
    // Decision-time and stage-trace tables are INSTRUMENTATION (they feed
    // only RunStats histograms, never control flow) and stages_ is not even
    // snapshot-persisted, both are intentionally outside state identity.
    // Scheduler contents in pop order (copy-then-drain: deterministic).
    h.tag("SCHED");
    {
        auto q = queue_;
        h.u64v(q.size());
        while (!q.empty()) {
            const WorldEvent e = q.top();
            q.pop();
            h.i64v(e.t);
            h.u8v(e.rank);
            h.u64v(e.tiebreak);
            h.u16v(static_cast<u16>(e.type));
            h.u64v(e.a);
            h.u64v(e.b);
            h.u64v(e.c);
            h.i64v(e.p);
        }
    }
    return h.value();
}

// ---------------- full-world checkpointing ----------------

namespace {
// Read-only access to a priority_queue's underlying container (standard
// protected-member access via a derived view; no mutation).
template <typename T, typename C>
const std::vector<T>& pq_underlying(const std::priority_queue<T, std::vector<T>, C>& q) {
    struct Accessor : std::priority_queue<T, std::vector<T>, C> {
        static const std::vector<T>& get(const std::priority_queue<T, std::vector<T>, C>& pq) {
            return pq.*(&Accessor::c);
        }
    };
    return Accessor::get(q);
}

// Semantic validation of a checksummed snapshot, run BEFORE any restore
// mutation: a validly encoded but semantically impossible state is rejected
// cleanly with a deterministic context code instead of being cast blindly
// into the live world. The context codes are stable diagnostics:
//   40 book ladder / order domain   41 order-account ownership
//   42 next_book_seq                43 MD packet attempts
//   44 receiver dedupe state        45 pending MD
//   46 in-flight order frames       47 exec notifications
//   48 scheduler                    49 risk accounts
//   50 reservations                 51 ledger
//   52 MM strategy state            53 momentum strategy state
//   54 sim state
// In-flight wire frames that fail to DECODE are legitimate (in-transit
// corruption is real state); a frame that DOES decode must agree with its
// stored channel/sequence/flags metadata.
Status validate_snapshot_semantics(const WorldSnapshot& s, const WorldConfig& cfg) {
    auto bad = [](u64 ctx) { return fail(Errc::SnapshotInvalid, ctx); };

    // scalar stream sanity
    if (s.now < 0 || s.expected_md_seq > s.md_seq || s.gap_pending > 1 || s.halted > 1 ||
        s.started > 1)
        return bad(45);
    if (!s.started && (s.events != 0 || s.tiebreak != 0 || s.now != 0 || !s.queue.empty()))
        return bad(45);
    // the persisted configuration must hash to the recorded config hash:
    // a tampered config block is exactly the "validly encoded but wrong"
    // state this validator exists to catch (v13 snapshots)
    if (!validate_world_config(s.config).empty() || !validate_world_config(cfg).empty() ||
        s.config.hash() != s.config_hash)
        return bad(38);

    // ---- book ladder: structure, per-order domain, span, ids ----
    if (const Status inv = check_book_invariants(s.book); !inv) return bad(40);
    std::vector<std::pair<u64, u32>> resting;  // (id, account) from ownership
    {
        std::vector<u64> ids;
        auto check_side = [&](const std::vector<LadderLevel>& lv) -> Status {
            for (const auto& lvl : lv) {
                if (lvl.price < cfg.book_min_price || lvl.price > cfg.book_max_price)
                    return bad(40);
                for (const auto& o : lvl.orders) {
                    if (o.id == 0 || o.id > DomainBounds::kMaxOrderId) return bad(40);
                    if (o.qty == 0 || o.leaves == 0 || o.leaves > o.qty) return bad(40);
                    ids.push_back(o.id);
                }
            }
            return good();
        };
        if (const Status st = check_side(s.book.bids); !st) return st;
        if (const Status st = check_side(s.book.asks); !st) return st;
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return bad(40);
        // ownership: real resting orders only, no duplicates, account >= 1
        for (const auto& [id, acct] : s.order_accounts) {
            if (id == 0 || acct == 0) return bad(41);
            if (!std::binary_search(ids.begin(), ids.end(), id)) return bad(41);
            resting.emplace_back(id, acct);
        }
        std::sort(resting.begin(), resting.end());
        if (std::adjacent_find(resting.begin(), resting.end(),
                               [](const auto& a, const auto& b) { return a.first == b.first; }) !=
            resting.end())
            return bad(41);
        if (resting.size() != ids.size()) return bad(41);
    }
    if (s.next_book_seq == 0) return bad(42);

    // ---- MD packet attempts: unique ids below the counter ----
    {
        std::vector<u64> pids;
        pids.reserve(s.md_packets.size());
        for (const auto& p : s.md_packets) {
            if (p.packet_id >= s.md_packet_seq) return bad(43);
            pids.push_back(p.packet_id);
            // When the (possibly corrupted) frame still decodes, its channel
            // and embedded sequence must agree with the packet metadata.
            auto d = wire_decode(reinterpret_cast<const u8*>(p.frame.data()), p.frame.size());
            if (d && (d.value.channel != 1 || d.value.md_seq != p.md_seq ||
                      (d.value.flags & (kFlagSnapshot | kFlagExecReport)) != 0))
                return bad(43);
        }
        std::sort(pids.begin(), pids.end());
        if (std::adjacent_find(pids.begin(), pids.end()) != pids.end()) return bad(43);
    }

    // ---- receiver dedupe state: unique, in-range sequences ----
    {
        auto seqs_ok = [&](const std::vector<u64>& v, u64 counter) {
            if (!std::is_sorted(v.begin(), v.end())) return false;  // stored sorted
            if (std::adjacent_find(v.begin(), v.end()) != v.end()) return false;
            return v.empty() || v.back() <= counter;
        };
        if (!seqs_ok(s.order_rx_seqs, s.order_seq)) return bad(44);
        if (!seqs_ok(s.exec_rx_seqs, s.exec_seq)) return bad(44);
    }

    // ---- pending MD: unique keys inside [expected, md_seq) ----
    {
        std::vector<u64> keys;
        keys.reserve(s.pending_md.size());
        for (const auto& [seq, ev] : s.pending_md) {
            if (seq < s.expected_md_seq || seq >= s.md_seq) return bad(45);
            keys.push_back(seq);
        }
        std::sort(keys.begin(), keys.end());
        if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) return bad(45);
    }

    // ---- in-flight order frames: metadata agreement when decodable ----
    // NOTE: order-id uniqueness is NOT required here — a cancel frame for a
    // still-in-flight send legitimately shares its id (channel reordering).
    for (const auto& m : s.inflight) {
        if (m.transport_seq == 0 || m.transport_seq > s.order_seq) return bad(46);
        if (m.ord.id == 0 || m.ord.id > DomainBounds::kMaxOrderId) return bad(46);
        if (m.is_cancel > 1 || m.delivered > 1) return bad(46);
        auto d = wire_decode(reinterpret_cast<const u8*>(m.frame.data()), m.frame.size());
        if (d && (d.value.channel != 2 || d.value.md_seq != m.transport_seq ||
                  (d.value.flags & (kFlagSnapshot | kFlagExecReport)) != 0))
            return bad(46);
    }

    // ---- exec notifications: kind-aware fill domain + frame agreement ----
    // NOTE: taker-id uniqueness is NOT required — partial fills of one order
    // produce multiple kind-0 reports, all legitimate.
    for (const auto& n : s.notifies) {
        if (n.kind > 3) return bad(47);
        if (n.transport_seq == 0 || n.transport_seq > s.exec_seq) return bad(47);
        if (n.delivered > 1) return bad(47);
        const SnapFill& f = n.fill;
        if (f.taker_id == 0 || f.taker_acct == 0 || f.t < 0 || f.t > s.now) return bad(47);
        if (f.taker_side > 1 || f.maker_side > 1 || f.maker_liq > 1) return bad(47);
        if (n.kind == 0) {
            if (f.maker_id == 0 || f.maker_acct == 0 || f.price <= 0 || f.qty == 0)
                return bad(47);
        } else if (n.kind == 1) {
            // the reason slot carries a RiskReason (0..7) or, for a
            // book-admission reject, the Errc code that add_order returned
            const u64 reason = f.maker_id;
            const bool risk_reason = reason <= 7;
            const bool book_errc =
                reason == static_cast<u64>(Errc::InvalidArgument) ||
                reason == static_cast<u64>(Errc::OutOfRange) ||
                reason == static_cast<u64>(Errc::Duplicate) ||
                reason == static_cast<u64>(Errc::InsufficientLiquidity) ||
                reason == static_cast<u64>(Errc::Capacity);
            if (!risk_reason && !book_errc) return bad(47);
        } else if (n.kind == 2) {
            if (f.qty == 0 || f.price != 0) return bad(47);
        } else {  // kind 3: cancel confirmation
            if (f.qty != 0 || f.price != 0) return bad(47);
        }
        auto d = wire_decode(reinterpret_cast<const u8*>(n.frame.data()), n.frame.size());
        if (d && (d.value.channel != 3 || d.value.md_seq != n.transport_seq ||
                  (d.value.flags & kFlagExecReport) == 0))
            return bad(47);
    }

    // ---- scheduler: known types/ranks, sane times, valid references ----
    {
        std::vector<u64> tbs;
        tbs.reserve(s.queue.size());
        for (const auto& e : s.queue) {
            if (e.t < s.now) return bad(48);
            if (e.rank < 0 || e.rank > 7) return bad(48);
            if (e.type < 20 || e.type > 28) return bad(48);
            if (e.tiebreak < 0 || static_cast<u64>(e.tiebreak) >= s.tiebreak) return bad(48);
            tbs.push_back(static_cast<u64>(e.tiebreak));
            switch (e.type) {
                case 21:  // MD delivery references an allocated packet id
                    if (e.b < 0 || static_cast<u64>(e.b) >= s.md_packet_seq) return bad(48);
                    break;
                case 23:  // gateway arrival / match due reference in-flight msgs
                case 24:
                    if (e.b < 0 || static_cast<u64>(e.b) >= s.inflight.size()) return bad(48);
                    break;
                case 25:  // exec notify references an in-flight notification
                    if (e.b < 0 || static_cast<u64>(e.b) >= s.notifies.size()) return bad(48);
                    break;
                case 26:  // GTD expiry carries an order id
                    if (e.a <= 0) return bad(48);
                    break;
                case 28:  // markout probe references a recorded probe
                    if (e.b < 0 || static_cast<u64>(e.b) >= s.markouts.size()) return bad(48);
                    break;
                default:
                    break;
            }
        }
        std::sort(tbs.begin(), tbs.end());
        if (std::adjacent_find(tbs.begin(), tbs.end()) != tbs.end()) return bad(48);
    }

    // ---- risk accounts: unique, ordered rate window ----
    {
        std::vector<u32> accts;
        for (const auto& ra : s.risk_accounts) {
            if (ra.account == 0) return bad(49);
            accts.push_back(ra.account);
            i64 prev = -1;
            for (i64 t : ra.window) {
                if (t < 0 || t > s.now || t < prev) return bad(49);
                prev = t;
            }
        }
        if (std::adjacent_find(accts.begin(), accts.end()) != accts.end()) return bad(49);
    }

    // ---- reservations: unique ids, valid sign/qty, owner consistency ----
    {
        std::vector<u64> ids;
        for (const auto& r : s.reservations) {
            if (r.order_id == 0 || r.order_id > DomainBounds::kMaxOrderId) return bad(50);
            if (r.account == 0) return bad(50);
            if (r.signed_qty == 0 ||
                r.signed_qty > static_cast<i64>(DomainBounds::kMaxQty) ||
                r.signed_qty < -static_cast<i64>(DomainBounds::kMaxQty))
                return bad(50);
            // A reservation for an order that is RESTING must agree with the
            // book's owner for that order (in-flight orders cannot be
            // cross-checked here).
            const auto it = std::lower_bound(
                resting.begin(), resting.end(), r.order_id,
                [](const auto& a, u64 id) { return a.first < id; });
            if (it != resting.end() && it->first == r.order_id && it->second != r.account)
                return bad(50);
            ids.push_back(r.order_id);
        }
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return bad(50);
    }

    // ---- ledger: strategy accounts only, unique, non-negative counters ----
    {
        std::vector<u32> accts;
        for (const auto& l : s.ledger) {
            if (l.account != cfg.mm.account && l.account != cfg.momentum.account)
                return bad(51);
            if (l.fees < 0) return bad(51);
            accts.push_back(l.account);
        }
        std::sort(accts.begin(), accts.end());
        if (std::adjacent_find(accts.begin(), accts.end()) != accts.end()) return bad(51);
    }

    // ---- market-maker strategy state ----
    {
        std::vector<u64> ids;
        for (const auto& o : s.mm.own) {
            if (o.id == 0) return bad(52);
            if (o.side > 1) return bad(52);
            if (o.price <= 0 || o.leaves == 0) return bad(52);
            ids.push_back(o.id);
        }
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return bad(52);
        if (s.mm.have_md > 1 || s.mm.fair < 0 || s.mm.next_order_id == 0) return bad(52);
        if (const Status inv = check_book_invariants(s.mm.view); !inv) return bad(52);
        auto check_view = [&](const std::vector<LadderLevel>& lv) -> Status {
            for (const auto& lvl : lv)
                if (lvl.price <= 0) return bad(52);
            return good();
        };
        if (const Status st = check_view(s.mm.view.bids); !st) return st;
        if (const Status st = check_view(s.mm.view.asks); !st) return st;
    }

    // ---- momentum strategy state ----
    {
        if (s.mom.last_px < 0 || s.mom.next_order_id == 0) return bad(53);
        for (i64 sign : s.mom.signs)
            if (sign < -1 || sign > 1) return bad(53);
    }

    // ---- sim state ----
    {
        if (s.sim.next_provider_t < 0 || s.sim.next_taker_t < 0) return bad(54);
        if (s.sim.regime != 0 && s.sim.regime != 1) return bad(54);
        if (s.sim.last_taker_side > 2) return bad(54);
        if (s.sim.last_trade_px < 0 || s.sim.last_bb < 0 || s.sim.last_ba < 0) return bad(54);
        std::vector<u64> ids = s.sim.live_provider_orders;
        for (u64 id : ids)
            if (id == 0) return bad(54);
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return bad(54);
    }

    // ---- decision-time table + open fill reports ----
    {
        std::vector<u64> ids;
        for (const auto& [id, t] : s.order_sent_t) {
            if (id == 0 || t < 0 || t > s.now) return bad(48);
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return bad(48);
        ids.clear();
        for (const auto& [id, n] : s.open_fill_reports) {
            if (id == 0 || n == 0) return bad(48);
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return bad(48);
        for (const auto& m : s.markouts) {
            if (m.side > 1 || m.account == 0 || m.fill_t < 0 || m.fill_t > s.now || m.fill_px <= 0)
                return bad(48);
        }
        for (u64 id : s.gateway_known_ids)
            if (id == 0) return bad(48);
    }
    return good();
}
}  // namespace

WorldSnapshot World::capture_snapshot() const {
    WorldSnapshot s;
    s.config_hash = cfg_hash_;
    s.master_seed = master_seed_;
    s.state_hash = state_hash();
    s.config = cfg_;  // v13: the exact generating configuration
    s.events = events_;
    s.tiebreak = tiebreak_;
    s.now = now_;
    s.md_seq = md_seq_;
    s.expected_md_seq = expected_md_seq_;
    s.gap_pending = gap_request_pending_;
    s.halted = halted_;
    s.started = started_;
    s.next_book_seq = book_.next_book_seq();

    for (const auto& ev : pq_underlying(queue_))
        s.queue.push_back(SnapSchedEvent{ev.t, ev.rank, static_cast<i64>(ev.tiebreak),
                                         static_cast<i64>(ev.type), static_cast<i64>(ev.a),
                                         static_cast<i64>(ev.b), static_cast<i64>(ev.c), ev.p});

    s.book = book_.to_ladder();
    s.order_accounts = book_.order_accounts();
    s.sim = sim_.state();

    s.mm.inventory = mm_.inventory();
    s.mm.cash = mm_.cash();
    s.mm.fair = mm_.fair();
    s.mm.have_md = mm_.has_market_data();
    s.mm.next_order_id = mm_.next_order_id();
    s.mm.inference_fills = mm_.inference_fills();
    s.mm.real_fills = mm_.real_fills();
    s.mm.real_filled_qty = mm_.real_filled_qty();
    s.mm.ambiguous_qty = mm_.ambiguous_fill_qty();
    s.mm.quote_updates = mm_.quote_updates();
    s.mm.confirm_seq_counter = mm_.confirm_seq_counter();
    for (const auto& o : mm_.own_snapshot())
        s.mm.own.push_back(SnapOwn{o.id, static_cast<u8>(o.side), o.price, o.leaves, o.ahead,
                                   static_cast<u8>(o.confirmed ? 1 : 0), o.confirm_seq});
    s.mm.view = mm_.view_ladder();

    s.mom.inventory = mom_.inventory();
    s.mom.cash = mom_.cash();
    s.mom.last_px = mom_.last_px();
    s.mom.next_order_id = mom_.next_order_id();
    s.mom.filled_qty = mom_.filled_qty();
    s.mom.signs = mom_.signs_window();

    s.risk_accounts.clear();
    risk_.for_each_state([&](u32 account, const RiskState& rs) {
        SnapRiskAccount ra;
        ra.account = account;
        ra.position = rs.position;
        ra.accepted = rs.accepted;
        ra.rejected = rs.rejected;
        for (size_t i = 0; i < ra.by_reason.size() && i < 8; ++i)
            ra.by_reason[i] = rs.rejects_by_reason[i];
        ra.window = rs.window;
        s.risk_accounts.push_back(ra);
    });

    s.gateway_known_ids.assign(gateway_known_ids_.begin(), gateway_known_ids_.end());
    std::sort(s.gateway_known_ids.begin(), s.gateway_known_ids.end());
    s.channel_rng = {md_ch_.state(), order_ch_.state(), exec_ch_.state()};
    s.channel_burst = {md_ch_.burst_state(), order_ch_.burst_state(), exec_ch_.burst_state()};
    s.corrupt_rng = corrupt_rng_.state;

    s.md_buffer = md_ring_dump();
    s.md_packets.reserve(md_packets_.size());
    for (const auto& [pid, pkt] : md_packets_)
        s.md_packets.push_back(SnapMdPacket{pid, pkt.md_seq, pkt.frame});
    s.md_packet_seq = md_packet_seq_;
    s.pending_md.assign(pending_md_.begin(), pending_md_.end());
    s.order_seq = order_seq_;
    s.exec_seq = exec_seq_;
    s.order_rx_seqs.assign(order_rx_seqs_.begin(), order_rx_seqs_.end());
    s.exec_rx_seqs.assign(exec_rx_seqs_.begin(), exec_rx_seqs_.end());
    for (const auto& [id, r] : risk_.reservations())
        s.reservations.push_back(SnapReservation{id, r.account, r.signed_qty});
    for (const auto& [acct, e] : ledger_)
        s.ledger.push_back(SnapLedger{acct, e.position, e.cash, e.fees, e.filled_qty});
    for (const auto& [id, n] : open_fill_reports_) s.open_fill_reports.emplace_back(id, n);
    s.inflight.reserve(inflight_.size());
    for (const auto& m : inflight_) {
        SnapMsg sm;
        sm.is_cancel = m.is_cancel;
        sm.delivered = m.delivered;
        sm.transport_seq = m.transport_seq;
        const Order& o = m.ord;
        sm.ord = SnapOrder{o.id, o.book_seq, o.account, static_cast<u8>(o.side),
                           static_cast<u8>(o.type), static_cast<u8>(o.tif), o.price, o.qty,
                           o.leaves, o.arrival, o.expiry};
        sm.frame = m.frame;
        s.inflight.push_back(sm);
    }
    s.notifies.reserve(notifies_.size());
    for (const auto& n : notifies_) {
        SnapNotify sn;
        sn.kind = n.kind;
        sn.delivered = n.delivered;
        sn.transport_seq = n.transport_seq;
        const Fill& f = n.fill;
        sn.fill = SnapFill{f.taker_order_id, f.maker_order_id, f.maker_account, f.taker_account,
                           f.price, f.qty, f.t, static_cast<u8>(f.maker_liq),
                           static_cast<u8>(f.taker_side), static_cast<u8>(f.maker_side)};
        sn.frame = n.frame;
        s.notifies.push_back(sn);
    }
    s.markouts.reserve(markouts_.size());
    for (const auto& m : markouts_) {
        SnapMarkout sm;
        sm.fill_t = m.fill_t;
        sm.fill_px = m.fill_px;
        sm.side = static_cast<u8>(m.side);
        sm.account = m.account;
        s.markouts.push_back(sm);
    }
    s.order_sent_t.reserve(order_sent_t_.size());
    for (const auto& [id, t] : order_sent_t_) s.order_sent_t.emplace_back(id, t);
    std::sort(s.order_sent_t.begin(), s.order_sent_t.end());

    s.stats.trades = stats_.trades;
    s.stats.volume = stats_.volume;
    s.stats.bot_fills = stats_.bot_fills;
    s.stats.strategy_fills = stats_.strategy_fills;
    s.stats.momentum_fills = stats_.momentum_fills;
    s.stats.strategy_orders_sent = stats_.strategy_orders_sent;
    s.stats.strategy_cancels_sent = stats_.strategy_cancels_sent;
    s.stats.order_frames_dropped = stats_.order_frames_dropped;
    s.stats.order_frames_duplicated = stats_.order_frames_duplicated;
    s.stats.exec_frames_dropped = stats_.exec_frames_dropped;
    s.stats.exec_frames_duplicated = stats_.exec_frames_duplicated;
    s.stats.risk_accepted = stats_.risk_accepted;
    s.stats.risk_rejected = stats_.risk_rejected;
    s.stats.md_published = stats_.md_published;
    s.stats.md_delivered = stats_.md_delivered;
    s.stats.md_dropped = stats_.md_dropped;
    s.stats.md_duplicated = stats_.md_duplicated;
    s.stats.md_reordered = stats_.md_reordered;
    s.stats.md_gap_events = stats_.md_gap_events;
    s.stats.md_recovered = stats_.md_recovered;
    s.stats.md_snapshot_recoveries = stats_.md_snapshot_recoveries;
    s.stats.wire_frames_encoded = stats_.wire_frames_encoded;
    s.stats.wire_frames_decoded = stats_.wire_frames_decoded;
    s.stats.wire_frames_corrupted = stats_.wire_frames_corrupted;
    return s;
}

Status World::restore_snapshot(const WorldSnapshot& s, const WorldConfig& continuation_cfg,
                               bool strict_config) {
    if (s.format_version != kWorldSnapshotVersion) return fail(Errc::Unsupported, s.format_version);
    if (strict_config && s.config_hash != continuation_cfg.hash())
        return fail(Errc::SnapshotInvalid, 1);
    if (cfg_hash_ != continuation_cfg.hash()) return fail(Errc::SnapshotInvalid, 3);
    if (s.master_seed != master_seed_) return fail(Errc::SnapshotInvalid, 2);
    // Reject semantically impossible state before rebuilding. Later rebuild or
    // final hash verification can still fail after mutation: restore into a
    // fresh World and discard it on any failure.
    if (const Status st = validate_snapshot_semantics(s, continuation_cfg); !st) return st;

    now_ = s.now;
    events_ = s.events;
    tiebreak_ = s.tiebreak;
    md_seq_ = s.md_seq;
    expected_md_seq_ = s.expected_md_seq;
    gap_request_pending_ = s.gap_pending != 0;
    halted_ = s.halted != 0;
    md_packet_seq_ = s.md_packet_seq;
    order_seq_ = s.order_seq;
    exec_seq_ = s.exec_seq;

    while (!queue_.empty()) queue_.pop();
    for (const auto& e : s.queue)
        queue_.push(WorldEvent{e.t, static_cast<u8>(e.rank), static_cast<u64>(e.tiebreak),
                               static_cast<EvType>(e.type), static_cast<u64>(e.a),
                               static_cast<u64>(e.b), static_cast<u64>(e.c), e.p});

    // book: rebuild into a LOCAL OptBook first — a failed rest (corrupt
    // snapshot ladder) or an allocation failure aborts the restore loudly
    // with the live book untouched; commit the rebuild only once complete.
    {
        OptBook rebuilt{OptBook::Options{cfg_.book_min_price, cfg_.book_max_price, 1u << 14}};
        for (const auto& lvl : s.book.bids)
            for (const auto& o : lvl.orders)
                if (Status st = rebuilt.rest_order(Order{o.id, 0, Side::Buy, OrderType::Limit,
                                                        Tif::GTC, lvl.price, o.qty, o.leaves, 0,
                                                        0, 0});
                    !st)
                    return st;
        for (const auto& lvl : s.book.asks)
            for (const auto& o : lvl.orders)
                if (Status st = rebuilt.rest_order(Order{o.id, 0, Side::Sell, OrderType::Limit,
                                                        Tif::GTC, lvl.price, o.qty, o.leaves, 0,
                                                        0, 0});
                    !st)
                    return st;
        // rest_order assigns temporary reconstruction sequence numbers; set
        // the captured next value only after the ladder has been rebuilt.
        rebuilt.set_next_book_seq(s.next_book_seq);
        // ownership: restore each resting order's account (notification routing)
        for (const auto& [id, acct] : s.order_accounts) rebuilt.set_order_account(id, acct);
        book_ = std::move(rebuilt);
    }

    sim_.restore(s.sim);

    mm_.restore_full(s.mm.inventory, s.mm.cash, s.mm.fair, s.mm.have_md != 0, s.mm.next_order_id,
                     s.mm.inference_fills, s.mm.real_fills, s.mm.real_filled_qty,
                     s.mm.ambiguous_qty, s.mm.quote_updates, s.mm.confirm_seq_counter, {},
                     s.mm.view);
    // own orders include confirmation state; re-add via the extended snapshots
    for (const auto& o : s.mm.own) {
        MarketMakerStrategy::OwnSnapshot os{o.id, static_cast<Side>(o.side), o.price, o.leaves,
                                            o.ahead, o.confirmed != 0, o.confirm_seq};
        mm_.add_restored_own(os);
    }

    mom_.restore_full(s.mom.inventory, s.mom.cash, s.mom.last_px, s.mom.next_order_id,
                      s.mom.filled_qty, s.mom.signs);

    // risk: restore every account's isolated state (v6). Clear first so a
    // restore into an already-used engine cannot leak prior account state.
    {
        risk_.clear_states();
        for (const auto& ra : s.risk_accounts) {
            RiskState rs;
            rs.position = ra.position;
            rs.accepted = ra.accepted;
            rs.rejected = ra.rejected;
            for (size_t i = 0; i < 8; ++i) rs.rejects_by_reason[i] = ra.by_reason[i];
            rs.window = ra.window;
            risk_.restore_state(ra.account, rs);
        }
    }

    gateway_known_ids_.clear();
    gateway_known_ids_.insert(s.gateway_known_ids.begin(), s.gateway_known_ids.end());
    md_ch_.set_state(s.channel_rng[0]);
    order_ch_.set_state(s.channel_rng[1]);
    exec_ch_.set_state(s.channel_rng[2]);
    md_ch_.set_burst_state(s.channel_burst[0]);
    order_ch_.set_burst_state(s.channel_burst[1]);
    exec_ch_.set_burst_state(s.channel_burst[2]);
    corrupt_rng_.state = s.corrupt_rng;

    md_ring_ = {};
    md_head_ = 0;
    md_count_ = 0;
    for (const auto& [seq, ev] : s.md_buffer) md_ring_push(seq, ev);
    md_packets_.clear();
    for (const auto& p : s.md_packets) md_packets_.emplace(p.packet_id, MdPacket{p.md_seq, p.frame});
    pending_md_.clear();
    for (const auto& [seq, ev] : s.pending_md) pending_md_.emplace(seq, ev);
    order_rx_seqs_.clear();
    order_rx_seqs_.insert(s.order_rx_seqs.begin(), s.order_rx_seqs.end());
    exec_rx_seqs_.clear();
    exec_rx_seqs_.insert(s.exec_rx_seqs.begin(), s.exec_rx_seqs.end());
    risk_.clear_reservations();
    for (const auto& r : s.reservations)
        risk_.restore_reservation(r.order_id, RiskReservation{r.account, r.signed_qty});
    ledger_.clear();
    for (const auto& l : s.ledger)
        ledger_[l.account] = LedgerEntry{l.position, l.cash, l.fees, l.filled_qty};

    inflight_.clear();
    for (const auto& m : s.inflight) {
        Msg mm2;
        mm2.is_cancel = m.is_cancel != 0;
        mm2.delivered = m.delivered != 0;
        mm2.transport_seq = m.transport_seq;
        const SnapOrder& so = m.ord;
        mm2.ord = Order{so.id, so.account, static_cast<Side>(so.side),
                        static_cast<OrderType>(so.otype), static_cast<Tif>(so.tif), so.price,
                        so.qty, so.leaves, so.arrival, so.book_seq, so.expiry};
        mm2.frame = m.frame;
        inflight_.push_back(std::move(mm2));
    }
    notifies_.clear();
    for (const auto& n : s.notifies) {
        Notify nn;
        nn.kind = n.kind;
        nn.delivered = n.delivered != 0;
        nn.transport_seq = n.transport_seq;
        const SnapFill& sf = n.fill;
        nn.fill = Fill{sf.taker_id, sf.maker_id, sf.maker_acct, sf.taker_acct, sf.price, sf.qty,
                       sf.t, static_cast<Liquidity>(sf.maker_liq), static_cast<Side>(sf.taker_side),
                       static_cast<Side>(sf.maker_side)};
        nn.frame = n.frame;
        notifies_.push_back(std::move(nn));
    }
    markouts_.clear();
    markouts_.reserve(s.markouts.size());
    for (const auto& m : s.markouts) {
        MarkoutProbe mp;
        mp.fill_t = m.fill_t;
        mp.fill_px = m.fill_px;
        mp.side = static_cast<Side>(m.side);
        mp.account = m.account;
        markouts_.push_back(mp);
    }
    order_sent_t_.clear();
    for (const auto& [id, t] : s.order_sent_t) order_sent_t_[id] = t;
    open_fill_reports_.clear();
    for (const auto& [id, n] : s.open_fill_reports) open_fill_reports_[id] = n;
    stages_.clear();  // in-flight stage traces die with their orders

    stats_.trades = s.stats.trades;
    stats_.volume = s.stats.volume;
    stats_.bot_fills = s.stats.bot_fills;
    stats_.strategy_fills = s.stats.strategy_fills;
    stats_.momentum_fills = s.stats.momentum_fills;
    stats_.strategy_orders_sent = s.stats.strategy_orders_sent;
    stats_.strategy_cancels_sent = s.stats.strategy_cancels_sent;
    stats_.order_frames_dropped = s.stats.order_frames_dropped;
    stats_.order_frames_duplicated = s.stats.order_frames_duplicated;
    stats_.exec_frames_dropped = s.stats.exec_frames_dropped;
    stats_.exec_frames_duplicated = s.stats.exec_frames_duplicated;
    stats_.risk_accepted = s.stats.risk_accepted;
    stats_.risk_rejected = s.stats.risk_rejected;
    stats_.md_published = s.stats.md_published;
    stats_.md_delivered = s.stats.md_delivered;
    stats_.md_dropped = s.stats.md_dropped;
    stats_.md_duplicated = s.stats.md_duplicated;
    stats_.md_reordered = s.stats.md_reordered;
    stats_.md_gap_events = s.stats.md_gap_events;
    stats_.md_recovered = s.stats.md_recovered;
    stats_.md_snapshot_recoveries = s.stats.md_snapshot_recoveries;
    stats_.wire_frames_encoded = s.stats.wire_frames_encoded;
    stats_.wire_frames_decoded = s.stats.wire_frames_decoded;
    stats_.wire_frames_corrupted = s.stats.wire_frames_corrupted;

    started_ = s.started != 0;

    // self-validation: a complete snapshot must reproduce the captured hash
    if (const u64 h = state_hash(); h != s.state_hash) return fail(Errc::SnapshotInvalid, h);
    return good();
}

}  // namespace arbiter
