#include "arbiter/differential.hpp"
#include "arbiter/errors.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <unordered_set>

namespace arbiter {
namespace diff {

Status apply_op(Op::Kind kind, const Order& ord, u64 target, RefBook& b, std::vector<Fill>& fills,
                AddResult* res) {
    switch (kind) {
        case Op::Kind::Add: return b.add_order(ord, fills, res);
        case Op::Kind::Cancel: return b.cancel(target);
        case Op::Kind::Replace: return b.replace(target, ord, fills, res);
    }
    return fail(Errc::Internal);
}

Status apply_op(Op::Kind kind, const Order& ord, u64 target, OptBook& b, std::vector<Fill>& fills,
                AddResult* res) {
    switch (kind) {
        case Op::Kind::Add: return b.add_order(ord, fills, res);
        case Op::Kind::Cancel: return b.cancel(target);
        case Op::Kind::Replace: return b.replace(target, ord, fills, res);
    }
    return fail(Errc::Internal);
}

namespace {

bool fills_equal(const std::vector<Fill>& a, const std::vector<Fill>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].maker_order_id != b[i].maker_order_id || a[i].price != b[i].price ||
            a[i].qty != b[i].qty || a[i].maker_account != b[i].maker_account)
            return false;
    }
    return true;
}


}  // namespace

Report run_differential(const std::vector<Op>& ops, const Options& opts) {
    Report rep;
    RefBook ref(opts.ref_book);
    OptBook opt(opts.opt_book);
    std::vector<Fill> fref, fopt;

    for (size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        fref.clear();
        fopt.clear();
        AddResult rref, ropt;
        Status sref = apply_op(op.kind, op.ord, op.target, ref, fref, &rref);
        Status sopt = apply_op(op.kind, op.ord, op.target, opt, fopt, &ropt);

        rep.ops_applied = i + 1;
        rep.fills_total += fref.size();

        auto diverge = [&](const char* why, u64 ctx) {
            rep.diverged = true;
            rep.div = Divergence{static_cast<u64>(i), op.kind, why, ctx};
        };

        if (sref.code != sopt.code || sref.context != sopt.context) {
            ++rep.status_mismatches;
            diverge("status mismatch", sref.code != sopt.code ? static_cast<u64>(sref.code) : sref.context);
        } else if (!fills_equal(fref, fopt)) {
            ++rep.fill_mismatches;
            diverge("fill mismatch", fref.size() != fopt.size() ? fref.size() : fref[0].maker_order_id);
        } else if (ref.best_bid() != opt.best_bid() || ref.best_ask() != opt.best_ask()) {
            ++rep.best_mismatches;
            diverge("best price mismatch", ref.best_bid());
        } else {
            const u64 href = ref.state_hash();
            const u64 hopt = opt.state_hash();
            if (href != hopt) {
                ++rep.ladder_mismatches;
                diverge("ladder hash mismatch", href);
            }
        }

        if (rep.diverged && opts.stop_at_first_divergence) return rep;
    }
    return rep;
}

