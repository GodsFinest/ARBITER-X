#pragma once
// The deterministic event machine.
//
// A World is a discrete-event simulation with a single logical clock:
//   scheduler pops the earliest event ordered by (logical_ns, rank, tiebreak)
//   -> each event kind advances one pipeline stage:
//
//     SimAction      bot decides; its orders hit the book directly (co-located)
//     MdDeliver      a market-data frame arrives at the strategy's feed handler
//     IntentDue      strategy timer fires; intents are encoded and depart
//     GatewayArrival order frame reaches the gateway: decode -> risk -> book
//     ExecNotify     execution/risk-decision frame arrives back at a strategy
//     GtdExpiry      exchange cancels an expired GTD order
//     GapRequestDue  feed handler requests retransmission after a detected gap
//
// Determinism contract (docs/DETERMINISM.md): given identical
// (config, master_seed) the event stream, book, positions and state hashes
// are bitwise identical across runs. Logical time is the ONLY clock that
// influences behavior; wall-clock is instrumentation.
//
// Two Worlds with the same seed but different
// latency parameters share the same random draw streams, so their histories
// are identical until the changed latency reorders events, measured by the
// state-hash timeline.

#include "arbiter/event.hpp"
#include "arbiter/world_config.hpp"
#include "arbiter/journal.hpp"
#include "arbiter/metrics.hpp"
#include "arbiter/net.hpp"
#include "arbiter/order.hpp"
#include "arbiter/risk.hpp"
#include "arbiter/sim.hpp"
#include "arbiter/snapshot.hpp"
#include "arbiter/world_snapshot.hpp"
#include "arbiter/strategy.hpp"
#include "arbiter/types.hpp"
#include "arbiter/wire.hpp"

#include <array>
#include <map>
#include <set>
#include <utility>
#include <memory>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

namespace arbiter {

// Scheduler item.
struct WorldEvent {
    LogicalNs t;
    u8 rank;
    u64 tiebreak;  // deterministic insertion counter
    EvType type;
    u64 a = 0, b = 0, c = 0;
    i64 p = 0;
    // ordering: time, then rank, then insertion order
    bool operator<(const WorldEvent& o) const noexcept {
        if (t != o.t) return t > o.t;          // priority_queue = max-heap: invert
        if (rank != o.rank) return rank > o.rank;
        return tiebreak > o.tiebreak;
    }
};

class World {
public:
    World(const WorldConfig& cfg, u64 master_seed);
    ~World();

    // Process at most `max_additional_events` more events (0 budget is a no-op
    // unless the world has never started). The horizon/max_events config
    // limits still apply. Returns accumulated run statistics.
    RunStats run(u64 max_additional_events = ~u64{0});
    u64 state_hash() const;

    // Deterministic identity: derived from (config, master seed).
    u64 config_hash() const noexcept { return cfg_hash_; }
    // Debug/introspection: the exchange book's cached best prices (they are
    // DERIVED state, the restore contract requires them to equal the
    // ladder-derived values after any event).
    std::pair<Price, Price> book_best() const noexcept { return {book_.best_bid(), book_.best_ask()}; }
    u64 world_id() const noexcept;

    // capture_snapshot() is non-destructive
    // and may be called between events (after run(budget) returns).
    // restore() rebuilds the complete scheduler/execution state; with
    // strict_config it rejects a config different from the captured one
    // (forks pass strict_config=false and record the branch identity).
    WorldSnapshot capture_snapshot() const;
    Status restore_snapshot(const WorldSnapshot& snap, const WorldConfig& continuation_cfg,
                            bool strict_config);

    const RunStats& stats() const noexcept { return stats_; }
    const std::string& journal_path() const noexcept { return journal_path_; }
    u64 events_processed() const noexcept { return events_; }
    LogicalNs now() const noexcept { return now_; }

