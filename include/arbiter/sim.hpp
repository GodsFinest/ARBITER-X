#pragma once
// Deterministic market simulator: the bot population whose interaction
// produces the market. Bots are internal exchange participants (their orders
// do not cross the emulated network and bypass the strategy-facing risk
// gateway, documented asymmetry, docs/SIMULATION.md). The exchange book is
// REAL: bots add/cancel/take against the same matching engine as the
// strategy, so research and simulation semantics cannot drift apart.
//
// Stochastic structure (all draws from named Rng streams):
//   * provider bots (liquidity): Poisson arrivals; join the touch (sometimes
//     deeper), random cancels, both sides stay alive
//   * taker bots: independent Poisson arrivals; aggressive IOC orders with a
//     momentum bias from recent trade directions
//   * regime switch: calm <-> volatile Markov chain scaling arrival rates,
//     sizes, and spreads (volatility clustering emerges from dwell times)

#include "arbiter/order.hpp"
#include "arbiter/rng.hpp"
#include "arbiter/types.hpp"

#include <vector>

namespace arbiter {

struct SimConfig {
    Price start_mid = 1500;
    Qty provider_qty = 20;
    Qty taker_qty = 12;
    double provider_arrival_per_ms = 60.0;
    double taker_arrival_per_ms = 8.0;
    double cancel_fraction = 0.35;     // of provider arrivals are cancels
    i64 touch_offset_ticks = 1;        // provider joins inside the spread
    i64 deep_offset_ticks = 6;         // sometimes quotes deeper
    LogicalNs regime_dwell_ns = 4'000'000;
    double volatile_rate_multiplier = 3.0;
    Qty volatile_qty_multiplier = 2;
    u32 provider_accounts = 3;         // number of provider bots
    Price min_price = 1000;
    Price max_price = 2000;
    // ---- flow clustering (the flagship mechanism variable) ----
    // Taker self-excitation in [0,1): each executed trade MULTIPLIES the next
    // taker inter-arrival draw by (1 - k). k=0 is exactly the memoryless
    // Poisson null (historical behavior, bit-identical); k>0 produces
    // clustered/self-exciting taker bursts whose duration grows with k.
    // This is a true intensity-memory mechanism on the taker stream: NOT
    // the calm/volatile regime chain (which stays as-is and independent).
    double taker_self_excitation = 0.0;
};

// One bot decision to be applied by the World at the scheduled time.
struct BotAction {
    enum class Kind : u8 { Add = 0, Cancel = 1, Taker = 2, None = 3 };
    Kind kind = Kind::None;
    Order ord;       // Add / Taker
    u64 target = 0;  // Cancel
    u32 account = 0;
};

class MarketSim {
public:
    MarketSim(const SimConfig& cfg, u64 master_seed);

    // Pull model: the World schedules at min(next_provider_t, next_taker_t)
    // and asks for the action due then.
    LogicalNs next_action_time() const { return std::min(next_provider_t_, next_taker_t_); }
    void advance(LogicalNs t) { now_ = t; }
    BotAction next_action();

    // The World feeds the observable book state so providers can join the touch.
    // `touch_shadowed`: when true the touch is computed EXCLUDING strategy
    // orders (interference control, see docs/COUNTERFACTUALS.md): bot
    // behavior becomes independent of the strategies' presence/quotes, so
    // a counterfactual that changes only strategy behavior faces a frozen
    // exogenous flow process (SUTVA holds by construction in that mode).
    void observe(Price best_bid, Price best_ask) {
        if (best_bid) last_bb_ = best_bid;
        if (best_ask) last_ba_ = best_ask;
    }
    void observe_shadow(Price sb, Price sa) {
        if (sb) last_bb_ = sb;
        if (sa) last_ba_ = sa;
    }
    // Taker momentum bias: fed by the World for each trade print.
    void note_trade(Price price);

    int regime() const noexcept { return regime_; }
    u64 actions_generated() const noexcept { return actions_; }

    // state for snapshot/restore (rng states + counters)
    struct SimState {
        u64 provider_rng, taker_rng, regime_rng, price_rng;
        LogicalNs next_provider_t, next_taker_t;
        int regime;
        LogicalNs regime_until;
        u64 actions;
        i64 bias;
        i64 last_trade_px;
        u64 next_bot_id;
        Price last_bb, last_ba;   // observed touch (provider quoting input)
        LogicalNs now;            // sim-local clock (advance() target)
        bool taker_boost_active;  // a trade fired since the last taker draw
        u8 last_taker_side;       // 0=Buy,1=Sell,2=none yet (sign persistence)
        std::vector<u64> live_provider_orders;
    };
    SimState state() const;
    void restore(const SimState& s);

private:
    void schedule_provider();
    void schedule_taker();
    Price provider_price(Side s, bool deep) const;

    SimConfig cfg_;
    Rng provider_rng_, taker_rng_, regime_rng_, price_rng_;
    LogicalNs now_ = 0;
    LogicalNs next_provider_t_ = 0;
    LogicalNs next_taker_t_ = 0;
    int regime_ = 0;  // 0 calm, 1 volatile
    LogicalNs regime_until_ = 0;
    u64 actions_ = 0;
    u64 next_bot_id_ = 2'000'000'000;
    i64 bias_ = 0;
    i64 last_trade_px_ = 0;
    Price last_bb_ = 0, last_ba_ = 0;
    // self-excitation memory: set when a trade executes; the NEXT taker
    // inter-arrival draw is shortened by (1-k) while set, then cleared.
    // One-shot memory (not a decaying intensity process): the minimal
    // mechanism that makes trade-triggered windows size-biased.
    bool taker_boost_active_ = false;
    // last taker side (sign-persistence memory for the clustered regime;
    // 2 = no taker has acted yet). k=0 never reads it.
    u8 last_taker_side_ = 2;
    std::vector<u64> live_provider_orders_;
};

}  // namespace arbiter