std::vector<Op> random_ops(u64 seed, size_t n_ops) {
    Rng rng = Rng::stream(seed, 0xAB1E5EEDull);
    std::vector<Op> ops;
    ops.reserve(n_ops);
    std::unordered_set<u64> maybe_live;  // ids that *may* still be resting
    u64 next_id = 1;
    for (size_t i = 0; i < n_ops; ++i) {
        const u64 roll = rng.bounded(100);
        Op op;
        if (roll < 62 || maybe_live.empty()) {
            // Add: mostly limits, some market; GTC/IOC/FOK/GTD mix
            Order o;
            o.id = next_id++;
            o.account = static_cast<u32>(1 + rng.bounded(4));
            o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
            const u64 kind_roll = rng.bounded(100);
            if (kind_roll < 78) {
                o.type = OrderType::Limit;
                o.price = 1000 + static_cast<Price>(rng.bounded(1001));
                const u64 tif_roll = rng.bounded(100);
                o.tif = tif_roll < 60 ? Tif::GTC : (tif_roll < 80 ? Tif::IOC : (tif_roll < 90 ? Tif::FOK : Tif::GTD));
                if (o.tif == Tif::GTD) o.expiry = 50'000 + static_cast<LogicalNs>(rng.bounded(100'000));
                if (o.tif == Tif::GTC || o.tif == Tif::GTD) maybe_live.insert(o.id);
            } else if (kind_roll < 95) {
                o.type = OrderType::Market;
                o.tif = Tif::IOC;
            } else {
                o.type = OrderType::Market;
                o.tif = Tif::FOK;
            }
            o.qty = 1 + rng.bounded(30);
            o.leaves = o.qty;
            op.kind = Op::Kind::Add;
            op.ord = o;
        } else if (roll < 85) {
            // Cancel a plausibly-live id
            auto it = maybe_live.begin();
            std::advance(it, rng.bounded(maybe_live.size()));
            op.kind = Op::Kind::Cancel;
            op.target = *it;
            maybe_live.erase(it);
        } else {
            // Replace a plausibly-live id with a fresh order
            auto it = maybe_live.begin();
            std::advance(it, rng.bounded(maybe_live.size()));
            op.kind = Op::Kind::Replace;
            op.target = *it;
            maybe_live.erase(it);
            Order o;
            o.id = next_id++;
            o.account = static_cast<u32>(1 + rng.bounded(4));
            o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
            o.type = OrderType::Limit;
            o.price = 1000 + static_cast<Price>(rng.bounded(1001));
            const u64 tif_roll = rng.bounded(100);
            o.tif = tif_roll < 70 ? Tif::GTC : Tif::IOC;
            o.qty = 1 + rng.bounded(30);
            o.leaves = o.qty;
            if (o.tif == Tif::GTC) maybe_live.insert(o.id);
            op.ord = o;
        }
        ops.push_back(op);
    }
    return ops;
}

std::vector<Op> minimize_ops(const std::vector<Op>& ops,
                             const std::function<bool(const std::vector<Op>&)>& fails) {
    if (!fails(ops)) return {};
    std::vector<Op> current = ops;
    size_t chunk = current.size() / 2;
    while (chunk >= 1) {
        bool changed = false;
        // try removing chunks of size `chunk`
        size_t start = 0;
        while (start < current.size()) {
            const size_t len = std::min(chunk, current.size() - start);
            std::vector<Op> candidate;
            candidate.reserve(current.size() - len);
            candidate.insert(candidate.end(), current.begin(), current.begin() + static_cast<long>(start));
            candidate.insert(candidate.end(), current.begin() + static_cast<long>(start + len), current.end());
            if (fails(candidate)) {
                current = std::move(candidate);
                changed = true;
            } else {
                start += len;
            }
        }
        if (!changed) {
            if (chunk == 1) break;
            chunk = std::max<size_t>(1, chunk / 2);
        } else {
            chunk = std::max<size_t>(1, current.size() / 2);
        }
        if (current.empty()) break;
    }
    return current;
}

// ---- adversarial search over the generator parameter space ----

