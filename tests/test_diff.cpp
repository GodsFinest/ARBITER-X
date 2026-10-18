#include "testfw.hpp"

#include "arbiter/book_common.hpp"
#include "arbiter/differential.hpp"
#include "arbiter/errors.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

using namespace arbiter;

AX_TEST(diff_property_random_streams_agree) {
    // Property: reference and optimized books are behaviorally identical over
    // randomized mixed workloads, compared after EVERY operation.
    for (u64 seed = 1; seed <= 40; ++seed) {
        auto ops = diff::random_ops(seed, 3000);
        diff::Report rep = diff::run_differential(ops);
        if (rep.diverged) {
            std::printf("  seed %llu diverged at op %llu (%s): %s\n",
                        static_cast<unsigned long long>(seed),
                        static_cast<unsigned long long>(rep.div.op_index),
                        rep.div.why.c_str(), to_cstr(static_cast<Errc>(0)));
            testfw::current_failed() = true;
            return;
        }
        CHECK_EQ(rep.status_mismatches, 0u);
        CHECK_EQ(rep.fill_mismatches, 0u);
        CHECK_EQ(rep.ladder_mismatches, 0u);
        CHECK_EQ(rep.best_mismatches, 0u);
        CHECK_EQ(rep.ops_applied, 3000u);
    }
}

AX_TEST(diff_long_stream_larger_workload) {
    // One longer run with a different op-mix seed domain.
    auto ops = diff::random_ops(123456789ull, 50000);
    diff::Report rep = diff::run_differential(ops);
    CHECK(!rep.diverged);
    if (rep.diverged)
        std::printf("  divergence at op %llu: %s\n",
                    static_cast<unsigned long long>(rep.div.op_index), rep.div.why.c_str());
}

AX_TEST(diff_invariants_hold_on_both_books_after_random_ops) {
    // Independent of differential agreement: each book individually keeps its
    // structural invariants after arbitrary op streams.
    for (u64 seed : {1ull, 7ull, 31ull, 999ull}) {
        auto ops = diff::random_ops(seed, 2000);
        RefBook ref{RefBook::Options{}};
        OptBook opt{OptBook::Options{1000, 2000, 1024}};
        std::vector<Fill> fills;
        for (const auto& op : ops) {
            diff::apply_op(op.kind, op.ord, op.target, ref, fills, nullptr);
            fills.clear();
            diff::apply_op(op.kind, op.ord, op.target, opt, fills, nullptr);
            fills.clear();
            Status sr = check_book_invariants(ref.to_ladder());
            Status so = check_book_invariants(opt.to_ladder());
            if (!sr || !so) {
                std::printf("  seed %llu invariant violation: ref=%s opt=%s\n",
                            static_cast<unsigned long long>(seed), to_cstr(sr.code), to_cstr(so.code));
                testfw::current_failed() = true;
                return;
            }
        }
    }
}

AX_TEST(minimizer_reduces_failing_stream) {
    // Synthetic monotone failure: fails iff ops #17 and #250 are both present.
    auto ops = diff::random_ops(424242ull, 1000);
    auto fails = [&ops](const std::vector<diff::Op>& candidate) {
        bool has17 = false, has250 = false;
        for (const auto& op : candidate) {
            if (op.kind == ops[17].kind && op.ord.id == ops[17].ord.id && op.target == ops[17].target &&
                op.ord.qty == ops[17].ord.qty && op.ord.price == ops[17].ord.price)
                has17 = true;
            if (op.kind == ops[250].kind && op.ord.id == ops[250].ord.id && op.target == ops[250].target &&
                op.ord.qty == ops[250].ord.qty && op.ord.price == ops[250].ord.price)
                has250 = true;
        }
        return has17 && has250;
    };
    auto minimal = diff::minimize_ops(ops, fails);
    CHECK(!minimal.empty());
    CHECK(minimal.size() <= 4);  // only the two load-bearing ops should survive (plus possible dup hits)
    CHECK(fails(minimal));
}

AX_TEST(minimizer_rejects_non_failing_input) {
    auto ops = diff::random_ops(1, 200);
    auto minimal = diff::minimize_ops(ops, [](const std::vector<diff::Op>&) { return false; });
    CHECK(minimal.empty());
}

AX_TEST(corpus_text_format_roundtrips) {
    // The persistent corpus format (ops_to_text/ops_from_text) must
    // roundtrip exactly, it is the regression-lock medium for adversarial
    // finds.
    auto ops = diff::random_ops_tuned(777, 500, diff::GenParams{});
    const std::string text = diff::ops_to_text(ops);
    auto back = diff::ops_from_text(text);
    CHECK(back);
    if (!back) return;
    CHECK_EQ(back.value.size(), ops.size());
    for (size_t i = 0; i < ops.size(); ++i) {
        CHECK_EQ(back.value[i].kind, ops[i].kind);
        CHECK_EQ(back.value[i].ord.id, ops[i].ord.id);
        CHECK_EQ(back.value[i].ord.account, ops[i].ord.account);
        CHECK_EQ(back.value[i].ord.side, ops[i].ord.side);
        CHECK_EQ(back.value[i].ord.type, ops[i].ord.type);
        CHECK_EQ(back.value[i].ord.tif, ops[i].ord.tif);
        CHECK_EQ(back.value[i].ord.price, ops[i].ord.price);
        CHECK_EQ(back.value[i].ord.qty, ops[i].ord.qty);
        CHECK_EQ(back.value[i].target, ops[i].target);
        if (testfw::current_failed()) return;
    }
    // corrupted line must be a loud parse failure
    const std::string bad = text;
    const size_t nl = bad.find('\n');
    auto badr = diff::ops_from_text("X 1 1 0 0 0 1000 5 0\n");
    CHECK(!badr);
    (void)nl;
    // tuned generator with default params == classic distribution shape
    // (same stream seed discipline: NOT identical draws, but the classic
    // distribution must still produce zero divergence)
    for (u64 seed : {5ull, 6ull, 7ull}) {
        auto tops = diff::random_ops_tuned(seed, 3000, diff::GenParams{});
        diff::Report rep = diff::run_differential(tops);
        CHECK(!rep.diverged);
    }
}

AX_TEST(corpus_replays_clean) {
    // The corpus is the regression lock for adversarial finds. Entries must
    // PARSE and run deterministically; divergence count must be 0 (all known
    // finds fixed). The directory itself is part of the contract: it must
    // EXIST (an accidentally-deleted corpus must fail loudly, not pass
    // vacuously). AX_SOURCE_DIR (CMake) resolves the repo root from any
    // working directory.
    const std::string dir = std::string(AX_SOURCE_DIR) + "/tests/corpus";
    CHECK(std::filesystem::exists(dir));
    if (!std::filesystem::exists(dir)) return;
    const auto [checked, diverged] = diff::run_corpus(dir);
    std::printf("  [corpus dir=%s] %llu files, %llu divergences (all fixed finds stay fixed)\n",
                dir.c_str(), (unsigned long long)checked, (unsigned long long)diverged);
    CHECK_EQ(diverged, 0u);
}

AX_TEST_MAIN