    // internal event kinds used with EvType
    static constexpr EvType EvSimAction = static_cast<EvType>(20);
    static constexpr EvType EvMdDeliver = static_cast<EvType>(21);
    static constexpr EvType EvIntentDue = static_cast<EvType>(22);
    static constexpr EvType EvGatewayArrival = static_cast<EvType>(23);
    static constexpr EvType EvMatchDue = static_cast<EvType>(24);
    static constexpr EvType EvExecNotify = static_cast<EvType>(25);
    static constexpr EvType EvGtdExpiry = static_cast<EvType>(26);
    static constexpr EvType EvGapRequestDue = static_cast<EvType>(27);
    static constexpr EvType EvMarkout = static_cast<EvType>(28);  // internal: adverse-selection markout probe
    static constexpr u8 RankSim = 0, RankMd = 1, RankIntent = 2, RankGateway = 3, RankExec = 4,
                        RankGtd = 5, RankGap = 6, RankMarkout = 7;

private:
    // A message in flight on the order channel. The order itself travels as a
    // WIRE FRAME (src/exec/world.cpp route_intent); `frame` is the encoded
    // bytes and the gateway reconstructs the order by decoding it, nothing
    // bypasses the wire. `transport_seq` is the frame's wire sequence, the
    // receiver-side dedupe key: a duplicate delivery carries the SAME frame
    // and sequence and is dropped at the gateway, never re-processed and
    // never surfaced as a business-level duplicate-order reject.
    struct Msg {
        bool is_cancel = false;
        bool delivered = false;  // at least one delivery attempt was processed
        u64 transport_seq = 0;   // order-channel wire sequence (dedupe key)
        Order ord{};          // decoded view (rebuilt from the frame at arrival)
        std::string frame;    // encoded wire frame (authoritative in-flight bytes)
    };
    // A notification in flight on the exec channel, encoded as an exec-report
    // wire frame. Dedupe mirrors the order channel (same frame + sequence on
    // duplicate delivery, deduped by decoded sequence at the strategy feed).
    // kind: 0 = fill, 1 = risk reject, 2 = IOC remainder, 3 = cancel confirm
    struct Notify {
        u8 kind = 0;
        bool delivered = false;  // at least one delivery attempt was processed
        u64 transport_seq = 0;   // exec-channel wire sequence (dedupe key)
        Fill fill{};
        std::string frame;   // encoded exec-report wire frame (authoritative bytes)
    };
    // One MARKET-DATA packet ATTEMPT (original, duplicate, or retransmit).
    // Attempts are immutable and identified by packet id (the scheduled
    // EvMdDeliver event carries the id), so a retransmit can never alias the
    // original's delivery slot: the original's scheduled event delivers the
    // original frame, the retransmit's own event delivers the retransmit
    // frame after its full recovery delay. A packet is erased when its event
    // processes it (delivered or rejected), so delivery state == presence in
    // the map.
    struct MdPacket {
        u64 md_seq = 0;     // transport sequence carried inside the frame
        std::string frame;  // authoritative bytes of THIS attempt
    };
    // Exchange-authoritative per-account economics ledger (match-time truth).
    // Updated for every strategy-account fill leg at the exchange, whether or
    // not the execution report survives the exec channel. Strategy-local
    // inventory/cash remain notification-driven (behavioral view). Both legs
    // of a self-trade are recorded truthfully.
    struct LedgerEntry {
        i64 position = 0;
        i64 cash = 0;      // ticks*qty, fees included
        i64 fees = 0;
        u64 filled_qty = 0;
    };
    // A pending adverse-selection markout probe (mid sampled dt after a
    // strategy fill). The probe carries the strategy ACCOUNT so the sample
    // lands in the right archetype histogram (MM vs momentum, never pooled);
    // probe state is kept OUT of all hashes.
    struct MarkoutProbe {
        LogicalNs fill_t = 0;
        Price fill_px = 0;
        Side side = Side::Buy;   // the strategy's side in the fill
        u32 account = 0;         // which strategy's fill this probe measures
    };

    void schedule(LogicalNs t, u8 rank, EvType type, u64 a = 0, u64 b = 0, i64 p = 0);
    void process(const WorldEvent& ev);
    // Central exchange entry: matches an order, journals, publishes MD,
    // notifies strategies of fills.
    void enter_book(Order o, LogicalNs t);
    void cancel_in_book(u64 order_id, LogicalNs t);
    void publish_md(std::vector<Event>& mds, LogicalNs t);
    void route_intent(const StrategyAction& act, LogicalNs t);
    // Encode + transmit one exec-channel notification: assigns the transport
    // sequence, applies the channel's delivery plan (drop/corrupt/duplicate),
    // and schedules the delivery events. Returns true when a CLEAN (will
    // deliver and decode) report was scheduled — callers use this to track
    // open fill reports.
    bool send_notify(Notify n, LogicalNs t);
    // Terminal trace cleanup that must not depend on notification delivery:
    // when an order reaches an exchange terminal state and no fill report for
    // it is still in flight, its per-order instrumentation dies immediately
    // (lost/corrupted terminal reports cannot leak the tables).
    void maybe_cleanup_terminal_trace(u64 order_id);
    // Exchange-authoritative ledger update for one strategy fill leg.
    void ledger_fill(u32 account, Side ours, Price price, Qty qty);
    // A delivered fill report resolves its legs' open-report counts.
    void resolve_open_fill_reports(const Fill& f);
    void feed_handler_md(u64 packet_id);
    void deliver_expected();
    void maybe_schedule_gap_request();
    void gap_recover();
    LadderView book_ladder_without_own() const;
    // Best prices of the book EXCLUDING strategy orders (bots_observe_shadow
    // mode). O(level count), only used when the interference-control mode
    // is enabled.
    Price shadow_best_bid() const;
    Price shadow_best_ask() const;
    void deliver_fill(const Fill& f, LogicalNs t);
    // static encode/decode helpers (frame codecs, live path)
    static std::string encode_order_frame(const StrategyAction& act, u32 mm_account, u64 seq);
    static Order decode_order_frame(const std::string& frame, bool& is_cancel, bool& ok,
                                    u8& channel, u64& seq);
    // Encode an execution notification into its wire frame (kind 0..3).
    static std::string encode_notify_frame(const Notify& n, u64 seq);
    // Apply in-transit corruption to a frame (deterministic single-byte flip
    // inside the payload region).
    static void corrupt_frame(std::string& frame, Rng& rng);

