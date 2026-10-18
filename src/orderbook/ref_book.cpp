#include "arbiter/ref_book.hpp"
#include "arbiter/book_common.hpp"
#include "arbiter/errors.hpp"

#include <algorithm>

namespace arbiter {

namespace {
// Effective cross-detection limit for a taker: market orders cross anything.
Price effective_limit(const Order& o) noexcept {
    if (o.type == OrderType::Limit) return o.price;
    return is_buy(o.side) ? DomainBounds::kMaxPrice : 0;
}
}  // namespace

Qty RefBook::available_within(Side taker_side, Price limit_price) const noexcept {
    Qty total = 0;
    if (is_buy(taker_side)) {
        for (const auto& [px, lvl] : asks_) {
            if (px > limit_price) break;
            total += lvl.total;
        }
    } else {
        for (const auto& [px, lvl] : bids_) {
            if (px < limit_price) break;
            total += lvl.total;
        }
    }
    return total;
}

Qty RefBook::match_against(Order& t, std::vector<Fill>& fills) {
    Qty matched = 0;
    const Price lim = effective_limit(t);
    if (is_buy(t.side)) {
        while (t.leaves > 0) {
            auto it = asks_.begin();
            if (it == asks_.end() || !crosses(t.side, lim, it->first)) break;
            Level& lvl = it->second;
            while (t.leaves > 0 && !lvl.fifo.empty()) {
                Order& m = lvl.fifo.front();
                const Qty q = std::min(t.leaves, m.leaves);
                fills.push_back(Fill{t.id, m.id, m.account, t.account, it->first, q, t.arrival, Liquidity::Maker});
                m.leaves -= q;
                lvl.total -= q;
                t.leaves -= q;
                matched += q;
                if (m.leaves == 0) {
                    index_.erase(m.id);
                    lvl.fifo.pop_front();
                }
            }
            if (lvl.fifo.empty()) asks_.erase(it);
        }
    } else {
        while (t.leaves > 0) {
            auto it = bids_.begin();
            if (it == bids_.end() || !crosses(t.side, lim, it->first)) break;
            Level& lvl = it->second;
            while (t.leaves > 0 && !lvl.fifo.empty()) {
                Order& m = lvl.fifo.front();
                const Qty q = std::min(t.leaves, m.leaves);
                fills.push_back(Fill{t.id, m.id, m.account, t.account, it->first, q, t.arrival, Liquidity::Maker});
                m.leaves -= q;
                lvl.total -= q;
                t.leaves -= q;
                matched += q;
                if (m.leaves == 0) {
                    index_.erase(m.id);
                    lvl.fifo.pop_front();
                }
            }
            if (lvl.fifo.empty()) bids_.erase(it);
        }
    }
    return matched;
}

void RefBook::rest(Order o) {
    o.book_seq = next_book_seq_++;
    if (is_buy(o.side)) {
        Level& lvl = bids_[o.price];
        lvl.total += o.leaves;
        auto& lst = lvl.fifo;
        Order& stored = lst.emplace_back(std::move(o));
        index_[stored.id] = Loc{stored.side, stored.price, std::prev(lst.end())};
    } else {
        Level& lvl = asks_[o.price];
        lvl.total += o.leaves;
        auto& lst = lvl.fifo;
        Order& stored = lst.emplace_back(std::move(o));
        index_[stored.id] = Loc{stored.side, stored.price, std::prev(lst.end())};
    }
}

Status RefBook::add_order(const Order& o, std::vector<Fill>& fills, AddResult* out) {
    if (Status s = validate_order_entry(o, index_.count(o.id) != 0); !s) return s;
    const bool would_rest = o.type == OrderType::Limit && (o.tif == Tif::GTC || o.tif == Tif::GTD);
    if (would_rest && index_.size() >= opt_.max_orders) return fail(Errc::Capacity, o.id);

    Order t = o;
    t.leaves = o.qty;

    if (o.tif == Tif::FOK) {
        if (available_within(o.side, effective_limit(o)) < o.qty)
            return fail(Errc::InsufficientLiquidity, o.id);
    }

    const Qty matched = match_against(t, fills);
    Qty canceled = 0;
    if (t.leaves > 0) {
        if (o.type == OrderType::Market || o.tif == Tif::IOC || o.tif == Tif::FOK)
            canceled = t.leaves;
        else
            rest(std::move(t));
    }
    if (out) *out = AddResult{matched, canceled};
    return good();
}

Status RefBook::cancel(u64 order_id) {
    auto it = index_.find(order_id);
    if (it == index_.end()) return fail(Errc::NotFound, order_id);
    const Loc loc = it->second;
    if (is_buy(loc.side)) {
        auto lit = bids_.find(loc.px);
        lit->second.total -= loc.it->leaves;
        lit->second.fifo.erase(loc.it);
        if (lit->second.fifo.empty()) bids_.erase(lit);
    } else {
        auto lit = asks_.find(loc.px);
        lit->second.total -= loc.it->leaves;
        lit->second.fifo.erase(loc.it);
        if (lit->second.fifo.empty()) asks_.erase(lit);
    }
    index_.erase(it);
    return good();
}

Status RefBook::replace(u64 order_id, const Order& replacement, std::vector<Fill>& fills, AddResult* out) {
    if (replacement.id == order_id) return fail(Errc::InvalidArgument, replacement.id);
    if (Status s = cancel(order_id); !s) return s;
    return add_order(replacement, fills, out);
}

const Order* RefBook::find(u64 order_id) const noexcept {
    auto it = index_.find(order_id);
    if (it == index_.end()) return nullptr;
    return &*it->second.it;
}

Qty RefBook::total_bid_qty() const noexcept {
    Qty t = 0;
    for (const auto& [px, lvl] : bids_) t += lvl.total;
    return t;
}
Qty RefBook::total_ask_qty() const noexcept {
    Qty t = 0;
    for (const auto& [px, lvl] : asks_) t += lvl.total;
    return t;
}

LadderView RefBook::to_ladder() const {
    LadderView v;
    v.bids.reserve(bids_.size());
    for (const auto& [px, lvl] : bids_) {
        LadderLevel ll{px, lvl.total, {}};
        ll.orders.reserve(lvl.fifo.size());
        for (const auto& o : lvl.fifo) ll.orders.push_back(LadderOrder{o.id, o.leaves, o.qty});
        v.bids.push_back(std::move(ll));
    }
    v.asks.reserve(asks_.size());
    for (const auto& [px, lvl] : asks_) {
        LadderLevel ll{px, lvl.total, {}};
        ll.orders.reserve(lvl.fifo.size());
        for (const auto& o : lvl.fifo) ll.orders.push_back(LadderOrder{o.id, o.leaves, o.qty});
        v.asks.push_back(std::move(ll));
    }
    return v;
}

Status RefBook::rest_order(const Order& o) {
    if (index_.count(o.id)) return fail(Errc::Duplicate, o.id);
    if (o.qty == 0 || o.leaves == 0 || o.leaves > o.qty) return fail(Errc::OutOfRange, o.id);
    if (o.price < DomainBounds::kMinPrice || o.price > DomainBounds::kMaxPrice)
        return fail(Errc::OutOfRange, o.id);
    Order copy = o;
    copy.type = OrderType::Limit;
    rest(std::move(copy));
    return good();
}

}  // namespace arbiter