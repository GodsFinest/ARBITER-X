#pragma once
// Strategies. Orders NEVER take a shortcut to a fill: every decision leaves
// the strategy as an order intent and traverses
//   strategy -> risk -> wire -> gateway -> exchange -> matching -> fill,
// with logical latencies at every hop (docs/EXECUTION_MODEL.md).
//
// The strategy maintains a LOCAL BOOK VIEW built only from the market-data
// stream it receives through the emulated network (gap detection and
// recovery included), it never touches the exchange's book directly.
//
// The queue-position model tracks, for each of the strategy's
// resting orders, the quantity ahead of it at its price level, updated by
// MdNew / MdCancel / MdTrade events. Fill inference from the queue model is
// compared against real fills in live runs (reported as
// queue_inference_filled vs fills), this quantifies the model's accuracy
// instead of assuming it.

#include "arbiter/book_common.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/opt_book.hpp"
#include "arbiter/order.hpp"
#include "arbiter/types.hpp"

#include <unordered_map>
#include <vector>

namespace arbiter {

struct StrategyConfig {
    u32 account = 7;                  // strategy account id (risk/bots differ)
    Qty quote_size = 25;
    i64 half_spread_ticks = 1;  // join the touch: a maker quoting wider never trades in this market
    i64 skew_denominator = 1000;      // inventory skew: price_offset = inv * skew_ticks / skew_denominator
    i64 skew_ticks = 6;
    i64 max_inventory = 4000;
    LogicalNs requote_interval_ns = 25'000;
    bool enabled = true;
};

// One order the strategy wants to send (post-risk routing is the World's job).
struct OrderIntent {
    Side side{};
    OrderType type{};
    Tif tif{};
    Price price = 0;
    Qty qty = 0;
    LogicalNs expiry = 0;  // GTD
};

// One decision the strategy hands to the World for routing through the real
// execution path (risk -> wire -> gateway -> exchange).
struct StrategyAction {
    enum class Kind : u8 { Send = 0, Cancel = 1 };
    Kind kind = Kind::Send;
    Order ord{};     // Send
    u64 cancel_id = 0;  // Cancel
};

class MarketMakerStrategy {
public:
    explicit MarketMakerStrategy(const StrategyConfig& cfg) : cfg_(cfg) {}

    // --- inbound (already delivered through the network layer) ---
    void on_md_new(u64 md_id, Side side, Price price, Qty qty);
    void on_md_cancel(u64 md_id);
    void on_md_trade(Price price, Qty qty);
    // Real fill delivered by the exchange (authoritative).
    void on_fill(u64 order_id, Side side, Price price, Qty qty);
    // Exchange fee charged on a fill (integer ticks*qty; reduces cash).
    void on_fee(i64 amount) noexcept { cash_ -= amount; }
    void on_cancel_confirmed(u64 order_id);
    void on_reject(u64 order_id);
    // Periodic decision point; returns actions (quote maintenance).
    std::vector<StrategyAction> on_timer();

    // --- introspection ---
    Price fair() const noexcept { return fair_; }
    i64 inventory() const noexcept { return inventory_; }
    i64 cash() const noexcept { return cash_; }  // ticks*qty
    i64 marked_pnl(Price mid) const noexcept { return cash_ + inventory_ * mid; }
    u64 resting_count() const noexcept { return own_.size(); }
    u64 inference_fills() const noexcept { return inference_fills_; }
    u64 inference_fill_qty() const noexcept { return inference_fills_; }  // lots (same counter: inference sums qty)
    u64 real_fills() const noexcept { return real_fills_; }               // fill EVENTS (authoritative)
    u64 real_filled_qty() const noexcept { return real_filled_qty_; }      // fill LOTS (authoritative)
    // Trade quantity that arrived while our own orders at that level were
    // still UNCONFIRMED (own MdNew not yet delivered): the model cannot
    // attribute those trades to us or others from its information set,
    // the bound on potential mis-attribution, reported explicitly instead
    // of hiding it inside the point estimate.
    u64 ambiguous_fill_qty() const noexcept { return ambiguous_qty_; }
    u64 quote_updates() const noexcept { return quote_updates_; }
    bool has_market_data() const noexcept { return have_md_; }
    u64 confirm_seq_counter() const noexcept { return confirm_seq_counter_; }
    LadderView view_ladder() const { return view_.to_ladder(); }

    void hash_into(Hasher& h) const;
    u64 state_hash() const {
        Hasher h;
        h.tag("ARBITERX/STRAT/2");  // /2: +have_md/real_filled_qty/quote_updates/confirm_seq/view ladder
        hash_into(h);
        return h.value();
    }

    // Gap/snapshot recovery: rebuild the local view from an exchange ladder
    // (the strategy's own orders are excluded via its id set).
    void rebuild_view(const LadderView& ladder);