std::vector<Op> random_ops_tuned(u64 seed, size_t n_ops, const GenParams& p) {
    Rng rng = Rng::stream(seed, 0xAB1E5EEDull);
    std::vector<Op> ops;
    ops.reserve(n_ops);
    std::unordered_set<u64> maybe_live;
    u64 next_id = 1;
    const u32 add = std::min<u32>(99, p.add_rate_pct);
    const u32 limit = std::min<u32>(99, p.limit_pct);
    const u32 gtc = std::min<u32>(89, p.gtc_pct);
    const u32 ioc = std::min<u32>(90 - gtc, p.ioc_pct);
    const u32 qty_max = std::max<u32>(1, p.qty_max);
    const u32 span = std::max<u32>(1, p.price_span);
    for (size_t i = 0; i < n_ops; ++i) {
        const u64 roll = rng.bounded(100);
        Op op;
        if (roll < add || maybe_live.empty()) {
            Order o;
            o.id = next_id++;
            o.account = static_cast<u32>(1 + rng.bounded(4));
            o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
            const u64 kind_roll = rng.bounded(100);
            if (kind_roll < limit) {
                o.type = OrderType::Limit;
                o.price = 1000 + static_cast<Price>(rng.bounded(span));
                const u64 tif_roll = rng.bounded(100);
                o.tif = tif_roll < gtc ? Tif::GTC
                                      : (tif_roll < gtc + ioc ? Tif::IOC
                                                              : (tif_roll < 90 ? Tif::FOK : Tif::GTD));
                if (o.tif == Tif::GTD) o.expiry = 50'000 + static_cast<LogicalNs>(rng.bounded(100'000));
                if (o.tif == Tif::GTC || o.tif == Tif::GTD) maybe_live.insert(o.id);
            } else {
                o.type = OrderType::Market;
                o.tif = rng.chance(0.85) ? Tif::IOC : Tif::FOK;
            }
            o.qty = 1 + rng.bounded(qty_max);
            o.leaves = o.qty;
            op.kind = Op::Kind::Add;
            op.ord = o;
        } else if (roll < add + (100 - add) * 23 / 100) {
            auto it = maybe_live.begin();
            std::advance(it, rng.bounded(maybe_live.size()));
            op.kind = Op::Kind::Cancel;
            op.target = *it;
            maybe_live.erase(it);
        } else {
            auto it = maybe_live.begin();
            std::advance(it, rng.bounded(maybe_live.size()));
            op.kind = Op::Kind::Replace;
            op.target = *it;
            maybe_live.erase(it);
            Order o;
            o.id = next_id++;
            o.account = static_cast<u32>(1 + rng.bounded(4));
            o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
            o.type = OrderType::Limit;
            o.price = 1000 + static_cast<Price>(rng.bounded(span));
            o.tif = rng.chance(0.7) ? Tif::GTC : Tif::IOC;
            o.qty = 1 + rng.bounded(qty_max);
            o.leaves = o.qty;
            if (o.tif == Tif::GTC) maybe_live.insert(o.id);
            op.ord = o;
        }
        ops.push_back(op);
    }
    return ops;
}

std::string ops_to_text(const std::vector<Op>& ops) {
    // line format: KIND id account side type tif price qty target
    std::string out;
    char buf[256];
    for (const Op& op : ops) {
        const char* k = op.kind == Op::Kind::Add ? "A" : (op.kind == Op::Kind::Cancel ? "C" : "R");
        std::snprintf(buf, sizeof buf, "%s %llu %u %u %u %u %lld %llu %llu\n", k,
                      (unsigned long long)op.ord.id, (unsigned)op.ord.account,
                      (unsigned)op.ord.side, (unsigned)op.ord.type, (unsigned)op.ord.tif,
                      (long long)op.ord.price, (unsigned long long)op.ord.qty,
                      (unsigned long long)op.target);
        out += buf;
    }
    return out;
}

Result<std::vector<Op>> ops_from_text(const std::string& text) {
    std::vector<Op> ops;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) break;
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        char kind = 0;
        unsigned long long id = 0, qty = 0, target = 0;
        unsigned acct = 0, side = 0, otype = 0, tif = 0;
        long long price = 0;
        if (std::sscanf(line.c_str(), "%c %llu %u %u %u %u %lld %llu %llu", &kind, &id, &acct,
                        &side, &otype, &tif, &price, &qty, &target) != 9)
            return ErrT<std::vector<Op>>(Errc::ProtocolInvalid, ops.size());
        Op op;
        op.target = target;
        if (kind == 'A') op.kind = Op::Kind::Add;
        else if (kind == 'C') op.kind = Op::Kind::Cancel;
        else if (kind == 'R') op.kind = Op::Kind::Replace;
        else return ErrT<std::vector<Op>>(Errc::ProtocolInvalid, ops.size());
        // range-validate the enum fields: a corrupt corpus file must be a
        // loud parse failure instead of feeding garbage to the books
        if (side > 1 || otype > 1 || tif > 3)
            return ErrT<std::vector<Op>>(Errc::ProtocolInvalid, ops.size());
        op.ord.id = id;
        op.ord.account = acct;
        op.ord.side = static_cast<Side>(side);
        op.ord.type = static_cast<OrderType>(otype);
        op.ord.tif = static_cast<Tif>(tif);
        op.ord.price = price;
        op.ord.qty = qty;
        op.ord.leaves = qty;
        ops.push_back(op);
    }
    return OkT(std::move(ops));
}

