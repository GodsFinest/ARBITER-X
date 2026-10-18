#pragma once
// Full-world snapshot schema.
//
// A WorldSnapshot captures EVERY piece of state that can affect future
// deterministic execution of a World:
//
//   scheduler queue (all pending events, exact tiebreak ordering inputs)
//   logical time, event counter, tiebreak counter
//   exchange book (canonical ladder) + book-seq counter
//   market simulator (RNG states, next arrival times, regime, counters)
//   market-maker strategy: local view ladder, own orders (leaves, queue
//     ahead, confirmation state, FIFO seq), inventory, cash, fair value,
//     id counter, inference/quote counters
//   momentum strategy: signs window, last price, inventory, cash, id counter
//   risk state: position, accept/reject counters, per-reason counters,
//     rate-window timestamps
//   gateway known-id set
//   network channel RNG states (md/order/exec)
//   exchange MD retransmit ring, feed-handler pending buffer, expected seq
//   in-flight order messages and execution notifications
//   order decision-time table (fill-latency attribution)
//   scalar run counters (trades, md accounting, risk totals)
//
// Excluded by design (documented):
//   * raw pointers / allocator state, canonical logical state only
//   * the journal, continuation journaling is a caller decision (a restored
//     world starts a fresh journal segment; deterministic behavior is
//     unaffected since the journal never feeds back into execution)
//   * histogram / hash-timeline measurement buffers, merged by the caller
//     when full-run equivalence of *measurements* is needed; they never
//     influence execution
//   * the configuration, supplied by the caller at restore time (this is
//     what makes forks possible); the snapshot stores its config hash and
//     restore() rejects a mismatched config
//
// Self-validation: the snapshot stores World::state_hash() at capture time.
// restore() rebuilds the world and recomputes the hash; a mismatch means a
// missed state element or a corrupt file and is rejected loudly.

#include "arbiter/book_common.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/event.hpp"
#include "arbiter/sim.hpp"
#include "arbiter/types.hpp"

#include <array>
#include <string>
#include <vector>

namespace arbiter {

struct SnapSchedEvent {  // mirrors WorldEvent
    i64 t;
    i64 rank;
    i64 tiebreak;
    i64 type;
    i64 a, b, c, p;
};

struct SnapOrder {  // mirrors Order
    u64 id, book_seq;
    u32 account;
    u8 side, otype, tif;
    i64 price;
    u64 qty, leaves;
    i64 arrival, expiry;
};

struct SnapMsg {  // mirrors World::Msg
    u8 is_cancel;
    SnapOrder ord;
    std::string frame;  // encoded order wire frame in flight
};

struct SnapFill {  // mirrors Fill
    u64 taker_id, maker_id;
    u32 maker_acct, taker_acct;
    i64 price;
    u64 qty;
    i64 t;
    u8 maker_liq, taker_side, maker_side;
};

struct SnapNotify {  // mirrors World::Notify
    u8 kind;
    SnapFill fill;
    std::string frame;  // encoded exec-report wire frame in flight
};

struct SnapMarkout {  // mirrors World::MarkoutProbe (v8); account since v9
    i64 fill_t;
    i64 fill_px;
    u8 side;
    u32 account;  // strategy account owning the probe (sample routing)
};

struct SnapOwn {  // mirrors MarketMakerStrategy::OwnOrder
    u64 id;
    u8 side;
    i64 price;
    u64 leaves, ahead;
    u8 confirmed;
    u64 confirm_seq;
};

struct SnapMM {
    i64 inventory, cash, fair;
    u8 have_md;
    u64 next_order_id, inference_fills, real_fills, real_filled_qty, ambiguous_qty,
        quote_updates, confirm_seq_counter;
    std::vector<SnapOwn> own;
    LadderView view;  // strategy's local view of others' orders
};

struct SnapMom {
    i64 inventory, cash, last_px;
    u64 next_order_id;
    u64 filled_qty;  // authoritative fill LOTS (v9)
    std::vector<i64> signs;
};

struct SnapStats {  // scalar run counters (see header comment re: histograms)
    u64 trades, volume, bot_fills, strategy_fills, momentum_fills;
    u64 strategy_orders_sent, strategy_cancels_sent, order_frames_dropped;
    u64 risk_accepted, risk_rejected;
    u64 md_published, md_delivered, md_dropped, md_duplicated, md_reordered;
    u64 md_gap_events, md_recovered, md_snapshot_recoveries;
    u64 wire_frames_encoded, wire_frames_decoded, wire_frames_corrupted;
};

struct SnapRiskAccount {  // per-account RiskState (per-account isolation)
    u32 account;
    i64 position;
    u64 accepted, rejected;
    std::array<u64, 8> by_reason;
    std::vector<i64> window;
};

inline constexpr u32 kWorldSnapshotVersion = 9;  // v9: markout probe account + mm ambiguous_qty

struct WorldSnapshot {
    u32 format_version = kWorldSnapshotVersion;
    u64 config_hash = 0;
    u64 master_seed = 0;
    u64 state_hash = 0;

    u64 events = 0, tiebreak = 0;
    i64 now = 0;
    u64 md_seq = 0, expected_md_seq = 0;
    u8 gap_pending = 0, halted = 0;
    u64 next_book_seq = 0;

    std::vector<SnapSchedEvent> queue;

    LadderView book;
    // order-id -> owning account for every resting order (the canonical
    // ladder does not carry ownership; matching ignores it but the World's
    // notification routing does not)
    std::vector<std::pair<u64, u32>> order_accounts;

    MarketSim::SimState sim;  // rng states, arrival times, regime, counters, live list

    SnapMM mm;
    SnapMom mom;
    std::vector<SnapRiskAccount> risk_accounts;  // per-account (isolation)

    std::vector<u64> gateway_known_ids;         // sorted
    std::array<u64, 3> channel_rng = {0, 0, 0}; // md, order, exec
    std::array<u32, 3> channel_burst = {0, 0, 0};  // mid-burst counters (v7)
    u64 corrupt_rng = 0;                        // in-transit corruption stream

    std::vector<std::pair<u64, Event>> md_buffer;    // seq order
    std::vector<std::pair<u64, Event>> pending_md;   // seq order
    std::vector<SnapMsg> inflight;
    std::vector<SnapNotify> notifies;
    std::vector<SnapMarkout> markouts;  // pending adverse-selection probes (v8)
    std::vector<std::pair<u64, i64>> order_sent_t;   // sorted by order id

    SnapStats stats;
};

// Branch genealogy for forked worlds, recorded in experiment
// artifacts so every counterfactual is auditable.
struct BranchIdentity {
    u64 world_id;
    u64 parent_world_id;
    u64 fork_event;
    u64 config_hash;
    u64 initial_state_hash;
};

Status write_world_snapshot(const std::string& path, const WorldSnapshot& snap);
Result<WorldSnapshot> read_world_snapshot(const std::string& path);

}  // namespace arbiter