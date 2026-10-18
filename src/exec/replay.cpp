#include "arbiter/replay.hpp"
#include "arbiter/book_common.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/opt_book.hpp"
#include "arbiter/ref_book.hpp"

#include <memory>
#include <vector>

namespace arbiter {

namespace {

// Replayer interface so replay works over either book implementation.
struct Replayer {
    virtual ~Replayer() = default;
    virtual Status add(const Order& o, std::vector<Fill>& fills) = 0;
    virtual Status cancel(u64 id) = 0;
    virtual Status replace(u64 orig, const Order& repl, std::vector<Fill>& fills) = 0;
    virtual Status rest_order(const Order& o) = 0;
    virtual u64 state_hash() const = 0;
};

struct RefReplayer final : Replayer {
    RefBook b;
    explicit RefReplayer(const RefBook::Options& o) : b(o) {}
    Status add(const Order& o, std::vector<Fill>& f) override { return b.add_order(o, f, nullptr); }
    Status cancel(u64 id) override { return b.cancel(id); }
    Status replace(u64 orig, const Order& repl, std::vector<Fill>& f) override {
        return b.replace(orig, repl, f, nullptr);
    }
    Status rest_order(const Order& o) override { return b.rest_order(o); }
    u64 state_hash() const override { return b.state_hash(); }
};

struct OptReplayer final : Replayer {
    OptBook b;
    explicit OptReplayer(const OptBook::Options& o) : b(o) {}
    Status add(const Order& o, std::vector<Fill>& f) override { return b.add_order(o, f, nullptr); }
    Status cancel(u64 id) override { return b.cancel(id); }
    Status replace(u64 orig, const Order& repl, std::vector<Fill>& f) override {
        return b.replace(orig, repl, f, nullptr);
    }
    Status rest_order(const Order& o) override { return b.rest_order(o); }
    u64 state_hash() const override { return b.state_hash(); }
};

Order order_from_add_event(const Event& e) {
    Order o;
    o.id = e.a;
    o.account = e.account;
    o.side = e.side;
    o.type = e.otype;
    o.tif = e.tif;
    o.price = e.p;
    o.qty = e.c;
    o.leaves = e.c;
    o.arrival = e.t;
    o.expiry = e.b;  // absolute logical expiry (GTD; carried since v? in slot b)
    return o;
}

// BookReplace layout: a = original id, b = replacement id, c = qty, p = price.
Order order_from_replace_event(const Event& e) {
    Order o = order_from_add_event(e);
    o.id = e.b;
    // The fixed-slot event has no room to carry a GTD expiry through a
    // replace (slot b is the replacement id). Replayed GTD replacements
    // would rest with expiry 0 forever, reject loudly instead.
    o.expiry = 0;
    return o;
}

// Rest every order of a canonical snapshot ladder (bids then asks, FIFO within
// levels) so the restored book preserves price-time priority and exact
// leaves/qty (partial fills included). A corrupt snapshot (duplicate ids /
// invalid entries) must fail the replay loudly rather than silently skipping orders.
Status restore_ladder(Replayer& rp, const LadderView& ladder, std::vector<Fill>& scratch) {
    const auto side_run = [&](const std::vector<LadderLevel>& levels, Side s) -> Status {
        for (const auto& lvl : levels) {
            for (const auto& lo : lvl.orders) {
                Order o;
                o.id = lo.id;
                o.side = s;
                o.type = OrderType::Limit;
                o.tif = Tif::GTC;
                o.price = lvl.price;
                o.qty = lo.qty;
                o.leaves = lo.leaves;
                scratch.clear();
                if (Status st = rp.rest_order(o); !st) return st;
            }
        }
        return good();
    };
    if (Status st = side_run(ladder.bids, Side::Buy); !st) return st;
    return side_run(ladder.asks, Side::Sell);
}

}  // namespace

ReplayResult replay_journal(const std::string& path, BookImpl impl, u64 until_seq,
                            const SnapshotData* base) {
    ReplayResult rep;
    std::unique_ptr<Replayer> replayer;
    std::vector<Fill> fills;

    if (impl == BookImpl::Reference)
        replayer = std::make_unique<RefReplayer>(RefBook::Options{});
    else
        replayer = std::make_unique<OptReplayer>(OptBook::Options{1, 1'000'000, 4096});

    u64 skip_through_seq = 0;
    if (base) {
        skip_through_seq = base->last_event_seq;
        rep.logical_t = base->logical_t;
        if (Status st = restore_ladder(*replayer, base->ladder, fills); !st) {
            rep.status = st;
            return rep;
        }
    }

    auto rd = JournalReader::open(path);
    if (!rd) {
        rep.status = Status{Errc::NotFound, rd.status.context};
        return rep;
    }
    JournalReader& r = rd.value;

    while (r.has_next()) {
        auto evr = r.next();
        if (!evr) {
            rep.status = evr.status;
            break;
        }
        const Event& e = evr.value;
        rep.events_seen++;
        if (until_seq != 0 && e.seq > until_seq) break;
        if (e.seq <= skip_through_seq) continue;
        rep.final_seq = e.seq;
        rep.logical_t = e.t;

        if (e.type == EvType::BookAdd || e.type == EvType::BookReplace) {
            fills.clear();
            Order target;
            if (e.type == EvType::BookReplace) {
                target = order_from_replace_event(e);
                if (target.tif == Tif::GTD) {
                    // fixed-slot events cannot carry a GTD expiry through a
                    // replace; the reconstructed order would silently rest
                    // forever, reject the journal loudly instead
                    rep.status = fail(Errc::ProtocolInvalid, e.seq);
                    break;
                }
            } else {
                target = order_from_add_event(e);
            }
            const Status s = (e.type == EvType::BookAdd)
                                 ? replayer->add(target, fills)
                                 : replayer->replace(e.a, target, fills);
            // Some entry outcomes are legitimate, deterministic behavior that
            // replay reproduces exactly: FOK rejections (no side effects) and
            // cancel/replace of an order that already filled. Anything else is
            // a replay failure.
            const bool tolerated = !s && (s.code == Errc::InsufficientLiquidity ||
                                          s.code == Errc::NotFound);
            if (!s && !tolerated) {
                rep.status = s;
                break;
            }
            if (!s) ++rep.entry_outcomes_reproduced;
            rep.events_applied++;
            rep.fills_recomputed += fills.size();
            // Cross-check the K recomputed fills against the K journaled
            // TradePrints that immediately follow this input event.
            for (const Fill& f : fills) {
                auto pr = r.next();
                if (!pr) {
                    rep.trade_mismatches++;
                    rep.status = Status{Errc::JournalCorrupt, 0};
                    break;
                }
                rep.events_seen++;
                rep.final_seq = pr.value.seq;
                if (pr.value.type != EvType::TradePrint || pr.value.a != f.taker_order_id ||
                    pr.value.b != f.maker_order_id || pr.value.c != f.qty || pr.value.p != f.price) {
                    rep.trade_mismatches++;
                }
            }
            if (rep.trade_mismatches > 0) {
                if (!rep.status) rep.status = Status{Errc::Internal, rep.trade_mismatches};
                break;
            }
        } else if (e.type == EvType::BookCancel) {
            const Status s = replayer->cancel(e.a);
            if (!s && s.code != Errc::NotFound) {  // NotFound reproduces a cancel of an already-filled order
                rep.status = s;
                break;
            }
            rep.events_applied++;
        } else if (e.type == EvType::Checkpoint) {
            // Verification point journaled during the original run: the
            // recomputed hash must match the recorded one.
            if (e.b != replayer->state_hash()) rep.checkpoint_mismatches++;
        }
        // Other event types (Md*, Timer, Risk, Fault, Session, Halt, orphan
        // TradePrints) carry no book input here; counted and skipped.
    }

    rep.state_hash = replayer->state_hash();
    if (rep.status || rep.trade_mismatches || rep.checkpoint_mismatches) {
        if (!rep.status) rep.status = fail(Errc::Internal, rep.trade_mismatches + rep.checkpoint_mismatches);
    } else {
        rep.status = good();
    }
    return rep;
}

}  // namespace arbiter