SearchResult adversarial_search(u64 base_seed, u32 iterations, u32 ops_per_run,
                                const std::string& corpus_dir) {
    SearchResult res;
    Rng rng = Rng::stream(base_seed, 0xAD5E4C4Eull);
    GenParams params;  // start from the classic distribution
    // hill-climb: a score > 0 means a divergence was found (stop + persist).
    // Otherwise prefer parameter sets that stress the matching engine more
    // (denser books: smaller spans, larger quantities).
    auto stress_score = [&](const GenParams& p) -> i64 {
        // lower span & bigger qty = denser levels = more partial fills
        return static_cast<i64>(1001 - std::min<u32>(1001, p.price_span)) +
               static_cast<i64>(p.qty_max);
    };
    i64 cur = stress_score(params);
    for (u32 it = 0; it < iterations; ++it) {
        res.iterations = it + 1;
        GenParams cand = params;
        const u32 which = static_cast<u32>(rng.bounded(6));
        const i32 delta = rng.chance(0.5) ? 1 : -1;
        switch (which) {
            case 0: cand.add_rate_pct = static_cast<u32>(std::clamp<int>(cand.add_rate_pct + delta * 7, 20, 95)); break;
            case 1: cand.limit_pct = static_cast<u32>(std::clamp<int>(cand.limit_pct + delta * 7, 10, 99)); break;
            case 2: cand.gtc_pct = static_cast<u32>(std::clamp<int>(cand.gtc_pct + delta * 7, 5, 85)); break;
            case 3: cand.ioc_pct = static_cast<u32>(std::clamp<int>(cand.ioc_pct + delta * 5, 2, 25)); break;
            case 4: cand.qty_max = static_cast<u32>(std::clamp<int>(cand.qty_max + delta * 11, 1, 200)); break;
            case 5: cand.price_span = static_cast<u32>(std::clamp<int>(cand.price_span + delta * 91, 1, 1001)); break;
        }
        for (u64 s = base_seed; s < base_seed + 2 && !res.found; ++s) {
            ++res.seeds_tried;
            auto ops = random_ops_tuned(s, ops_per_run, cand);
            Report rep = run_differential(ops);
            if (rep.diverged) {
                // DIVERGENCE FOUND: minimize + persist + stop
                res.found = true;
                res.div = Divergence{rep.div.op_index, rep.div.kind, rep.div.why, rep.div.context};
                res.best_params = cand;
                auto fails = [](const std::vector<Op>& s2) {
                    Report r = run_differential(s2);
                    return r.diverged;
                };
                res.minimized_reproducer = minimize_ops(ops, fails);
                std::error_code ec;
                std::filesystem::create_directories(corpus_dir, ec);
                const std::string name = corpus_dir + "/adv_" + std::to_string(base_seed) + "_" +
                                         std::to_string(rep.div.op_index) + ".ops";
                std::FILE* f = std::fopen(name.c_str(), "wb");
                if (f) {
                    const std::string text = ops_to_text(res.minimized_reproducer);
                    std::fwrite(text.data(), 1, text.size(), f);
                    std::fclose(f);
                }
                break;
            }
        }
        if (res.found) break;
        if (stress_score(cand) >= cur) {
            params = cand;
            cur = stress_score(cand);
        }
    }
    return res;
}

std::pair<u64, u64> run_corpus(const std::string& corpus_dir) {
    u64 checked = 0, diverged = 0;
    std::error_code ec;
    if (!std::filesystem::exists(corpus_dir, ec)) return {0, 0};
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(corpus_dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ops")
            files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    for (const std::string& f : files) {
        std::FILE* fh = std::fopen(f.c_str(), "rb");
        if (!fh) continue;
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, fh)) > 0) text.append(buf, n);
        std::fclose(fh);
        auto ops = ops_from_text(text);
        if (!ops) continue;
        ++checked;
        Report rep = run_differential(ops.value);
        if (rep.diverged) ++diverged;
    }
    return {checked, diverged};
}

}  // namespace diff
}  // namespace arbiter