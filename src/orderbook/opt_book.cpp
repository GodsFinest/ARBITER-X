#include "arbiter/opt_book.hpp"
#include "arbiter/book_common.hpp"
#include "arbiter/errors.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace arbiter {

OptBook::OptBook(const Options& opt) : opt_(opt) {
    if (opt_.min_tick < DomainBounds::kMinPrice || opt_.max_tick < opt_.min_tick ||
        static_cast<u64>(opt_.max_tick) - static_cast<u64>(opt_.min_tick) >=
            static_cast<u64>(0xFFFFFFFFu))
        throw std::invalid_argument("invalid OptBook price span");
    const u64 span = static_cast<u64>(opt_.max_tick) - static_cast<u64>(opt_.min_tick) + 1;
    if (span > kMaxLadderTicks)
        throw std::invalid_argument("OptBook price span exceeds the supported ladder");
    if (opt_.reserve_orders > std::numeric_limits<size_t>::max() / 2)
        throw std::invalid_argument("OptBook reserve_orders overflows index capacity");
    bids_.resize(span);
    asks_.resize(span);
    pool_.reserve(opt_.reserve_orders);
    index_.reserve(opt_.reserve_orders * 2);
}

Status OptBook::prepare_rest_resources(u64 order_id) {
    // One index slot: ensure load-factor headroom now so the commit phase's
    // insert_prechecked cannot grow (FlatIdMap::reserve may throw — again
    // before any logical book mutation). Do this first: if it throws, the
    // node pool/free-list must remain untouched as well.
    index_.reserve(index_.size() + 1);

    // One pool node: reuse the free list when possible; otherwise grow the
    // pool NOW (may throw bad_alloc — before the node becomes reachable from
    // the free-list). The index has already been prepared, so no operation
    // that follows can throw during the commit phase.
    if (free_head_ == kNone) {
        // packed Loc stores the node index in 24 bits: refuse growth beyond
        // the representable range as a normal, side-effect-free Capacity
        // failure instead of corrupting the index silently.
        if (pool_.size() >= 0xFFFFFFu) return fail(Errc::Capacity, order_id);
        pool_.emplace_back();
        free_head_ = static_cast<u32>(pool_.size() - 1);
        pool_[free_head_].next = kNone;
    }
    return good();
}

u32 OptBook::alloc_node(const Order& o) noexcept {
    // Precondition (prepare_rest_resources): the free list is non-empty.
    const u32 idx = free_head_;
    free_head_ = pool_[idx].next;
    pool_[idx] = Node{o, kNone, kNone};
    return idx;
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

#ifndef NDEBUG
void OptBook::assert_best_cache(Side side, u32 from_off) const noexcept {
    if (is_buy(side)) {
        Price expected = kNoBid;
        for (u32 off = from_off; off < bids_.size(); ++off) {
            if (bids_[off].count != 0) {
                expected = price_for(Side::Buy, off);
                break;
            }
        }
        assert(best_bid_ == expected);
    } else {
        Price expected = kNoAsk;
        for (u32 off = from_off; off < asks_.size(); ++off) {
            if (asks_[off].count != 0) {
                expected = price_for(Side::Sell, off);
                break;
            }
        }
        assert(best_ask_ == expected);
    }
}
#endif

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
    const Side maker_side = is_buy(taker_side) ? Side::Sell : Side::Buy;
    const Price best = is_buy(taker_side) ? best_ask_ : best_bid_;
    if (best == kNoAsk || best == kNoBid) return 0;

    // The raw span is only used to reject a non-crossing limit.  A market
    // order uses a sentinel limit, so its raw span is meaningless for path
    // selection; compare the clamped ladder range with the index capacity.
    const u64 level_span = is_buy(taker_side)
                               ? (best > limit_price ? 0u : static_cast<u64>(limit_price - best) + 1u)
                               : (best < limit_price ? 0u : static_cast<u64>(best - limit_price) + 1u);
    if (level_span == 0) return 0;

    const Price bounded_limit = is_buy(taker_side) ? std::min(limit_price, opt_.max_tick)
                                                    : std::max(limit_price, opt_.min_tick);
    const u32 begin = offset_for(maker_side, best);
    const u32 end = offset_for(maker_side, bounded_limit);
    const u64 scan_len = static_cast<u64>(end - begin) + 1u;
    if (scan_len > index_.capacity()) {
        Qty total = 0;
        index_.for_each([&](u64, u64 packed) {
            if (loc_side(packed) != maker_side) return;
            const Price p = price_for(maker_side, loc_off(packed));
            if ((is_buy(taker_side) && p <= limit_price) ||
                (!is_buy(taker_side) && p >= limit_price))
                total += pool_[loc_node(packed)].ord.leaves;
        });
        return total;
    }

    Qty total = 0;
    for (u32 off = begin;; ++off) {
        total += level_for(maker_side, off).total;
        if (off == end) break;
    }
    return total;
}

