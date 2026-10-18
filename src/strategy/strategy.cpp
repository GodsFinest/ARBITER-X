#include "arbiter/strategy.hpp"
#include "arbiter/errors.hpp"

#include <algorithm>
#include <unordered_map>

namespace arbiter {

namespace {
constexpr Qty kNoAhead = 0;
}  // namespace

// ---- MarketMakerStrategy ----

void MarketMakerStrategy::bump_pending_ahead(Side side, Price price, Qty qty) {
    for (auto& [id, oo] : own_) {
        if (!oo.confirmed && oo.side == side && oo.price == price) oo.ahead += qty;
    }
}

void MarketMakerStrategy::on_md_new(u64 md_id, Side side, Price price, Qty qty) {
    auto it = own_.find(md_id);
    if (it != own_.end()) {
        // Our order confirmed resting. Everything the view holds at this
        // price arrived before us, so it is all ahead of us.
        it->second.confirmed = true;
        it->second.confirm_seq = ++confirm_seq_counter_;
        it->second.ahead = view_.qty_at(side, price);
        return;
    }
    view_.rest_order(Order{md_id, 0, side, OrderType::Limit, Tif::GTC, price, qty, qty, 0, 0, 0});
    // Orders of ours that were sent but not yet confirmed may now be behind
    // this order, no. This order arrived before any unconfirmed order's own
    // MdNew will appear, so it counts as ahead for them.
    bump_pending_ahead(side, price, qty);
    const Price bb = view_.best_bid(), ba = view_.best_ask();
    if (bb && ba) {
        fair_ = (bb + ba) / 2;
        have_md_ = true;
    }
}

void MarketMakerStrategy::on_md_cancel(u64 md_id) {
    auto it = own_.find(md_id);
    if (it != own_.end()) {
        own_.erase(it);  // our cancel confirmed / order removed at exchange
        return;
    }
    const Order* v = view_.find(md_id);
    if (!v) return;  // unknown after a gap; snapshot recovery repairs the view
    const Qty removed = v->leaves;
    const Side s = v->side;
    const Price p = v->price;
    view_.cancel(md_id);
    // Approximation (documented in docs/EXECUTION_MODEL.md): a canceled order
    // observed at our price is assumed to have been ahead of us.
    for (auto& [id, oo] : own_) {
        if (oo.confirmed && oo.side == s && oo.price == p && oo.ahead > 0)
            oo.ahead = oo.ahead > removed ? oo.ahead - removed : kNoAhead;
    }
    const Price bb = view_.best_bid(), ba = view_.best_ask();
    if (bb && ba) fair_ = (bb + ba) / 2;
}

void MarketMakerStrategy::consume_view_level(Side s, Price price, Qty qty) {
    // 1) Determine how much of `qty` reaches our confirmed orders (exchange
    //    FIFO: others-before-us, then us, then others-after-us). `ahead`
    //    tracks others-before-us per own order.
    struct Cand { u64 confirm_seq; u64 id; };
    std::vector<Cand> cands;
    for (const auto& [id, oo] : own_)
        if (oo.confirmed && oo.side == s && oo.price == price) cands.push_back({oo.confirm_seq, id});
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.confirm_seq < b.confirm_seq; });

    Qty remaining = qty;
    Qty our_fill = 0;
    for (const auto& c : cands) {
        if (remaining == 0) break;
        auto it = own_.find(c.id);
        OwnOrder& oo = it->second;
        const Qty take = std::min(remaining, oo.ahead);
        oo.ahead -= take;
        remaining -= take;
        if (remaining > 0) {
            const Qty fq = std::min(remaining, oo.leaves);
            oo.leaves -= fq;
            our_fill += fq;
            remaining -= fq;
            if (oo.leaves == 0) own_.erase(it);
        }
    }
    if (our_fill) inference_fills_ += our_fill;
    // 2) Remove the others' share from the local view (FIFO, oldest first,
    //    matches exchange order for the ahead-of-us part; the behind part
    //    over-removes only after our orders consumed, i.e. never reached).
    if (qty > our_fill) view_.consume_at(s, price, qty - our_fill);
    const Price bb = view_.best_bid(), ba = view_.best_ask();
    if (bb && ba) fair_ = (bb + ba) / 2;
}

