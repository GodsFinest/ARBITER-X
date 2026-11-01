#include "testfw.hpp"

#include "arbiter/book_common.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/opt_book.hpp"
#include "arbiter/ref_book.hpp"

#include <stdexcept>
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

AX_TEST(rejected_replace_is_atomic_in_both_books) {
    auto exercise = [](auto& b) {
        std::vector<Fill> fills;
        CHECK(b.add_order(make_limit(1, Side::Buy, 100, 10), fills));
        CHECK(b.add_order(make_limit(2, Side::Buy, 99, 10), fills));
        const u64 before = b.state_hash();
        Order invalid = make_limit(3, Side::Buy, 100, 0);
        CHECK(!b.replace(1, invalid, fills));
        CHECK_EQ(b.state_hash(), before);
        CHECK(b.has(1));
        CHECK_EQ(fills.size(), 0u);
        CHECK(!b.replace(1, make_limit(2, Side::Buy, 100, 5), fills));
        CHECK_EQ(b.state_hash(), before);
        CHECK(b.has(1));
    };
    RefBook ref{RefBook::Options{}};
    OptBook opt{OptBook::Options{1, 1000, 16}};
    exercise(ref);
    exercise(opt);
}

AX_TEST(ref_capacity_allows_fully_executable_gtc) {
    RefBook b{RefBook::Options{1}};
    std::vector<Fill> fills;
    CHECK(b.add_order(make_limit(1, Side::Sell, 100, 5), fills));
    CHECK(b.add_order(make_limit(2, Side::Buy, 100, 5), fills));
    CHECK_EQ(fills.size(), 1u);
    CHECK_EQ(b.order_count(), 0u);
}

