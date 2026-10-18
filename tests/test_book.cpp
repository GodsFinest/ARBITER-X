#include "testfw.hpp"

#include "arbiter/book_common.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/opt_book.hpp"
#include "arbiter/ref_book.hpp"

#include <string>
#include <vector>

using namespace arbiter;

namespace {

Order make_limit(u64 id, Side side, Price px, Qty qty, Tif tif = Tif::GTC, u32 account = 1) {
    Order o;
    o.id = id;
    o.account = account;
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = tif;
    o.price = px;
    o.qty = qty;
    o.leaves = qty;
    o.arrival = static_cast<LogicalNs>(id) * 100;
    return o;
}

Order make_market(u64 id, Side side, Qty qty, Tif tif = Tif::IOC, u32 account = 1) {
    Order o;
    o.id = id;
    o.account = account;
    o.side = side;
    o.type = OrderType::Market;
    o.tif = tif;
    o.price = 0;
    o.qty = qty;
    o.leaves = qty;
    o.arrival = static_cast<LogicalNs>(id) * 100;
    return o;
}

// Runs one scenario lambda against both books and compares ladders + fills.
template <typename Fn>
void both_books(const char* scenario, Fn&& fn) {
    RefBook ref{RefBook::Options{}};
    OptBook opt(OptBook::Options{1, 100000, 256});
    std::vector<Fill> fref, fopt;
    fn(ref, fref, 0);
    fn(opt, fopt, 1);
    CHECK(ref.state_hash() == opt.state_hash());
    CHECK(fref.size() == fopt.size());
    for (size_t i = 0; i < fref.size() && i < fopt.size(); ++i) {
        CHECK(fref[i].maker_order_id == fopt[i].maker_order_id);
        CHECK(fref[i].price == fopt[i].price);
        CHECK(fref[i].qty == fopt[i].qty);
        if (testfw::current_failed()) {
            std::printf("  (%s: fill %zu mismatch)\n", scenario, i);
            return;
        }
    }
    if (!testfw::current_failed()) {
        CHECK(check_book_invariants(ref.to_ladder()));
        CHECK(check_book_invariants(opt.to_ladder()));
    }
}

}  // namespace

AX_TEST(ref_basic_quoting_and_bests) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    CHECK(b.add_order(make_limit(1, Side::Buy, 99, 10), fills));
    CHECK(b.add_order(make_limit(2, Side::Buy, 100, 5), fills));
    CHECK(b.add_order(make_limit(3, Side::Sell, 102, 8), fills));
    CHECK(b.add_order(make_limit(4, Side::Sell, 101, 7), fills));
    CHECK_EQ(b.best_bid(), 100);
    CHECK_EQ(b.best_ask(), 101);
    CHECK_EQ(b.order_count(), 4u);
    CHECK(check_book_invariants(b.to_ladder()));
    LadderView v = b.to_ladder();
    CHECK_EQ(v.bids.size(), 2u);
    CHECK_EQ(v.asks.size(), 2u);
    CHECK_EQ(v.bids[0].price, 100);
    CHECK_EQ(v.bids[0].total_qty, 5);
    CHECK_EQ(v.asks[0].price, 101);
    CHECK_EQ(v.asks[0].total_qty, 7);
}

AX_TEST(ref_price_time_priority) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    CHECK(b.add_order(make_limit(1, Side::Sell, 100, 5), fills));
    CHECK(b.add_order(make_limit(2, Side::Sell, 100, 5), fills));
    CHECK(b.add_order(make_limit(3, Side::Sell, 99, 5), fills));  // better price, later
    CHECK(b.add_order(make_market(4, Side::Buy, 7, Tif::IOC), fills));
    // Sell@99 first (price priority), then Sell@100 first-in-time.
    CHECK_EQ(fills.size(), 2u);
    CHECK_EQ(fills[0].maker_order_id, 3u);
    CHECK_EQ(fills[0].qty, 5u);
    CHECK_EQ(fills[1].maker_order_id, 1u);
    CHECK_EQ(fills[1].qty, 2u);
    CHECK_EQ(b.best_ask(), 100);
    CHECK_EQ(b.find(1)->leaves, 3u);
}

