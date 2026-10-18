#include "arbiter/opt_book.hpp"
#include "arbiter/book_common.hpp"
#include "arbiter/errors.hpp"

#include <algorithm>

namespace arbiter {

OptBook::OptBook(const Options& opt) : opt_(opt) {
    const u64 span = static_cast<u64>(opt_.max_tick) - static_cast<u64>(opt_.min_tick) + 1;
    bids_.resize(span);
    asks_.resize(span);
    pool_.reserve(opt_.reserve_orders);
    index_.reserve(opt_.reserve_orders * 2);
}

u32 OptBook::alloc_node(const Order& o) noexcept {
    if (free_head_ != kNone) {
        const u32 idx = free_head_;
        free_head_ = pool_[idx].next;
        pool_[idx] = Node{o, kNone, kNone};
        return idx;
    }
    // packed Loc stores the node index in 24 bits: refuse to grow beyond
    // the representable range instead of corrupting the index silently.
    if (pool_.size() >= 0xFFFFFFu) return kNone;
    pool_.push_back(Node{o, kNone, kNone});
    return static_cast<u32>(pool_.size() - 1);
}

void OptBook::free_node(u32 idx) noexcept {
    pool_[idx].next = free_head_;
    free_head_ = idx;
}

void OptBook::attach(Side s, u32 off, u32 nidx) noexcept {
    Level& lvl = level_for(s, off);
    Node& n = pool_[nidx];
    n.prev = lvl.tail;
    n.next = kNone;
    if (lvl.head == kNone)
        lvl.head = nidx;
    else
        pool_[lvl.tail].next = nidx;
    lvl.tail = nidx;
    lvl.count++;
    lvl.total += n.ord.leaves;
    const Price p = n.ord.price;
    if (is_buy(s)) {
        if (p > best_bid_) best_bid_ = p;
    } else {
        if (best_ask_ == kNoAsk || p < best_ask_) best_ask_ = p;
    }
}

void OptBook::detach(Side s, u32 off, u32 nidx) noexcept {
    Level& lvl = level_for(s, off);
    Node& n = pool_[nidx];
    if (n.prev != kNone)
        pool_[n.prev].next = n.next;
    else
        lvl.head = n.next;
    if (n.next != kNone)
        pool_[n.next].prev = n.prev;
    else
        lvl.tail = n.prev;
    lvl.count--;
    lvl.total -= n.ord.leaves;
    if (lvl.count == 0) {
        const Price p = price_for(s, off);
        if (is_buy(s) && p == best_bid_) recompute_best_after_empty(s, off);
        if (!is_buy(s) && p == best_ask_) recompute_best_after_empty(s, off);
    }
}

void OptBook::recompute_best_after_empty(Side s, u32 emptied_off) noexcept {
    const auto& arr = is_buy(s) ? bids_ : asks_;
    const u32 span = static_cast<u32>(arr.size());
    Price best = is_buy(s) ? kNoBid : kNoAsk;
    for (u32 i = emptied_off; i < span; ++i) {
        if (arr[i].count != 0) {
            best = price_for(s, i);
            break;
        }
    }
    if (is_buy(s))
        best_bid_ = best;
    else
        best_ask_ = best;
}

Qty OptBook::match_against(Order& t, std::vector<Fill>& fills) {
    Qty matched = 0;
    const Price lim = (t.type == OrderType::Limit) ? t.price
                                                   : (is_buy(t.side) ? DomainBounds::kMaxPrice : 0);
    if (is_buy(t.side)) {
        while (t.leaves > 0) {
            if (best_ask_ == kNoAsk || !crosses(t.side, lim, best_ask_)) break;
            const Price lvl_price = best_ask_;
            const u32 off = offset_for(Side::Sell, lvl_price);
            Level& lvl = asks_[off];
            while (t.leaves > 0 && lvl.head != kNone) {
                const u32 midx = lvl.head;
                Order& m = pool_[midx].ord;
                const Qty q = std::min(t.leaves, m.leaves);
                fills.push_back(Fill{t.id, m.id, m.account, t.account, lvl_price, q, t.arrival, Liquidity::Maker});
                m.leaves -= q;
                lvl.total -= q;
                t.leaves -= q;
                matched += q;
                if (m.leaves == 0) {
                    lvl.head = pool_[midx].next;
                    if (lvl.head != kNone)
                        pool_[lvl.head].prev = kNone;
                    else
                        lvl.tail = kNone;
                    lvl.count--;
                    index_.erase(m.id);
                    free_node(midx);
                }
            }
            if (lvl.count == 0) recompute_best_after_empty(Side::Sell, off);
        }
    } else {
        while (t.leaves > 0) {
            if (best_bid_ == kNoBid || !crosses(t.side, lim, best_bid_)) break;
            const Price lvl_price = best_bid_;
            const u32 off = offset_for(Side::Buy, lvl_price);
            Level& lvl = bids_[off];
            while (t.leaves > 0 && lvl.head != kNone) {
                const u32 midx = lvl.head;
                Order& m = pool_[midx].ord;
                const Qty q = std::min(t.leaves, m.leaves);
                fills.push_back(Fill{t.id, m.id, m.account, t.account, lvl_price, q, t.arrival, Liquidity::Maker});
                m.leaves -= q;
                lvl.total -= q;
                t.leaves -= q;
                matched += q;
                if (m.leaves == 0) {
                    lvl.head = pool_[midx].next;
                    if (lvl.head != kNone)
                        pool_[lvl.head].prev = kNone;
                    else
                        lvl.tail = kNone;
                    lvl.count--;
                    index_.erase(m.id);
                    free_node(midx);
                }
            }
            if (lvl.count == 0) recompute_best_after_empty(Side::Buy, off);
        }
    }
    return matched;
}

Qty OptBook::available_within(Side taker_side, Price limit_price) const noexcept {
    Qty total = 0;
    if (is_buy(taker_side)) {
        const u32 span = static_cast<u32>(asks_.size());
        for (u32 off = 0; off < span; ++off) {
            const Price p = price_for(Side::Sell, off);
            if (p > limit_price) break;
            if (asks_[off].count) total += asks_[off].total;
        }
    } else {
        const u32 span = static_cast<u32>(bids_.size());
        for (u32 off = 0; off < span; ++off) {
            const Price p = price_for(Side::Buy, off);
            if (p < limit_price) break;
            if (bids_[off].count) total += bids_[off].total;
        }
    }
    return total;
}

Status OptBook::rest(Order o) {
    o.book_seq = next_book_seq_++;
    const u32 off = offset_for(o.side, o.price);
    const u32 nidx = alloc_node(o);
    if (nidx == kNone) return fail(Errc::Capacity, o.id);  // pool exhausted the 24-bit index space
    attach(o.side, off, nidx);
    index_.insert(o.id, pack_loc(o.side, off, nidx));
    return good();
}

Status OptBook::add_order(const Order& o, std::vector<Fill>& fills, AddResult* out) {
    if (o.type == OrderType::Limit &&
        (o.price < opt_.min_tick || o.price > opt_.max_tick))
        return fail(Errc::OutOfRange, o.id);
    if (Status s = validate_order_entry(o, index_.contains(o.id)); !s) return s;

    Order t = o;
    t.leaves = o.qty;

    if (o.tif == Tif::FOK) {
        const Price lim = (o.type == OrderType::Limit) ? o.price
                                                       : (is_buy(o.side) ? DomainBounds::kMaxPrice : 0);
        if (available_within(o.side, lim) < o.qty) return fail(Errc::InsufficientLiquidity, o.id);
    }

    const Qty matched = match_against(t, fills);
    Qty canceled = 0;
    if (t.leaves > 0) {
        if (o.type == OrderType::Market || o.tif == Tif::IOC || o.tif == Tif::FOK)
            canceled = t.leaves;
        else if (Status s = rest(std::move(t)); !s)
            return s;  // fills already produced: matching happened; the rest failed
    }
    if (out) *out = AddResult{matched, canceled};
    return good();
}

Status OptBook::cancel(u64 order_id) {
    const u64* packed = index_.find(order_id);
    if (!packed) return fail(Errc::NotFound, order_id);
    const Side s = loc_side(*packed);
    const u32 off = loc_off(*packed);
    const u32 node = loc_node(*packed);
    detach(s, off, node);
    index_.erase(order_id);
    free_node(node);
    return good();
}

Status OptBook::replace(u64 order_id, const Order& replacement, std::vector<Fill>& fills, AddResult* out) {
    if (replacement.id == order_id) return fail(Errc::InvalidArgument, replacement.id);
    if (Status s = cancel(order_id); !s) return s;
    return add_order(replacement, fills, out);
}

const Order* OptBook::find(u64 order_id) const noexcept {
    const u64* packed = index_.find(order_id);
    if (!packed) return nullptr;
    return &pool_[loc_node(*packed)].ord;
}

Qty OptBook::total_bid_qty() const noexcept {
    Qty t = 0;
    for (const auto& lvl : bids_) t += lvl.total;
    return t;
}
Qty OptBook::total_ask_qty() const noexcept {
    Qty t = 0;
    for (const auto& lvl : asks_) t += lvl.total;
    return t;
}

size_t OptBook::level_count() const noexcept {
    size_t n = 0;
    for (const auto& lvl : bids_) n += (lvl.count != 0);
    for (const auto& lvl : asks_) n += (lvl.count != 0);
    return n;
}

std::vector<std::pair<u64, u32>> OptBook::order_accounts() const {
    std::vector<std::pair<u64, u32>> out;
    out.reserve(index_.size());
    index_.for_each([&](u64 id, u64 packed) {
        out.emplace_back(id, pool_[loc_node(packed)].ord.account);
    });
    std::sort(out.begin(), out.end());
    return out;
}

void OptBook::set_order_account(u64 id, u32 account) noexcept {
    const u64* packed = index_.find(id);
    if (packed) pool_[loc_node(*packed)].ord.account = account;
}

Qty OptBook::qty_at(Side s, Price p) const noexcept {
    if (p < opt_.min_tick || p > opt_.max_tick) return 0;
    const u32 off = offset_for(s, p);
    const Level& lvl = level_for(s, off);
    return lvl.count ? lvl.total : 0;
}

Qty OptBook::consume_at(Side s, Price p, Qty qty) noexcept {
    if (qty == 0 || p < opt_.min_tick || p > opt_.max_tick) return 0;
    const u32 off = offset_for(s, p);
    Level& lvl = level_for(s, off);
    Qty consumed = 0;
    while (consumed < qty && lvl.head != kNone) {
        const u32 hidx = lvl.head;
        Order& m = pool_[hidx].ord;
        const Qty q = std::min(qty - consumed, m.leaves);
        m.leaves -= q;
        lvl.total -= q;
        consumed += q;
        if (m.leaves == 0) {
            lvl.head = pool_[hidx].next;
            if (lvl.head != kNone)
                pool_[lvl.head].prev = kNone;
            else
                lvl.tail = kNone;
            lvl.count--;
            index_.erase(m.id);
            free_node(hidx);
        }
    }
    // The cached best must advance when consumption empties the best level,
    // otherwise best_bid()/best_ask() report stale (or crossed) prices. This
    // bug was invisible to ladder-hash differential testing (caches are not
    // hashed) and surfaced via the world restore contract.
    if (lvl.count == 0) {
        const Price pp = price_for(s, off);
        if (is_buy(s) && pp == best_bid_) recompute_best_after_empty(s, off);
        if (!is_buy(s) && pp == best_ask_) recompute_best_after_empty(s, off);
    }
    return consumed;
}

LadderView OptBook::to_ladder() const {
    LadderView v;
    for (const auto& lvl : bids_) {
        if (lvl.count == 0) continue;
        LadderLevel ll{price_for(Side::Buy, static_cast<u32>(&lvl - bids_.data())), lvl.total, {}};
        ll.orders.reserve(lvl.count);
        for (u32 n = lvl.head; n != kNone; n = pool_[n].next) {
            const Order& o = pool_[n].ord;
            ll.orders.push_back(LadderOrder{o.id, o.leaves, o.qty});
        }
        v.bids.push_back(std::move(ll));
    }
    for (const auto& lvl : asks_) {
        if (lvl.count == 0) continue;
        LadderLevel ll{price_for(Side::Sell, static_cast<u32>(&lvl - asks_.data())), lvl.total, {}};
        ll.orders.reserve(lvl.count);
        for (u32 n = lvl.head; n != kNone; n = pool_[n].next) {
            const Order& o = pool_[n].ord;
            ll.orders.push_back(LadderOrder{o.id, o.leaves, o.qty});
        }
        v.asks.push_back(std::move(ll));
    }
    return v;
}

Status OptBook::rest_order(const Order& o) {
    if (index_.contains(o.id)) return fail(Errc::Duplicate, o.id);
    if (o.qty == 0 || o.leaves == 0 || o.leaves > o.qty) return fail(Errc::OutOfRange, o.id);
    if (o.price < opt_.min_tick || o.price > opt_.max_tick) return fail(Errc::OutOfRange, o.id);
    Order copy = o;
    copy.type = OrderType::Limit;
    return rest(std::move(copy));
}

}  // namespace arbiter