    // restore support (full state incl. confirmation + FIFO seq + view)
    struct OwnSnapshot {
        u64 id;
        Side side;
        Price price;
        Qty leaves;
        Qty ahead;
        bool confirmed = true;
        u64 confirm_seq = 0;
    };
    void restore_state(i64 inventory, i64 cash, Price fair, u64 next_id,
                       const std::vector<OwnSnapshot>& own);
    void restore_full(i64 inventory, i64 cash, Price fair, bool have_md, u64 next_id,
                      u64 inference_fills, u64 real_fills, u64 real_filled_qty,
                      u64 ambiguous_qty, u64 quote_updates, u64 confirm_seq_counter,
                      const std::vector<OwnSnapshot>& own, const LadderView& view);
    void add_restored_own(const OwnSnapshot& s) {
        own_[s.id] = OwnOrder{s.side, s.price, s.leaves, s.ahead, s.confirmed, s.confirm_seq};
    }
    std::vector<OwnSnapshot> own_snapshot() const;
    u64 next_order_id() const noexcept { return next_order_id_; }
    void set_next_order_id(u64 v) noexcept { next_order_id_ = v; }

    const StrategyConfig& config() const noexcept { return cfg_; }

private:
    struct OwnOrder {
        Side side;
        Price price;
        Qty leaves;
        Qty ahead;               // queue position model: qty ahead at our price
        bool confirmed = false;  // our own MdNew seen (resting confirmed)
        u64 confirm_seq = 0;     // FIFO order among our orders at a price
    };

    void consume_view_level(Side side, Price price, Qty qty);
    void bump_pending_ahead(Side side, Price price, Qty qty);
    std::vector<StrategyAction> build_quotes();

    StrategyConfig cfg_;
    OptBook view_{OptBook::Options{1, 100'000, 512}};  // others' orders only
    std::unordered_map<u64, OwnOrder> own_;
    i64 inventory_ = 0;
    i64 cash_ = 0;
    Price fair_ = 1500;   // last mid estimate
    bool have_md_ = false;
    u64 next_order_id_ = 1;
    u64 inference_fills_ = 0;   // inferred fill LOTS (queue-model estimate)
    u64 real_fills_ = 0;        // authoritative fill EVENTS
    u64 real_filled_qty_ = 0;   // authoritative fill LOTS (same unit as inference_fills_)
    u64 ambiguous_qty_ = 0;     // trade LOTS at prices where we had UNCONFIRMED own orders
    u64 quote_updates_ = 0;
    u64 confirm_seq_counter_ = 0;
};

// Simple deterministic momentum taker.
// next_order_id_ starts at a base DISJOINT from the MM's id space (which
// starts at 1): order ids are GLOBALLY unique per experiment (the gateway
// dedups on id, two strategies emitting the same id would be a silent
// cross-strategy kill).
struct MomentumConfig {
    u32 account = 8;
    Qty size = 30;
    i64 min_signal = 3;               // signed sum of last k trade directions
    size_t window = 8;
    LogicalNs cooldown_ns = 40'000;
    i64 max_inventory = 3000;
    bool enabled = true;
    u64 id_base = 1'000'000'000;      // momentum ids live in [id_base, ...)
};

class MomentumStrategy {
public:
    explicit MomentumStrategy(const MomentumConfig& cfg) : cfg_(cfg), next_order_id_(cfg.id_base) {}
    void on_md_trade(Price price);
    std::vector<StrategyAction> on_timer();
    void on_fill(u64 order_id, Side side, Price price, Qty qty);
    void on_fee(i64 amount) noexcept { cash_ -= amount; }
    i64 inventory() const noexcept { return inventory_; }
    i64 cash() const noexcept { return cash_; }
    i64 marked_pnl(Price mid) const noexcept { return cash_ + inventory_ * mid; }
    i64 last_px() const noexcept { return last_px_; }
    u64 filled_qty() const noexcept { return filled_qty_; }  // authoritative LOTS
    const std::vector<i64>& signs_window() const noexcept { return signs_; }
    u64 next_order_id() const noexcept { return next_order_id_; }
    void set_next_order_id(u64 v) noexcept { next_order_id_ = v; }
    void hash_into(Hasher& h) const;
    void restore_full(i64 inventory, i64 cash, i64 last_px, u64 next_id, u64 filled_qty,
                      const std::vector<i64>& signs);
    const MomentumConfig& config() const noexcept { return cfg_; }

private:
    MomentumConfig cfg_;
    std::vector<i64> signs_;
    i64 last_px_ = 0;
    i64 inventory_ = 0, cash_ = 0;
    u64 filled_qty_ = 0;                 // authoritative fill LOTS
    u64 next_order_id_ = 1'000'000'000;  // MomentumConfig::id_base (disjoint from MM ids)
};

}  // namespace arbiter