AX_TEST(ref_partial_fill_and_rest) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    CHECK(b.add_order(make_limit(1, Side::Sell, 100, 10), fills));
    AddResult r;
    // small GTC buy crosses fully: fills 4, nothing rests
    CHECK(b.add_order(make_limit(2, Side::Buy, 100, 4, Tif::GTC), fills, &r));
    CHECK_EQ(r.filled, 4u);
    CHECK_EQ(r.canceled, 0u);
    CHECK_EQ(b.find(1)->leaves, 6u);
    CHECK_EQ(b.order_count(), 1u);
    // larger GTC buy exhausts the ask, remainder rests as the new best bid
    CHECK(b.add_order(make_limit(3, Side::Buy, 100, 15, Tif::GTC), fills, &r));
    CHECK_EQ(r.filled, 6u);
    CHECK_EQ(r.canceled, 0u);
    CHECK_EQ(b.best_bid(), 100);
    CHECK_EQ(b.best_ask(), 0);  // ask side empty
    CHECK_EQ(b.find(3)->leaves, 9u);
    CHECK_EQ(b.order_count(), 1u);
    CHECK(check_book_invariants(b.to_ladder()));
}

AX_TEST(ref_cancel_semantics) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    CHECK(b.add_order(make_limit(1, Side::Buy, 100, 10), fills));
    CHECK(b.add_order(make_limit(2, Side::Buy, 100, 10), fills));
    CHECK(b.add_order(make_limit(3, Side::Buy, 99, 10), fills));
    CHECK(b.cancel(1));
    CHECK(!b.has(1));
    CHECK_EQ(b.find(2)->leaves, 10u);
    CHECK_EQ(b.best_bid(), 100);
    CHECK(b.cancel(2));
    CHECK_EQ(b.best_bid(), 99);  // level emptied, best advanced
    CHECK(!b.cancel(999));       // unknown -> NotFound
    CHECK(check_book_invariants(b.to_ladder()));
}

AX_TEST(ref_ioc_cancels_remainder) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    CHECK(b.add_order(make_limit(1, Side::Sell, 100, 5), fills));
    AddResult r;
    CHECK(b.add_order(make_limit(2, Side::Buy, 100, 9, Tif::IOC), fills, &r));
    CHECK_EQ(r.filled, 5u);
    CHECK_EQ(r.canceled, 4u);
    CHECK_EQ(b.order_count(), 0u);  // nothing rested
}

AX_TEST(ref_fok_all_or_nothing_no_side_effects) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    CHECK(b.add_order(make_limit(1, Side::Sell, 100, 5), fills));
    CHECK(b.add_order(make_limit(2, Side::Sell, 101, 5), fills));
    const u64 hash_before = b.state_hash();
    CHECK(!b.add_order(make_limit(3, Side::Buy, 100, 8, Tif::FOK), fills));  // only 5 within limit
    CHECK_EQ(fills.size(), 0u);                                    // no fills recorded
    CHECK_EQ(b.state_hash(), hash_before);                          // book untouched
    CHECK_EQ(b.order_count(), 2u);

    // FOK that can fill within limit succeeds
    CHECK(b.add_order(make_limit(4, Side::Buy, 101, 10, Tif::FOK), fills));
    CHECK_EQ(fills.size(), 2u);
    CHECK_EQ(b.order_count(), 0u);
}

