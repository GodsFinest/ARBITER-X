#pragma once
// Differential verification harness.
//
// The same seeded operation stream is applied to RefBook and OptBook. After
// every operation we compare: entry status, produced fills, best prices, and
// the canonical ladder hash. On the first divergence we record the operation
// index and a diagnosis. `minimize_ops` performs delta debugging to reduce a
// failing stream to a small reproducer (docs/VERIFICATION.md).

#include "arbiter/book_common.hpp"
#include "arbiter/opt_book.hpp"
#include "arbiter/ref_book.hpp"
#include "arbiter/rng.hpp"

#include <functional>
#include <string>
#include <vector>

namespace arbiter {
namespace diff {

struct Op {
    enum class Kind : u8 { Add = 0, Cancel = 1, Replace = 2 };
    Kind kind = Kind::Add;
    Order ord;       // Add: the order; Replace: the replacement
    u64 target = 0;  // Cancel / Replace: id of the order being canceled/replaced
};

struct Divergence {
    u64 op_index = 0;
    Op::Kind kind = Op::Kind::Add;
    std::string why;      // human-readable diagnosis
    u64 context = 0;      // order id / price / whatever localizes it
};

struct Options {
    OptBook::Options opt_book{1000, 2000, 1024};
    RefBook::Options ref_book{};
    bool stop_at_first_divergence = true;
};

struct Report {
    bool diverged = false;
    Divergence div{};
    u64 ops_applied = 0;
    u64 status_mismatches = 0, fill_mismatches = 0, ladder_mismatches = 0, best_mismatches = 0;
    u64 fills_total = 0;
};

// Apply one op to one book. `fills` is appended to; `res` receives outcomes.
Status apply_op(Op::Kind kind, const Order& ord, u64 target, class RefBook& b, std::vector<Fill>& fills,
                AddResult* res);
Status apply_op(Op::Kind kind, const Order& ord, u64 target, class OptBook& b, std::vector<Fill>& fills,
                AddResult* res);

// Full differential run.
Report run_differential(const std::vector<Op>& ops, const Options& opts = {});

// Seeded random op generator. `n_ops` operations; prices inside [1000, 2000];
// quantities and tif varied; cancels target plausibly-live ids.
std::vector<Op> random_ops(u64 seed, size_t n_ops);

// ---- adversarial search over the generator parameter space ----
// Adversarial generator knobs (all bounded): the classic random_ops
// distribution is the starting point; a hill-climb mutates these to find
// DIVERGENCE between the books. Any find is minimized and persisted to the
// corpus directory for regression locking.
struct GenParams {
    u32 add_rate_pct = 62;        // % of ops that are Adds (rest cancels/replaces)
    u32 limit_pct = 78;           // % of Adds that are Limit (rest Market)
    u32 gtc_pct = 60;             // Tif mix within limits (GTC share)
    u32 ioc_pct = 20;             // IOC share (FOK gets the rest of 90%)
    u32 qty_max = 30;             // quantity upper bound
    u32 price_span = 1001;        // price span above 1000 (denser books when small)
};

// Parameterized generator (same stream discipline as random_ops).
std::vector<Op> random_ops_tuned(u64 seed, size_t n_ops, const GenParams& p);

struct SearchResult {
    bool found = false;
    Divergence div{};
    u64 iterations = 0;
    u64 seeds_tried = 0;
    GenParams best_params{};
    std::vector<Op> minimized_reproducer;
};

// Hill-climb: mutate GenParams, run differentials, chase divergences.
// `corpus_dir` receives one minimizer-reduced op stream file per find
// (text format: one op per line; replayable by run_corpus()).
SearchResult adversarial_search(u64 base_seed, u32 iterations, u32 ops_per_run,
                                const std::string& corpus_dir);

// Replay + recheck every file in a corpus directory (regression lock).
// Returns (files checked, divergences reproduced).
std::pair<u64, u64> run_corpus(const std::string& corpus_dir);

// Serialize one op stream to the corpus text format.
std::string ops_to_text(const std::vector<Op>& ops);
// Parse the corpus text format back (Status error on malformed lines).
Result<std::vector<Op>> ops_from_text(const std::string& text);

// Delta-debugging minimizer: returns a subsequence that still satisfies
// `fails` (monotone predicate), typically much shorter than `ops`.
std::vector<Op> minimize_ops(const std::vector<Op>& ops,
                             const std::function<bool(const std::vector<Op>&)>& fails);

}  // namespace diff
}  // namespace arbiter