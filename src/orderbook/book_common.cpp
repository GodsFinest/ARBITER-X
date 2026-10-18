#include "arbiter/book_common.hpp"

#include <unordered_set>

namespace arbiter {

void canonical_book_hash(const LadderView& v, Hasher& h) {
    h.tag("ARBITERX/BOOK/1");
    h.u64v(v.bids.size());
    for (const auto& lvl : v.bids) {
        h.i64v(lvl.price);
        h.u64v(lvl.total_qty);
        h.u64v(lvl.orders.size());
        for (const auto& o : lvl.orders) {
            h.u64v(o.id);
            h.u64v(o.leaves);
            h.u64v(o.qty);
        }
    }
    h.u64v(v.asks.size());
    for (const auto& lvl : v.asks) {
        h.i64v(lvl.price);
        h.u64v(lvl.total_qty);
        h.u64v(lvl.orders.size());
        for (const auto& o : lvl.orders) {
            h.u64v(o.id);
            h.u64v(o.leaves);
            h.u64v(o.qty);
        }
    }
}

u64 canonical_book_hash(const LadderView& v) {
    Hasher h;
    canonical_book_hash(v, h);
    return h.value();
}

Status check_book_invariants(const LadderView& v) {
    std::unordered_set<u64> ids;
    ids.reserve(v.bids.size() * 4 + v.asks.size() * 4);

    auto check_side = [&](const std::vector<LadderLevel>& levels, bool descending) -> Status {
        for (size_t i = 0; i < levels.size(); ++i) {
            const auto& lvl = levels[i];
            if (lvl.orders.empty()) return fail(Errc::Internal, static_cast<u64>(lvl.price));
            if (i > 0) {
                Price prev = levels[i - 1].price;
                bool ordered = descending ? (lvl.price < prev) : (lvl.price > prev);
                if (!ordered) return fail(Errc::Internal, static_cast<u64>(lvl.price));
            }
            Qty sum = 0;
            for (const auto& o : lvl.orders) {
                if (o.leaves == 0 || o.leaves > o.qty) return fail(Errc::Internal, o.id);
                if (!ids.insert(o.id).second) return fail(Errc::Duplicate, o.id);
                sum += o.leaves;
            }
            if (sum != lvl.total_qty) return fail(Errc::Internal, static_cast<u64>(lvl.price));
        }
        return good();
    };

    if (Status s = check_side(v.bids, /*descending=*/true); !s) return s;
    if (Status s = check_side(v.asks, /*descending=*/false); !s) return s;

    if (!v.bids.empty() && !v.asks.empty() && v.bids.front().price >= v.asks.front().price)
        return fail(Errc::Internal, static_cast<u64>(v.bids.front().price));
    return good();
}

void encode_ladder(ByteWriter& w, const LadderView& v) {
    auto side = [&](const std::vector<LadderLevel>& levels) {
        w.put_u64(levels.size());
        for (const auto& lvl : levels) {
            w.put_i64(lvl.price);
            w.put_u64(lvl.total_qty);
            w.put_u64(lvl.orders.size());
            for (const auto& o : lvl.orders) {
                w.put_u64(o.id);
                w.put_u64(o.leaves);
                w.put_u64(o.qty);
            }
        }
    };
    side(v.bids);
    side(v.asks);
}

bool decode_ladder(ByteReader& r, LadderView& v) {
    auto side = [&](std::vector<LadderLevel>& out) -> bool {
        const u64 n = r.get_u64();
        if (!r.ok() || n > (1u << 20)) return false;
        out.reserve(static_cast<size_t>(n));
        for (u64 i = 0; i < n; ++i) {
            LadderLevel lvl;
            lvl.price = r.get_i64();
            lvl.total_qty = r.get_u64();
            const u64 orders = r.get_u64();
            if (!r.ok() || orders > (1u << 24)) return false;
            lvl.orders.reserve(static_cast<size_t>(orders));
            for (u64 j = 0; j < orders; ++j) {
                LadderOrder o;
                o.id = r.get_u64();
                o.leaves = r.get_u64();
                o.qty = r.get_u64();
                if (!r.ok()) return false;
                lvl.orders.push_back(o);
            }
            out.push_back(std::move(lvl));
        }
        return true;
    };
    return side(v.bids) && side(v.asks);
}

Status validate_order_entry(const Order& o, bool duplicate_id) {
    if (o.id == 0 || o.id > DomainBounds::kMaxOrderId) return fail(Errc::OutOfRange, o.id);
    if (o.qty == 0 || o.qty > DomainBounds::kMaxQty) return fail(Errc::OutOfRange, o.id);
    if (duplicate_id) return fail(Errc::Duplicate, o.id);
    if (o.type == OrderType::Limit &&
        (o.price < DomainBounds::kMinPrice || o.price > DomainBounds::kMaxPrice))
        return fail(Errc::OutOfRange, o.id);
    if (o.type == OrderType::Market && o.price != 0) return fail(Errc::InvalidArgument, o.id);
    // GTD must carry an absolute expiry (> 0); expiry 0 would rest forever
    // (the engine schedules expiry only when expiry > arrival).
    if (o.tif == Tif::GTD && o.expiry <= 0) return fail(Errc::InvalidArgument, o.id);
    return good();
}

}  // namespace arbiter