// Infallible rest: the caller ran prepare_rest_resources(), so alloc_node
// and the index upsert cannot fail or allocate. A failure returned by
// add_order therefore can never carry fills or a partially mutated book.
void OptBook::rest(Order o) {
    o.book_seq = next_book_seq_++;
    const u32 off = offset_for(o.side, o.price);
    const u32 nidx = alloc_node(o);
    attach(o.side, off, nidx);
    index_.insert_prechecked(o.id, pack_loc(o.side, off, nidx));
}

Status OptBook::add_order(const Order& o, std::vector<Fill>& fills, AddResult* out) {
    if (o.type == OrderType::Limit &&
        (o.price < opt_.min_tick || o.price > opt_.max_tick))
        return fail(Errc::OutOfRange, o.id);
    if (Status s = validate_order_entry(o, index_.contains(o.id)); !s) return s;

    Order t = o;
    t.leaves = o.qty;

    const Price lim = (o.type == OrderType::Limit) ? o.price
                                                   : (is_buy(o.side) ? DomainBounds::kMaxPrice : 0);
    const Qty available = available_within(o.side, lim);
    const bool would_rest = o.type == OrderType::Limit && (o.tif == Tif::GTC || o.tif == Tif::GTD);
    const bool partially_executable = available < o.qty;
    // CONSERVATIVE active-order cap (aligned with RefBook, documented): a
    // resting entry that will not fully execute needs one slot beyond the
    // current live count; at the cap it is rejected BEFORE matching. Maker
    // slots that matching would free are intentionally NOT credited — the
    // projected-remainder contract is simpler and identical in both books.
    if (would_rest && partially_executable && index_.size() >= opt_.max_orders)
        return fail(Errc::Capacity, o.id);
    // FOK stays fully all-or-nothing.
    if (o.tif == Tif::FOK) {
        if (partially_executable) return fail(Errc::InsufficientLiquidity, o.id);
    }

    // Commit-phase infallibility: secure the node + index resources a resting
    // remainder needs BEFORE matching mutates anything. The projection is
    // exact: match_against consumes exactly min(qty, available) (both walk
    // the same crossing levels), so a remainder exists iff
    // partially_executable. A throw from the growth itself escapes BEFORE
    // any mutation; the Capacity return is a normal side-effect-free failure.
    if (would_rest && partially_executable)
        if (Status s = prepare_rest_resources(o.id); !s) return s;

    const Qty matched = match_against(t, fills);
    Qty canceled = 0;
    if (t.leaves > 0) {
        if (o.type == OrderType::Market || o.tif == Tif::IOC || o.tif == Tif::FOK)
            canceled = t.leaves;
        else
            rest(std::move(t));  // infallible: resources were preflighted
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
    const Price price = price_for(s, off);
    const bool was_best = is_buy(s) ? price == best_bid_ : price == best_ask_;
    detach(s, off, node);
    index_.erase(order_id);
    free_node(node);
    if (was_best && level_for(s, off).count == 0) assert_best_cache(s, off);
    return good();
}

Status OptBook::replace(u64 order_id, const Order& replacement, std::vector<Fill>& fills, AddResult* out) {
    if (replacement.id == order_id) return fail(Errc::InvalidArgument, replacement.id);
    const Order* original = find(order_id);
    if (!original) return fail(Errc::NotFound, order_id);
    if (replacement.type == OrderType::Limit &&
        (replacement.price < opt_.min_tick || replacement.price > opt_.max_tick))
        return fail(Errc::OutOfRange, replacement.id);
    if (Status s = validate_order_entry(replacement, index_.contains(replacement.id)); !s) return s;
    const Price lim = replacement.type == OrderType::Limit
                          ? replacement.price
                          : (is_buy(replacement.side) ? DomainBounds::kMaxPrice : 0);
    Qty available = available_within(replacement.side, lim);
    const bool replacement_rests =
        replacement.type == OrderType::Limit &&
        (replacement.tif == Tif::GTC || replacement.tif == Tif::GTD);
    if (replacement.tif == Tif::FOK) {
        // The original's own contra liquidity is removed by the replacement,
        // so it is excluded from the availability projection.
        if (original->side != replacement.side && crosses(replacement.side, lim, original->price))
            available = available > original->leaves ? available - original->leaves : 0;
        if (available < replacement.qty)
            return fail(Errc::InsufficientLiquidity, replacement.id);
    }
    // Everything that can fail normally is settled BEFORE the original is
    // touched. The cancel below frees one resting slot, so the conservative
    // active-order cap cannot fail post-cancel; the node/index resources a
    // resting remainder needs are secured here, pre-cancel, keeping the
    // replacement transactional (the original survives any rejection).
    if (replacement_rests && available < replacement.qty)
        if (Status s = prepare_rest_resources(replacement.id); !s) return s;
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
        const bool was_best = is_buy(s) ? pp == best_bid_ : pp == best_ask_;
        if (was_best) {
            recompute_best_after_empty(s, off);
            assert_best_cache(s, off);
        }
    }
    return consumed;
}