AX_TEST(restored_orders_use_full_entry_validation) {
    RefBook ref{RefBook::Options{}};
    OptBook opt{OptBook::Options{1, 1000, 16}};
    Order bad = make_limit(0, Side::Buy, 100, 1);
    CHECK(!ref.rest_order(bad));
    CHECK(!opt.rest_order(bad));
    bad = make_limit(9, Side::Buy, 100, DomainBounds::kMaxQty + 1);
    CHECK(!ref.rest_order(bad));
    CHECK(!opt.rest_order(bad));
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


AX_TEST(invalid_enums_rejected_in_both_books) {
    auto exercise = [](auto& b) {
        std::vector<Fill> fills;
        Order bad = make_limit(1, Side::Buy, 100, 5);
        bad.side = static_cast<Side>(0x7F);
        CHECK(!b.add_order(bad, fills));
        bad = make_limit(1, Side::Buy, 100, 5);
        bad.type = static_cast<OrderType>(0x55);
        CHECK(!b.add_order(bad, fills));
        bad = make_limit(1, Side::Buy, 100, 5);
        bad.tif = static_cast<Tif>(0x99);
        CHECK(!b.add_order(bad, fills));
        CHECK_EQ(fills.size(), 0u);
    };
    RefBook ref{RefBook::Options{}};
    OptBook opt{OptBook::Options{1, 1000, 16}};
    exercise(ref);
    exercise(opt);
}

AX_TEST(gtd_expiry_at_or_before_arrival_rejected) {
    RefBook ref{RefBook::Options{}};
    OptBook opt{OptBook::Options{1, 1000, 16}};
    std::vector<Fill> fills;
    Order gtd = make_limit(1, Side::Buy, 100, 5, Tif::GTD);
    gtd.arrival = 1000;
    gtd.expiry = 1000;  // == arrival: the engine would rest it forever
    CHECK(!ref.add_order(gtd, fills));
    CHECK(!opt.add_order(gtd, fills));
    gtd.expiry = 500;  // < arrival
    CHECK(!ref.add_order(gtd, fills));
    CHECK(!opt.add_order(gtd, fills));
    gtd.expiry = 1500;  // > arrival: valid
    CHECK(ref.add_order(gtd, fills));
    std::vector<Fill> fills2;
    Order ok = make_limit(2, Side::Buy, 100, 5, Tif::GTD);
    ok.arrival = 1000;
    ok.expiry = 1500;
    CHECK(opt.add_order(ok, fills2));
}

AX_TEST(out_of_span_replacement_preserves_original) {
    OptBook opt{OptBook::Options{1, 1000, 16}};
    std::vector<Fill> fills;
    CHECK(opt.add_order(make_limit(1, Side::Buy, 500, 10), fills));
    const u64 before = opt.state_hash();
    CHECK(!opt.replace(1, make_limit(2, Side::Buy, 2000, 5), fills));  // outside span
    CHECK_EQ(opt.state_hash(), before);
    CHECK(opt.has(1));
    CHECK_EQ(fills.size(), 0u);
}

AX_TEST(fok_replacement_excludes_original_contra_liquidity) {
    auto exercise = [](auto& b) {
        std::vector<Fill> fills;
        // ask 1 (the original): 5 lots @100; ask 2: 10 lots @100
        CHECK(b.add_order(make_limit(1, Side::Sell, 100, 5), fills));
        CHECK(b.add_order(make_limit(2, Side::Sell, 100, 10), fills));
        const u64 before = b.state_hash();
        // FOK buy of 12: 15 lots sit on the ask side, but the original's 5
        // are contra liquidity the replacement would REMOVE -> only 10 are
        // truly available. A naive implementation sees 15 and proceeds,
        // destroying the original on a FOK that cannot fill.
        CHECK(!b.replace(1, make_limit(3, Side::Buy, 100, 12, Tif::FOK), fills));
        CHECK_EQ(b.state_hash(), before);
        CHECK(b.has(1));
        CHECK_EQ(fills.size(), 0u);
        // FOK buy of 10: exactly the OTHER order's liquidity -> the
        // replacement proceeds and fills 10 (never 15). A fully-filling FOK
        // never rests, so neither order remains.
        CHECK(b.replace(1, make_limit(3, Side::Buy, 100, 10, Tif::FOK), fills));
        CHECK_EQ(fills.size(), 1u);
        CHECK_EQ(fills.back().qty, 10u);
        CHECK(!b.has(1));
        CHECK(!b.has(3));
    };
    RefBook ref{RefBook::Options{}};
    OptBook opt{OptBook::Options{1, 1000, 16}};
    exercise(ref);
    exercise(opt);
}

AX_TEST(capacity_rejection_zero_fills_both_books) {
    // RefBook: one-slot cap. The incoming order would rest and cannot fully
    // execute: rejected BEFORE matching (zero fills, zero mutation).
    RefBook ref{RefBook::Options{1}};
    std::vector<Fill> fills;
    CHECK(ref.add_order(make_limit(1, Side::Buy, 100, 5), fills));
    const u64 before = ref.state_hash();
    CHECK(!ref.add_order(make_limit(2, Side::Buy, 99, 5), fills));
    CHECK_EQ(ref.state_hash(), before);
    CHECK_EQ(ref.order_count(), 1u);
    CHECK_EQ(fills.size(), 0u);

    // OptBook: same active-order cap semantics (Options field 4).
    OptBook opt{OptBook::Options{1, 1000, 16, 1}};
    std::vector<Fill> f2;
    CHECK(opt.add_order(make_limit(1, Side::Buy, 100, 5), f2));
    const u64 before2 = opt.state_hash();
    CHECK(!opt.add_order(make_limit(2, Side::Buy, 99, 5), f2));
    CHECK_EQ(opt.state_hash(), before2);
    CHECK_EQ(opt.order_count(), 1u);
    CHECK_EQ(f2.size(), 0u);
    // A fully executable order needs no resting slot: admitted at capacity.
    // The IOC sell of 2 crosses the resting bid (5) partially and cancels
    // its remainder, so the bid survives with 3 leaves.
    CHECK(opt.add_order(make_market(3, Side::Sell, 2, Tif::IOC), f2));
    CHECK_EQ(f2.size(), 1u);
    CHECK_EQ(opt.order_count(), 1u);
    const Order* bid = opt.find(1);
    CHECK(bid);
    CHECK_EQ(bid->leaves, 3u);
}

AX_TEST(opt_book_invalid_span_throws_before_allocation) {
    auto expect_throw = [](OptBook::Options o) {
        bool threw = false;
        try {
            OptBook bad{o};
            (void)bad;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    };
    expect_throw(OptBook::Options{500, 100, 16});  // reversed span
    expect_throw(OptBook::Options{0, 1000, 16});   // below the price domain
    expect_throw(OptBook::Options{1, 1'000'001, 16});  // prevent an unbounded ladder allocation
}

AX_TEST(opt_book_reserve_preserves_index) {
    // A tiny reserve forces repeated rehashes. Every existing id must remain
    // reachable while the contiguous ladder keeps all resting orders.
    OptBook book{OptBook::Options{1, 100, 1}};
    std::vector<Fill> fills;
    for (u64 id = 1; id <= 12; ++id)
        CHECK(book.add_order(make_limit(id, Side::Buy, 10 + static_cast<Price>(id), 1), fills));
    CHECK_EQ(book.order_count(), 12u);
    for (u64 id = 1; id <= 12; ++id) {
        CHECK(book.has(id));
        CHECK(book.find(id) != nullptr);
    }
    CHECK(check_book_invariants(book.to_ladder()));
    for (u64 id = 1; id <= 12; ++id) CHECK(book.cancel(id));
    CHECK_EQ(book.order_count(), 0u);
}


AX_TEST(rejected_add_leaves_caller_fill_vector_and_seq_untouched) {
    auto exercise = [](auto& b) {
        std::vector<Fill> fills;
        CHECK(b.add_order(make_limit(1, Side::Sell, 100, 5), fills));
        // simulate caller-owned fills from an earlier operation
        fills.push_back(Fill{9, 9, 1, 2, 100, 1, 0, Liquidity::Maker, Side::Buy, Side::Sell});
        const u64 before_hash = b.state_hash();
        const u64 before_seq = b.next_book_seq();
        const size_t before_fills = fills.size();
        // every normal failure path must append nothing and touch nothing
        CHECK(!b.add_order(make_limit(1, Side::Buy, 100, 5), fills));    // duplicate id
        Order bad_enum = make_limit(3, Side::Buy, 100, 5);
        bad_enum.tif = static_cast<Tif>(0x77);
        CHECK(!b.add_order(bad_enum, fills));                            // invalid enum
        Order bad_gtd = make_limit(3, Side::Buy, 100, 5, Tif::GTD);
        bad_gtd.arrival = 1000;
        bad_gtd.expiry = 1000;
        CHECK(!b.add_order(bad_gtd, fills));                             // expired GTD
        CHECK(!b.add_order(make_limit(3, Side::Buy, 100, 6, Tif::FOK), fills));  // FOK miss (6 > 5)
        CHECK_EQ(b.state_hash(), before_hash);
        CHECK_EQ(b.next_book_seq(), before_seq);
        CHECK_EQ(fills.size(), before_fills);
        CHECK_EQ(fills.back().taker_order_id, 9u);  // caller prefix intact
    };
    RefBook ref{RefBook::Options{}};
    OptBook opt{OptBook::Options{1, 1000, 16}};
    exercise(ref);
    exercise(opt);
    // out-of-span replacement is an OptBook-specific failure; the original
    // must survive it identically
    std::vector<Fill> fills;
    CHECK(opt.add_order(make_limit(21, Side::Sell, 100, 5), fills));
    const u64 h = opt.state_hash();
    const u64 sq = opt.next_book_seq();
    CHECK(!opt.replace(21, make_limit(22, Side::Buy, 5000, 5), fills));
    CHECK_EQ(opt.state_hash(), h);
    CHECK_EQ(opt.next_book_seq(), sq);
    CHECK(opt.has(21));
}

AX_TEST(capacity_rejection_holds_even_when_matching_would_free_slots) {
    // Documented conservative contract (identical in both books): at the
    // active-order cap, a resting entry that cannot fully execute is
    // rejected BEFORE matching — maker slots the match would free are not
    // credited. Zero fills, zero mutation.
    auto exercise = [](auto& b) {
        std::vector<Fill> fills;
        CHECK(b.add_order(make_limit(1, Side::Sell, 100, 10), fills));
        CHECK(b.add_order(make_limit(2, Side::Sell, 100, 10), fills));
        const u64 before_hash = b.state_hash();
        const u64 before_seq = b.next_book_seq();
        CHECK(!b.add_order(make_limit(3, Side::Buy, 100, 25), fills));  // 20 < 25, at cap
        CHECK_EQ(b.state_hash(), before_hash);
        CHECK_EQ(b.next_book_seq(), before_seq);
        CHECK_EQ(fills.size(), 0u);
        CHECK_EQ(b.order_count(), 2u);
        // below the cap the same order is admitted: 20 matched, 5 rest
        // (cap 3 via a fresh book)
    };
    RefBook ref{RefBook::Options{2}};
    OptBook opt{OptBook::Options{1, 1000, 16, 2}};
    exercise(ref);
    exercise(opt);
}

AX_TEST(partial_cross_rests_its_remainder_below_the_cap) {
    auto exercise = [](auto& b) {
        std::vector<Fill> fills;
        CHECK(b.add_order(make_limit(1, Side::Sell, 100, 10), fills));
        CHECK(b.add_order(make_limit(2, Side::Sell, 100, 10), fills));
        // 25 @100: available 20, remainder 5 must rest; book has room (cap 3)
        CHECK(b.add_order(make_limit(3, Side::Buy, 100, 25), fills));
        CHECK_EQ(fills.size(), 2u);       // one per maker
        CHECK_EQ(fills[0].qty, 10u);
        CHECK_EQ(fills[1].qty, 10u);
        CHECK_EQ(b.order_count(), 1u);    // both makers consumed; remainder rests
        const Order* rem = b.find(3);
        CHECK(rem);
        CHECK_EQ(rem->leaves, 5u);
        CHECK_EQ(b.best_bid(), 100u);
    };
    RefBook ref{RefBook::Options{3}};
    OptBook opt{OptBook::Options{1, 1000, 16, 3}};
    exercise(ref);
    exercise(opt);
}

AX_TEST(fully_executable_gtc_admitted_at_capacity_in_both_books) {
    auto exercise = [](auto& b) {
        std::vector<Fill> fills;
        CHECK(b.add_order(make_limit(1, Side::Sell, 100, 5), fills));
        CHECK(b.add_order(make_limit(2, Side::Sell, 100, 5), fills));
        // at cap (2), but 10 <= available 10: no resting slot needed
        CHECK(b.add_order(make_limit(3, Side::Buy, 100, 10), fills));
        CHECK_EQ(fills.size(), 2u);
        CHECK_EQ(b.order_count(), 0u);  // fully consumed, nothing rested
    };
    RefBook ref{RefBook::Options{2}};
    OptBook opt{OptBook::Options{1, 1000, 16, 2}};
    exercise(ref);
    exercise(opt);
}

AX_TEST(invalid_replacement_preserves_original_seq_and_prefix_fills) {
    auto exercise = [](auto& b) {
        std::vector<Fill> fills;
        CHECK(b.add_order(make_limit(1, Side::Sell, 100, 5), fills));
        fills.push_back(Fill{9, 9, 1, 2, 100, 1, 0, Liquidity::Maker, Side::Buy, Side::Sell});
        const u64 before_hash = b.state_hash();
        const u64 before_seq = b.next_book_seq();
        const size_t before_fills = fills.size();
        Order dup = make_limit(1, Side::Buy, 100, 5);                     // duplicate replacement id
        CHECK(!b.replace(1, dup, fills));
        Order bad_enum = make_limit(2, Side::Buy, 100, 5);
        bad_enum.tif = static_cast<Tif>(0x77);
        CHECK(!b.replace(1, bad_enum, fills));                            // invalid enum
        Order expired = make_limit(2, Side::Buy, 100, 5, Tif::GTD);
        expired.arrival = 1000;
        expired.expiry = 1000;
        CHECK(!b.replace(1, expired, fills));                             // expired GTD
        CHECK(!b.replace(1, make_limit(2, Side::Buy, 100, 5, Tif::FOK), fills));  // no contra liquidity
        CHECK_EQ(b.state_hash(), before_hash);
        CHECK_EQ(b.next_book_seq(), before_seq);
        CHECK_EQ(fills.size(), before_fills);
        CHECK(b.has(1));  // the original is intact
    };
    RefBook ref{RefBook::Options{}};
    OptBook opt{OptBook::Options{1, 1000, 16}};
    exercise(ref);
    exercise(opt);
}

AX_TEST(replacement_partially_crossing_at_capacity_is_transactional) {
    // The original rests (frees a slot when canceled) and the replacement
    // must rest a remainder. Resources are secured BEFORE the cancel, so the
    // replacement commits fully. Cancel-then-new semantics: the original's
    // liquidity is removed first, so the replacement matches only the OTHER
    // ask (10 of 25) and rests 15.
    auto exercise = [](auto& b) {
        std::vector<Fill> fills;
        CHECK(b.add_order(make_limit(1, Side::Sell, 100, 10), fills));
        CHECK(b.add_order(make_limit(2, Side::Sell, 100, 10), fills));
        CHECK(b.replace(1, make_limit(3, Side::Buy, 100, 25), fills));
        CHECK_EQ(fills.size(), 1u);
        CHECK_EQ(fills.back().qty, 10u);
        CHECK(!b.has(1));
        const Order* rem = b.find(3);
        CHECK(rem);
        CHECK_EQ(rem->leaves, 15u);
        CHECK_EQ(b.order_count(), 1u);  // the other ask was fully consumed
    };
    RefBook ref{RefBook::Options{2}};
    OptBook opt{OptBook::Options{1, 1000, 16, 2}};
    exercise(ref);
    exercise(opt);
}

AX_TEST(opt_book_index_growth_boundary_keeps_state_exact) {
    // Drive FlatIdMap across its load-factor growth threshold inside the
    // preflight path: the book must stay exactly equivalent to RefBook for
    // the same op sequence.
    RefBook ref{RefBook::Options{}};
    OptBook opt{OptBook::Options{1, 100000, 4096, 2'000'000}};
    std::vector<Fill> fref, fopt;
    u64 id = 1;
    for (; id <= 5900; ++id) {  // crosses the 0.7 load factor of the 8192 table
        CHECK(ref.add_order(make_limit(id, Side::Buy, 100 + static_cast<Price>(id % 50), 1), fref));
        CHECK(opt.add_order(make_limit(id, Side::Buy, 100 + static_cast<Price>(id % 50), 1), fopt));
    }
    CHECK_EQ(ref.state_hash(), opt.state_hash());
    CHECK_EQ(ref.next_book_seq(), opt.next_book_seq());
    // churn: cancel a band, re-add, verify the index survives growth+deletion
    for (u64 k = 1; k <= 500; ++k) {
        CHECK(ref.cancel(k * 7));
        CHECK(opt.cancel(k * 7));
    }
    for (u64 k = 1; k <= 500; ++k) {
        const u64 nid = 10000 + k;
        CHECK(ref.add_order(make_limit(nid, Side::Buy, 300, 1), fref));
        CHECK(opt.add_order(make_limit(nid, Side::Buy, 300, 1), fopt));
    }
    CHECK_EQ(ref.state_hash(), opt.state_hash());
    CHECK_EQ(fref.size(), fopt.size());
}

AX_TEST_MAIN
