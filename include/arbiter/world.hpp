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
#include <utility>
#include <memory>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

namespace arbiter {

struct WorldConfig {
    // Deterministic identity of the configuration: covers every field that
    // can change execution behavior (tag ARBITERX/WORLDCFG/3, includes
    // risk.enabled, mm.enabled, momentum.enabled). Journal/instrumentation
    // fields (journal_events, journal_path, checkpoint/hash cadence) are
    // excluded: they never feed back into the deterministic world. Any new
    // behavior-changing field MUST be added to WorldConfig::hash and bump
    // the tag, see tests/test_quant.cpp (config_hash_covers_all_fields).
    u64 hash() const;

    // --- logical latency knobs (the counterfactual variables) ---
    i64 algo_latency_ns = 300;         // strategy decision -> order frame departs
    // Cancel-specific decision latency (message-type granularity, §27):
    // cancels are smaller/simpler messages than new orders, and their
    // economics (queue-exposure termination under adverse flow) differ
    // structurally. Defaults to algo_latency_ns so existing behavior is
    // bit-identical unless explicitly set.
    i64 cancel_latency_ns = -1;       // < 0 = "follow algo_latency_ns"
    i64 exchange_processing_ns = 150;  // gateway accept -> match
    i64 publish_latency_ns = 80;       // match -> execution frame departs
    NetProfile md_channel{200, 300, 0.0, 0.0, 0, 200, 0, 0.0};      // exchange -> strategy
    NetProfile order_channel{250, 200, 0.0, 0.0, 0, 200, 0, 0.0};   // strategy -> exchange
    NetProfile exec_channel{200, 250, 0.0, 0.0, 0, 200, 0, 0.0};    // exchange -> strategy

    // --- market ---
    SimConfig sim;
    LogicalNs horizon_ns = 200'000'000;   // 200 ms logical default
    u64 max_events = 2'000'000;
    // exchange fee schedule: flat fee per lot per fill (integer ticks*qty
    // units, cash accounting stays integer). A counterfactual variable:
    // fork the world with fee_per_lot changed and measure the PnL shift.
    i64 fee_per_lot = 0;
    // Interference control (docs/COUNTERFACTUALS.md "fixed vs reactive
    // world"): when true, bots observe a SHADOW touch computed WITHOUT
    // strategy orders, so their behavior is exogenous to the strategies,
    // the counterfactual then measures strategy-vs-frozen-market effects
    // (no SUTVA violation from bot reactions). When false (default), bots
    // see the live touch and may react to strategy quotes: the measured
    // effect includes market feedback. Comparing the two modes estimates
    // interference bias in this simulator.
    bool bots_observe_shadow = false;

    // --- risk ---
    RiskConfig risk;
    Price book_min_price = 1000;
    Price book_max_price = 2000;

    // effective decision->departure delay for a message kind (cancel falls
    // back to algo_latency_ns when cancel_latency_ns < 0)
    i64 decision_latency_for(bool is_cancel) const noexcept {
        return is_cancel && cancel_latency_ns >= 0 ? cancel_latency_ns : algo_latency_ns;
    }

    // --- strategies ---
    StrategyConfig mm;
    MomentumConfig momentum;

    // --- instrumentation ---
    bool journal_events = true;
    std::string journal_path;          // default: artifacts/world_<seed>.journal
    LogicalNs warmup_ns = 0;           // strategies idle until this logical time
    u64 checkpoint_every_events = 0;   // 0 = no Checkpoint events
    u64 hash_every_events = 256;
    // stage tracing (decision->fill decomposition). Pure instrumentation:
    // must never alter behavior; equivalence is test-enforced
    // (world_tracing_off_matches_tracing_on).
    bool stage_tracing = true;
};

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
    // bypasses the wire.
    struct Msg {
        bool is_cancel = false;
        Order ord{};          // decoded view (rebuilt from the frame at arrival)
        std::string frame;    // encoded wire frame (authoritative in-flight bytes)
    };
    // A notification in flight on the exec channel, encoded as an exec-report
    // wire frame.
    // kind: 0 = fill, 1 = risk reject, 2 = IOC remainder, 3 = cancel confirm
    struct Notify {
        u8 kind = 0;
        Fill fill{};
        std::string frame;   // encoded wire frame (authoritative in-flight bytes)
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
    void feed_handler_md(u64 md_seq);
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
    // Encode/decode one order intent as a wire frame (Send/Cancel). The
    // gateway path reconstructs the order from decode output ONLY.
    static std::string encode_order_frame(const StrategyAction& act, u32 mm_account);
    static Order decode_order_frame(const std::string& frame, bool& is_cancel, bool& ok);
    // Encode an execution notification into its wire frame (kind 0..3).
    static std::string encode_notify_frame(const Notify& n);
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

    // feed handler state (strategy side): receive buffer keyed by md_seq
    std::map<u64, Event> pending_md_;
    u64 expected_md_seq_ = 1;
    bool gap_request_pending_ = false;

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