LadderView OptBook::to_ladder() const {
    // Do not scan the whole bounded price span here.  A normal OptBook can
    // cover millions of ticks while holding only a handful of orders; a
    // full ladder scan made every state hash proportional to that configured
    // span and turned replay/snapshot validation into an accidental timeout
    // under sanitizers.  The id index already contains exactly the live
    // nodes, so materialize those nodes and order them by the same invariants
    // as the intrusive FIFO: price level first, then exchange book sequence.
    // `book_seq` is assigned monotonically by rest(); the id tie-break keeps
    // the result deterministic even for a manually reconstructed order with
    // a duplicate sequence.
    struct LiveOrder {
        Side side;
        u32 off;
        u64 book_seq;
        u64 id;
        const Order* order;
    };

    std::vector<LiveOrder> live;
    live.reserve(index_.size());
    index_.for_each([&](u64 id, u64 packed) {
        const Side side = loc_side(packed);
        const u32 off = loc_off(packed);
        const Order* order = &pool_[loc_node(packed)].ord;
        live.push_back(LiveOrder{side, off, order->book_seq, id, order});
    });

    const auto less = [](const LiveOrder& lhs, const LiveOrder& rhs) noexcept {
        if (lhs.side != rhs.side) return is_buy(lhs.side) > is_buy(rhs.side);
        if (lhs.off != rhs.off) return lhs.off < rhs.off;
        if (lhs.book_seq != rhs.book_seq) return lhs.book_seq < rhs.book_seq;
        return lhs.id < rhs.id;
    };
    std::sort(live.begin(), live.end(), less);

    LadderView v;
    size_t i = 0;
    while (i < live.size()) {
        const Side side = live[i].side;
        const u32 off = live[i].off;
        const Price price = price_for(side, off);
        LadderLevel level{price, 0, {}};
        while (i < live.size() && live[i].side == side && live[i].off == off) {
            const Order& order = *live[i].order;
            level.total_qty += order.leaves;
            level.orders.push_back(LadderOrder{order.id, order.leaves, order.qty});
            ++i;
        }
        if (is_buy(side))
            v.bids.push_back(std::move(level));
        else
            v.asks.push_back(std::move(level));
    }
    return v;
}

Status OptBook::rest_order(const Order& o) {
    if (Status s = validate_order_entry(o, index_.contains(o.id)); !s) return s;
    if (o.qty == 0 || o.leaves == 0 || o.leaves > o.qty) return fail(Errc::OutOfRange, o.id);
    if (o.price < opt_.min_tick || o.price > opt_.max_tick) return fail(Errc::OutOfRange, o.id);
    // One node + one index slot, secured before any mutation.
    if (Status s = prepare_rest_resources(o.id); !s) return s;
    Order copy = o;
    copy.type = OrderType::Limit;
    rest(std::move(copy));
    return good();
}

}  // namespace arbiter