    WorldConfig cfg_;
    u64 cfg_hash_ = 0;
    u64 master_seed_ = 0;
    bool started_ = false;
    u64 tiebreak_ = 0;
    u64 events_ = 0;
    LogicalNs now_ = 0;
    bool halted_ = false;

    std::priority_queue<WorldEvent> queue_;
    OptBook book_;
    MarketSim sim_;
    MarketMakerStrategy mm_;
    MomentumStrategy mom_;
    RiskEngine risk_;
    std::unordered_set<u64> gateway_known_ids_;

    // emulated network
    Channel md_ch_, order_ch_, exec_ch_;
    // exchange-side md retransmit ring (md_seq -> event): fixed-capacity
    // circular buffer: O(1) insert/evict (the previous vector+erase(begin)
    // shifted ~130KB per message once at capacity).
    u64 md_seq_ = 1;
    std::array<std::pair<u64, Event>, 4096> md_ring_{};
    size_t md_head_ = 0;  // index of the OLDEST entry
    size_t md_count_ = 0;
    static constexpr size_t kMdBufferCap = 4096;
    // oldest->newest view of the ring (snapshot serialization + gap_recover)
    std::vector<std::pair<u64, Event>> md_ring_dump() const;
    void md_ring_push(u64 seq, const Event& e);
    const Event* md_ring_find(u64 seq) const;

    // MD packet attempts still in transit, keyed by packet id (the scheduled
    // EvMdDeliver event carries the id in `b`). Distinct from pending_md_,
    // which holds packets that have ARRIVED at the strategy feed handler.
    std::map<u64, MdPacket> md_packets_;
    u64 md_packet_seq_ = 0;  // next MD packet id
    // feed handler state (strategy side): arrived receive buffer keyed by seq
    std::map<u64, Event> pending_md_;
    u64 expected_md_seq_ = 1;
    bool gap_request_pending_ = false;

    // Transport sequence counters (one monotonic sequence per channel) and
    // the receiver-side dedupe state (decoded sequences already accepted).
    u64 order_seq_ = 0;  // last assigned order-channel frame sequence
    u64 exec_seq_ = 0;   // last assigned exec-channel frame sequence
    std::set<u64> order_rx_seqs_;  // sequences the gateway has accepted
    std::set<u64> exec_rx_seqs_;   // sequences the strategy feed has accepted

    // order id -> published exec-channel fill reports not yet resolved by a
    // delivery event. Gates terminal trace cleanup so an early terminal
    // report cannot destroy state a later fill report still needs.
    // Instrumentation-adjacent (feeds only RunStats): snapshot-persisted for
    // stats continuity, excluded from state identity like order_sent_t_.
    std::map<u64, u64> open_fill_reports_;

    // Exchange-authoritative economics ledger (see LedgerEntry above).
    std::map<u32, LedgerEntry> ledger_;

    // in-flight messages / notifications (network "packets" under construction)
    std::vector<Msg> inflight_;      // index carried in WorldEvent.b
    std::vector<Notify> notifies_;   // index carried in WorldEvent.b
    std::unordered_map<u64, LogicalNs> order_sent_t_;  // order id -> decision time
    std::vector<MarkoutProbe> markouts_;  // index carried in WorldEvent.b
    // per-(seed,channel) stream for in-transit corruption draws (deterministic)
    Rng corrupt_rng_{0};
    // stage tracing (logical, per strategy order id): timestamps at each hop
    // of the decision->fill pipeline. Bounded: entries are erased when the
    // order reaches a terminal state (fully filled / cancel-confirmed /
    // rejected / IOC remainder). EVERY fill of a traced order is recorded,
    // so per-stage sums cover the same fill set as fill_latency.
    // Disabled entirely (cfg_.stage_tracing == false) the table stays empty:
    // same behavior, no per-order bookkeeping.
    struct StageTimes {
        LogicalNs decision = 0, depart = 0, gateway = 0, match = 0;
    };
    std::unordered_map<u64, StageTimes> stages_;

    // journal
    std::unique_ptr<JournalWriter> journal_;
    std::string journal_path_;

    RunStats stats_;

    void jappend(const Event& e);
    void schedule_seed_events();
    // record one completed order's stage deltas into the RunStats histograms
    void note_stage_trace(u64 order_id, LogicalNs fill_t);
    void drop_stage_trace(u64 order_id);  // order ended without fill (reject/cancel)
};

}  // namespace arbiter
