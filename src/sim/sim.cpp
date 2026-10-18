#include "arbiter/sim.hpp"

#include <algorithm>
#include <cmath>

namespace arbiter {

MarketSim::MarketSim(const SimConfig& cfg, u64 master_seed)
    : cfg_(cfg),
      provider_rng_(Rng::stream(master_seed, 0x515001)),
      taker_rng_(Rng::stream(master_seed, 0x515002)),
      regime_rng_(Rng::stream(master_seed, 0x515003)),
      price_rng_(Rng::stream(master_seed, 0x515004)) {
    regime_until_ = cfg.regime_dwell_ns;
    last_bb_ = cfg.start_mid - 1;
    last_ba_ = cfg.start_mid + 1;
    schedule_provider();
    schedule_taker();
}

namespace {
// Exponential interarrival draw (integer ns). The IEEE double is a draw value
// only; it never enters comparable state.
LogicalNs exp_draw(Rng& rng, double per_ns_rate) {
    double u = rng.unit();
    if (u <= 0) u = 1e-12;
    double dt = -std::log(1.0 - u) / per_ns_rate;
    if (dt < 1.0) dt = 1.0;
    if (dt > 1e12) dt = 1e12;
    return static_cast<LogicalNs>(dt);
}
}  // namespace

void MarketSim::schedule_provider() {
    const double mult = regime_ == 1 ? cfg_.volatile_rate_multiplier : 1.0;
    next_provider_t_ = now_ + exp_draw(provider_rng_, cfg_.provider_arrival_per_ms * 1e-6 * mult);
}

void MarketSim::schedule_taker() {
    const double mult = regime_ == 1 ? cfg_.volatile_rate_multiplier : 1.0;
    double rate = cfg_.taker_arrival_per_ms * 1e-6 * mult;
    // Self-excitation: if a trade executed since the last taker draw, the
    // NEXT inter-arrival is shortened by factor (1-k). k=0 -> no change
    // (Poisson null, bit-identical to the historical process).
    if (taker_boost_active_ && cfg_.taker_self_excitation > 0.0)
        rate /= (1.0 - cfg_.taker_self_excitation);
    taker_boost_active_ = false;
    next_taker_t_ = now_ + exp_draw(taker_rng_, rate);
}

void MarketSim::note_trade(Price price) {
    if (last_trade_px_ != 0) {
        const i64 d = price > last_trade_px_ ? 1 : (price < last_trade_px_ ? -1 : 0);
        bias_ = std::clamp(bias_ + d, i64{-4}, i64{4});
    }
    last_trade_px_ = price;
    // every executed trade arms the next-taker intensity boost (memory for
    // the clustering mechanism; harmless when taker_self_excitation == 0,
    // and NOT consumed as an RNG draw, so the null stream stays aligned)
    taker_boost_active_ = true;
}

Price MarketSim::provider_price(Side s, bool deep) const {
    const i64 off = cfg_.touch_offset_ticks + (deep ? cfg_.deep_offset_ticks : 0);
    // Quote relative to the live touch AND never cross: when one side of the
    // book is empty (its last observed price is stale), the provider anchors
    // to the OPPOSITE side so the empty side can reform. Without this, a
    // one-sided market ratchets: stale-anchor orders cross and execute
    // instantly, and the empty side can never rebuild.
    if (is_buy(s)) {
        const Price best = last_bb_ != 0 ? last_bb_ : cfg_.start_mid - 1;
        const Price cap = last_ba_ != 0 ? last_ba_ - 1 : best + 2 * off;
        return std::clamp(std::min(best + static_cast<Price>(off), cap),
                          cfg_.min_price, cfg_.max_price);
    }
    const Price best = last_ba_ != 0 ? last_ba_ : cfg_.start_mid + 1;
    const Price floor = last_bb_ != 0 ? last_bb_ + 1 : best - 2 * off;
    return std::clamp(std::max(best - static_cast<Price>(off), floor),
                      cfg_.min_price, cfg_.max_price);
}

BotAction MarketSim::next_action() {
    ++actions_;
    // regime switch check
    if (now_ >= regime_until_) {
        regime_ = regime_rng_.chance(0.5) ? 1 - regime_ : regime_;
        regime_until_ = now_ + cfg_.regime_dwell_ns;
    }
    BotAction a;
    if (next_provider_t_ <= next_taker_t_) {
        a.account = 100 + static_cast<u32>(provider_rng_.bounded(cfg_.provider_accounts));
        schedule_provider();
        if (!live_provider_orders_.empty() && provider_rng_.unit() < cfg_.cancel_fraction) {
            const size_t idx = provider_rng_.bounded(live_provider_orders_.size());
            a.kind = BotAction::Kind::Cancel;
            a.target = live_provider_orders_[idx];
            live_provider_orders_.erase(live_provider_orders_.begin() + static_cast<long>(idx));
            return a;
        }
        a.kind = BotAction::Kind::Add;
        a.ord = Order{};
        a.ord.id = next_bot_id_++;
        a.ord.account = a.account;
        a.ord.type = OrderType::Limit;
        a.ord.tif = Tif::GTC;
        a.ord.side = price_rng_.chance(0.5) ? Side::Buy : Side::Sell;
        a.ord.price = provider_price(a.ord.side, price_rng_.chance(0.25));
        a.ord.qty = std::max<Qty>(1, cfg_.provider_qty *
                                         (regime_ == 1 ? cfg_.volatile_qty_multiplier : 1));
        a.ord.leaves = a.ord.qty;
        live_provider_orders_.push_back(a.ord.id);
        return a;
    }
    schedule_taker();
    a.kind = BotAction::Kind::Taker;
    a.account = 200;
    a.ord = Order{};
    a.ord.id = next_bot_id_++;
    a.ord.account = a.account;
    a.ord.type = OrderType::Market;
    a.ord.tif = Tif::IOC;
    // momentum bias: takers lean with the recent flow
    double p_buy = 0.5 + 0.08 * static_cast<double>(bias_);
    // clustering sign persistence: in the clustered regime a taker FOLLOWS
    // the previous taker's side with probability k (real bursts are signed).
    // k=0 -> independent sides (the Poisson null keeps exactly its
    // historical bit-stream).
    const bool has_last_side = last_taker_side_ == static_cast<u8>(Side::Buy) ||
                               last_taker_side_ == static_cast<u8>(Side::Sell);
    if (cfg_.taker_self_excitation > 0.0 && has_last_side &&
        taker_rng_.unit() < cfg_.taker_self_excitation) {
        a.ord.side = static_cast<Side>(last_taker_side_);
        last_taker_side_ = static_cast<u8>(a.ord.side);
        a.ord.price = 0;
        a.ord.qty = std::max<Qty>(1, cfg_.taker_qty * (regime_ == 1 ? cfg_.volatile_qty_multiplier : 1));
        a.ord.leaves = a.ord.qty;
        return a;
    }
    a.ord.side = taker_rng_.unit() < p_buy ? Side::Buy : Side::Sell;
    last_taker_side_ = static_cast<u8>(a.ord.side);
    a.ord.price = 0;
    a.ord.qty = std::max<Qty>(1, cfg_.taker_qty * (regime_ == 1 ? cfg_.volatile_qty_multiplier : 1));
    a.ord.leaves = a.ord.qty;
    return a;
}

MarketSim::SimState MarketSim::state() const {
    SimState s;
    s.provider_rng = provider_rng_.state;
    s.taker_rng = taker_rng_.state;
    s.regime_rng = regime_rng_.state;
    s.price_rng = price_rng_.state;
    s.next_provider_t = next_provider_t_;
    s.next_taker_t = next_taker_t_;
    s.regime = regime_;
    s.regime_until = regime_until_;
    s.actions = actions_;
    s.bias = bias_;
    s.last_trade_px = last_trade_px_;
    s.next_bot_id = next_bot_id_;
    s.last_bb = last_bb_;
    s.last_ba = last_ba_;
    s.now = now_;
    s.taker_boost_active = taker_boost_active_;
    s.last_taker_side = last_taker_side_;
    s.live_provider_orders = live_provider_orders_;
    return s;
}

void MarketSim::restore(const SimState& s) {
    provider_rng_.state = s.provider_rng;
    taker_rng_.state = s.taker_rng;
    regime_rng_.state = s.regime_rng;
    price_rng_.state = s.price_rng;
    next_provider_t_ = s.next_provider_t;
    next_taker_t_ = s.next_taker_t;
    regime_ = s.regime;
    regime_until_ = s.regime_until;
    actions_ = s.actions;
    bias_ = s.bias;
    last_trade_px_ = s.last_trade_px;
    next_bot_id_ = s.next_bot_id;
    last_bb_ = s.last_bb;
    last_ba_ = s.last_ba;
    now_ = s.now;
    taker_boost_active_ = s.taker_boost_active;
    last_taker_side_ = s.last_taker_side;
    live_provider_orders_ = s.live_provider_orders;
}

}  // namespace arbiter