AX_TEST(ref_market_order_semantics) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    AddResult r;
    // empty book: market order cancels fully, no error
    CHECK(b.add_order(make_market(1, Side::Buy, 10, Tif::IOC), fills, &r));
    CHECK_EQ(r.filled, 0u);
    CHECK_EQ(r.canceled, 10u);
    // partial against book
    CHECK(b.add_order(make_limit(2, Side::Sell, 100, 4), fills));
    CHECK(b.add_order(make_limit(3, Side::Sell, 101, 4), fills));
    CHECK(b.add_order(make_market(4, Side::Buy, 10, Tif::IOC), fills, &r));
    CHECK_EQ(r.filled, 8u);
    CHECK_EQ(r.canceled, 2u);
    CHECK_EQ(b.order_count(), 0u);
    // market FOK on insufficient liquidity fails cleanly
    CHECK(b.add_order(make_limit(5, Side::Sell, 100, 3), fills));
    CHECK(!b.add_order(make_market(6, Side::Buy, 10, Tif::FOK), fills));
    CHECK_EQ(b.order_count(), 1u);
}

AX_TEST(ref_replace_loses_priority_and_validates) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    CHECK(b.add_order(make_limit(1, Side::Buy, 100, 10), fills));
    CHECK(b.add_order(make_limit(2, Side::Buy, 100, 10), fills));
    Order repl = make_limit(3, Side::Buy, 100, 10);
    CHECK(b.replace(1, repl, fills));
    // replaced order is at the back of the queue
    LadderView v = b.to_ladder();
    CHECK_EQ(v.bids[0].orders[0].id, 2u);
    CHECK_EQ(v.bids[0].orders[1].id, 3u);
    CHECK(!b.replace(999, repl, fills));          // unknown original
    CHECK(!b.replace(2, make_limit(2, Side::Buy, 100, 5), fills));  // same id rejected
}

AX_TEST(ref_validation_errors) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    CHECK_EQ(b.add_order(make_limit(0, Side::Buy, 100, 10), fills).code, Errc::OutOfRange);
    CHECK_EQ(b.add_order(make_limit(1, Side::Buy, 0, 10), fills).code, Errc::OutOfRange);
    CHECK_EQ(b.add_order(make_limit(2, Side::Buy, 100, 0), fills).code, Errc::OutOfRange);
    CHECK_EQ(b.add_order(make_limit(3, Side::Buy, 100, 10), fills).code, Errc::Ok);
    CHECK_EQ(b.add_order(make_limit(3, Side::Buy, 100, 10), fills).code, Errc::Duplicate);
    CHECK_EQ(b.add_order(make_market(4, Side::Buy, 10, Tif::IOC), fills).code, Errc::Ok);
    {
        Order bad_market = make_market(5, Side::Buy, 10, Tif::IOC);
        bad_market.price = 100;  // market orders must carry price=0
        CHECK_EQ(b.add_order(bad_market, fills).code, Errc::InvalidArgument);
    }
    // oversize qty rejected by domain bounds
    CHECK_EQ(b.add_order(make_limit(6, Side::Buy, 100, DomainBounds::kMaxQty + 1), fills).code,
             Errc::OutOfRange);
}