void MarketMakerStrategy::on_md_trade(Price price, Qty qty) {
    // Side attribution: a trade at a price where the view holds others'
    // liquidity attributes to that side. A trade at a price the view cannot
    // place (typically OUR quote made the touch, our orders are not in the
    // view) attributes by price vs the fair estimate.
    Side s;
    if (view_.qty_at(Side::Sell, price) > 0)
        s = Side::Sell;
    else if (view_.qty_at(Side::Buy, price) > 0)
        s = Side::Buy;
    else if (!have_md_)
        return;
    else
        s = price >= fair_ ? Side::Sell : Side::Buy;
    // Ambiguity accounting: if any of our orders at this (side, price) are
    // still UNCONFIRMED, this trade raced our own MdNew, the model cannot
    // know from its information set whether it consumed us (partially or at
    // all). Record the quantity so the inference metric can report the
    // attribution UNCERTAINTY BOUND instead of presenting the residual as
    // if it were precise.
    for (const auto& [id, oo] : own_)
        if (!oo.confirmed && oo.side == s && oo.price == price) {
            ambiguous_qty_ += qty;
            break;
        }
    consume_view_level(s, price, qty);
}

void MarketMakerStrategy::on_fill(u64 order_id, Side side, Price price, Qty qty) {
    ++real_fills_;
    real_filled_qty_ += qty;
    const i64 signed_qty = is_buy(side) ? static_cast<i64>(qty) : -static_cast<i64>(qty);
    inventory_ += signed_qty;
    cash_ -= signed_qty * price;  // buy: cash down; sell: cash up (ticks*qty)
    auto it = own_.find(order_id);
    if (it != own_.end()) {
        it->second.leaves = it->second.leaves > qty ? it->second.leaves - qty : 0;
        if (it->second.leaves == 0) own_.erase(it);
    }
}

void MarketMakerStrategy::on_cancel_confirmed(u64 order_id) { own_.erase(order_id); }

void MarketMakerStrategy::on_reject(u64 order_id) { own_.erase(order_id); }

std::vector<StrategyAction> MarketMakerStrategy::build_quotes() {
    std::vector<StrategyAction> acts;
    if (!cfg_.enabled || !have_md_) return acts;
    const i64 skew = inventory_ * cfg_.skew_ticks / cfg_.skew_denominator;
    Price bid = fair_ - cfg_.half_spread_ticks - skew;
    Price ask = fair_ + cfg_.half_spread_ticks - skew;
    bid = std::max<Price>(bid, 2);
    ask = std::min<Price>(ask, 99'999);
    if (ask <= bid) ask = bid + 1;
    const bool want_bid = inventory_ < cfg_.max_inventory;
    const bool want_ask = inventory_ > -cfg_.max_inventory;

    // Action emission order must never depend on
    // unordered_map iteration order, a restored world would otherwise assign
    // different network draws to the same logical messages. Iterate by id.
    u64 have_bid = 0, have_ask = 0;
    std::vector<u64> ids;
    ids.reserve(own_.size());
    for (const auto& [id, oo] : own_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::vector<u64> to_cancel;
    for (u64 id : ids) {
        const OwnOrder& oo = own_.at(id);
        if (oo.side == Side::Buy) {
            if (!want_bid || oo.price != bid)
                to_cancel.push_back(id);
            else
                have_bid = id;
        } else {
            if (!want_ask || oo.price != ask)
                to_cancel.push_back(id);
            else
                have_ask = id;
        }
    }
    for (u64 id : to_cancel) {
        StrategyAction a;
        a.kind = StrategyAction::Kind::Cancel;
        a.cancel_id = id;
        acts.push_back(a);
    }
    if (want_bid && !have_bid) {
        StrategyAction a;
        a.kind = StrategyAction::Kind::Send;
        a.ord = Order{next_order_id_++, cfg_.account, Side::Buy, OrderType::Limit, Tif::GTC,
                      bid, cfg_.quote_size, cfg_.quote_size, 0, 0, 0};
        own_[a.ord.id] = OwnOrder{Side::Buy, bid, cfg_.quote_size, kNoAhead, false, 0};
        acts.push_back(a);
    }
    if (want_ask && !have_ask) {
        StrategyAction a;
        a.kind = StrategyAction::Kind::Send;
        a.ord = Order{next_order_id_++, cfg_.account, Side::Sell, OrderType::Limit, Tif::GTC,
                      ask, cfg_.quote_size, cfg_.quote_size, 0, 0, 0};
        own_[a.ord.id] = OwnOrder{Side::Sell, ask, cfg_.quote_size, kNoAhead, false, 0};
        acts.push_back(a);
    }
    if (!acts.empty()) ++quote_updates_;
    return acts;
}

std::vector<StrategyAction> MarketMakerStrategy::on_timer() { return build_quotes(); }

void MarketMakerStrategy::rebuild_view(const LadderView& ladder) {
    view_ = OptBook{OptBook::Options{1, 100'000, 512}};
    for (const auto& lvl : ladder.bids)
        for (const auto& o : lvl.orders) {
            if (own_.count(o.id)) continue;
            view_.rest_order(Order{o.id, 0, Side::Buy, OrderType::Limit, Tif::GTC, lvl.price,
                                   o.qty, o.leaves, 0, 0, 0});
        }
    for (const auto& lvl : ladder.asks)
        for (const auto& o : lvl.orders) {
            if (own_.count(o.id)) continue;
            view_.rest_order(Order{o.id, 0, Side::Sell, OrderType::Limit, Tif::GTC, lvl.price,
                                   o.qty, o.leaves, 0, 0, 0});
        }
    // Own confirmed orders missing from the authoritative ladder are gone.
    for (auto it = own_.begin(); it != own_.end();) {
        if (it->second.confirmed) {
            const LadderLevel* lvl = nullptr;
            bool found = false;
            for (const auto& L : ladder.bids)
                if (L.price == it->second.price) lvl = &L;
            for (const auto& L : ladder.asks)
                if (L.price == it->second.price) lvl = &L;
            if (lvl)
                for (const auto& o : lvl->orders)
                    if (o.id == it->first) found = true;
            if (!found)
                it = own_.erase(it);
            else
                ++it;
        } else {
            ++it;
        }
    }
    // recompute ahead
    for (auto& [id, oo] : own_)
        if (oo.confirmed) oo.ahead = view_.qty_at(oo.side, oo.price);
}

void MarketMakerStrategy::hash_into(Hasher& h) const {
    h.i64v(inventory_);
    h.i64v(cash_);
    h.i64v(fair_);
    h.boolean(have_md_);
    h.u64v(next_order_id_);
    h.u64v(inference_fills_);
    h.u64v(real_fills_);
    h.u64v(real_filled_qty_);
    h.u64v(ambiguous_qty_);
    h.u64v(quote_updates_);
    h.u64v(confirm_seq_counter_);
    h.u64v(own_.size());
    std::vector<u64> ids;
    ids.reserve(own_.size());
    for (const auto& [id, oo] : own_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (u64 id : ids) {
        const OwnOrder& oo = own_.at(id);
        h.u64v(id);
        h.u8v(static_cast<u8>(oo.side));
        h.i64v(oo.price);
        h.u64v(oo.leaves);
        h.u64v(oo.ahead);
        h.boolean(oo.confirmed);
        h.u64v(oo.confirm_seq);
    }
    // Local view ladder: quotes derive from it, so it is behaviorally
    // relevant state (not just a cache).
    canonical_book_hash(view_.to_ladder(), h);
}

void MarketMakerStrategy::restore_state(i64 inventory, i64 cash, Price fair, u64 next_id,
                                        const std::vector<OwnSnapshot>& own) {
    restore_full(inventory, cash, fair, have_md_, next_id, inference_fills_, real_fills_,
                 real_filled_qty_, ambiguous_qty_, quote_updates_, confirm_seq_counter_, own,
                 view_.to_ladder());
}

void MarketMakerStrategy::restore_full(i64 inventory, i64 cash, Price fair, bool have_md,
                                        u64 next_id, u64 inference_fills, u64 real_fills,
                                        u64 real_filled_qty, u64 ambiguous_qty,
                                        u64 quote_updates, u64 confirm_seq_counter,
                                        const std::vector<OwnSnapshot>& own,
                                        const LadderView& view) {
    inventory_ = inventory;
    cash_ = cash;
    fair_ = fair;
    have_md_ = have_md;
    next_order_id_ = next_id;
    inference_fills_ = inference_fills;
    real_fills_ = real_fills;
    real_filled_qty_ = real_filled_qty;
    ambiguous_qty_ = ambiguous_qty;
    quote_updates_ = quote_updates;
    confirm_seq_counter_ = confirm_seq_counter;
    // view: rebuild the local book from the canonical ladder
    view_ = OptBook{OptBook::Options{1, 100'000, 512}};
    for (const auto& lvl : view.bids)
        for (const auto& o : lvl.orders)
            view_.rest_order(Order{o.id, 0, Side::Buy, OrderType::Limit, Tif::GTC, lvl.price,
                                   o.qty, o.leaves, 0, 0, 0});
    for (const auto& lvl : view.asks)
        for (const auto& o : lvl.orders)
            view_.rest_order(Order{o.id, 0, Side::Sell, OrderType::Limit, Tif::GTC, lvl.price,
                                   o.qty, o.leaves, 0, 0, 0});
    own_.clear();
    for (const auto& s : own) {
        OwnOrder oo{s.side, s.price, s.leaves, s.ahead, s.confirmed, s.confirm_seq};
        own_[s.id] = oo;
    }
}

std::vector<MarketMakerStrategy::OwnSnapshot> MarketMakerStrategy::own_snapshot() const {
    std::vector<OwnSnapshot> out;
    out.reserve(own_.size());
    for (const auto& [id, oo] : own_)
        out.push_back({id, oo.side, oo.price, oo.leaves, oo.ahead, oo.confirmed, oo.confirm_seq});
    return out;
}

void MomentumStrategy::restore_full(i64 inventory, i64 cash, i64 last_px, u64 next_id,
                                    u64 filled_qty, const std::vector<i64>& signs) {
    inventory_ = inventory;
    cash_ = cash;
    last_px_ = last_px;
    next_order_id_ = next_id;
    filled_qty_ = filled_qty;
    signs_ = signs;
}

// ---- MomentumStrategy ----

void MomentumStrategy::on_md_trade(Price price) {
    if (last_px_ != 0) {
        const i64 sign = price > last_px_ ? 1 : (price < last_px_ ? -1 : 0);
        signs_.push_back(sign);
        if (signs_.size() > cfg_.window) signs_.erase(signs_.begin());
    }
    last_px_ = price;
}

std::vector<StrategyAction> MomentumStrategy::on_timer() {
    std::vector<StrategyAction> acts;
    if (!cfg_.enabled) return acts;
    i64 sum = 0;
    for (i64 s : signs_) sum += s;
    if (std::abs(sum) < cfg_.min_signal) return acts;
    const Side side = sum > 0 ? Side::Buy : Side::Sell;
    const i64 headroom = cfg_.max_inventory - (side == Side::Buy ? inventory_ : -inventory_);
    if (headroom <= 0) return acts;
    const Qty q = std::min<Qty>(cfg_.size, static_cast<Qty>(headroom));
    StrategyAction a;
    a.kind = StrategyAction::Kind::Send;
    a.ord = Order{next_order_id_++, cfg_.account, side, OrderType::Market, Tif::IOC,
                  0, q, q, 0, 0, 0};
    acts.push_back(a);
    return acts;
}

void MomentumStrategy::on_fill(u64, Side side, Price price, Qty qty) {
    const i64 signed_qty = is_buy(side) ? static_cast<i64>(qty) : -static_cast<i64>(qty);
    inventory_ += signed_qty;
    cash_ -= signed_qty * price;
    filled_qty_ += qty;
}

void MomentumStrategy::hash_into(Hasher& h) const {
    h.i64v(inventory_);
    h.i64v(cash_);
    h.i64v(last_px_);
    h.u64v(next_order_id_);
    h.u64v(filled_qty_);
    h.u64v(signs_.size());
    for (i64 s : signs_) h.i64v(s);
}

}  // namespace arbiter