AX_TEST(opt_matches_ref_basic_scenarios) {
    // The same scripted scenarios run through both_books() must agree exactly.
    both_books("quoting", [](auto& b, auto& fills, int) {
        b.add_order(make_limit(1, Side::Buy, 99, 10), fills);
        b.add_order(make_limit(2, Side::Buy, 100, 5), fills);
        b.add_order(make_limit(3, Side::Sell, 102, 8), fills);
        b.add_order(make_limit(4, Side::Sell, 101, 7), fills);
    });
    both_books("priority", [](auto& b, auto& fills, int) {
        b.add_order(make_limit(1, Side::Sell, 100, 5), fills);
        b.add_order(make_limit(2, Side::Sell, 100, 5), fills);
        b.add_order(make_limit(3, Side::Sell, 99, 5), fills);
        b.add_order(make_market(4, Side::Buy, 7, Tif::IOC), fills);
    });
    both_books("ioc", [](auto& b, auto& fills, int) {
        b.add_order(make_limit(1, Side::Sell, 100, 5), fills);
        b.add_order(make_limit(2, Side::Buy, 100, 9, Tif::IOC), fills);
        b.add_order(make_limit(3, Side::Buy, 99, 5), fills);
        b.add_order(make_market(4, Side::Sell, 2, Tif::IOC), fills);
    });
    both_books("fok", [](auto& b, auto& fills, int) {
        b.add_order(make_limit(1, Side::Sell, 100, 5), fills);
        b.add_order(make_limit(2, Side::Sell, 101, 5), fills);
        b.add_order(make_limit(3, Side::Buy, 100, 8, Tif::FOK), fills);  // rejected
        b.add_order(make_limit(4, Side::Buy, 101, 10, Tif::FOK), fills); // fills both levels
    });
    both_books("cancel_best", [](auto& b, auto& fills, int) {
        b.add_order(make_limit(1, Side::Buy, 100, 10), fills);
        b.add_order(make_limit(2, Side::Buy, 99, 10), fills);
        b.add_order(make_limit(3, Side::Sell, 101, 10), fills);
        b.cancel(1);
        b.add_order(make_limit(4, Side::Buy, 101, 1), fills);  // crosses the now-best ask
        b.cancel(3);
    });
    both_books("replace", [](auto& b, auto& fills, int) {
        b.add_order(make_limit(1, Side::Buy, 100, 10), fills);
        b.add_order(make_limit(2, Side::Buy, 100, 10), fills);
        b.replace(1, make_limit(3, Side::Buy, 100, 10), fills);
        b.add_order(make_market(4, Side::Sell, 15, Tif::IOC), fills);
    });
    both_books("sweep_levels", [](auto& b, auto& fills, int) {
        b.add_order(make_limit(1, Side::Sell, 100, 3), fills);
        b.add_order(make_limit(2, Side::Sell, 101, 3), fills);
        b.add_order(make_limit(3, Side::Sell, 102, 3), fills);
        b.add_order(make_limit(4, Side::Buy, 99, 5), fills);
        b.add_order(make_market(5, Side::Buy, 10, Tif::IOC), fills);  // sweeps all asks
        b.add_order(make_limit(6, Side::Buy, 105, 5), fills);         // rests as best bid
    });
}

AX_TEST(opt_best_tracking_edge_cases) {
    OptBook b{OptBook::Options{1, 100000, 64}};
    std::vector<Fill> fills;
    CHECK_EQ(b.best_bid(), 0);
    CHECK_EQ(b.best_ask(), 0);
    CHECK(b.add_order(make_limit(1, Side::Buy, 50000, 1), fills));
    CHECK_EQ(b.best_bid(), 50000);
    CHECK(b.cancel(1));
    CHECK_EQ(b.best_bid(), 0);  // side empty again
    // re-anchor after emptying
    CHECK(b.add_order(make_limit(2, Side::Buy, 40000, 1), fills));
    CHECK(b.add_order(make_limit(3, Side::Buy, 60000, 1), fills));
    CHECK_EQ(b.best_bid(), 60000);
    CHECK(b.cancel(3));
    CHECK_EQ(b.best_bid(), 40000);
    // out-of-span price rejected
    CHECK_EQ(b.add_order(make_limit(4, Side::Buy, 200000, 1), fills).code, Errc::OutOfRange);
    CHECK(check_book_invariants(b.to_ladder()));
}

AX_TEST(main_invariants_hold_after_ops) {
    RefBook b{RefBook::Options{}};
    std::vector<Fill> fills;
    for (u64 i = 1; i <= 50; ++i) {
        Side s = (i % 2) ? Side::Buy : Side::Sell;
        Price p = 95 + static_cast<Price>(i % 11);
        CHECK(b.add_order(make_limit(i, s, p, 1 + i % 7), fills));
        if (i % 5 == 0) b.cancel(i - 1);
        if (!testfw::current_failed()) CHECK(check_book_invariants(b.to_ladder()));
    }
    // no crossing despite aggressive two-sided flow
    CHECK(b.best_bid() < b.best_ask());
}

AX_TEST_MAIN