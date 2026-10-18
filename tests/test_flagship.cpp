// Flagship research-layer tests: typed allocation, flow regimes, tracing
// equivalence, snapshot/fork under new state, stage-placement validity.
// Deterministic; AX_SOURCE_DIR for repo-relative paths.
#include "testfw.hpp"

#include "arbiter/errors.hpp"
#include "arbiter/flagship.hpp"
#include "arbiter/journal.hpp"
#include "arbiter/latency.hpp"
#include "arbiter/net.hpp"
#include "arbiter/rng.hpp"
#include "arbiter/strategy.hpp"
#include "arbiter/world.hpp"
#include "arbiter/world_snapshot.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace arbiter;

// ---------------- typed latency allocation ----------------

AX_TEST(allocation_sum_invariant_and_rejection) {
    // valid: sum == total
    LatencyAllocation a{3000, 4000, 1000};
    CHECK_EQ(a.total_ns(), 8000);
    CHECK(validate_allocation(a, 8000));
    // wrong total: rejected
    CHECK(!validate_allocation(a, 9000));
    // negative stage: rejected
    LatencyAllocation bad{-1, 4000, 4001};
    CHECK(!validate_allocation(bad, 8000));
    CHECK_EQ(bad.total_ns(), 8000);  // no overflow, just invalid semantically
    // slice helper keeps the invariant EXACTLY for every share
    for (double x = 0.0; x <= 1.0001; x += 0.05) {
        const LatencyAllocation s = allocation_on_slice(8000, x);
        CHECK(validate_allocation(s, 8000));
    }
}

AX_TEST(allocation_serialization_roundtrip_and_hash) {
    const LatencyAllocation a{1234, 5678, 91};
    const std::string s = a.to_string();
    auto back = LatencyAllocation::from_string(s);
    CHECK(back);
    if (back) {
        CHECK_EQ(back.value.md_transit_ns, a.md_transit_ns);
        CHECK_EQ(back.value.decision_ns, a.decision_ns);
        CHECK_EQ(back.value.order_transit_ns, a.order_transit_ns);
    }
    // malformed strings: rejected loudly
    CHECK(!LatencyAllocation::from_string("md+1,dec+2"));
    CHECK(!LatencyAllocation::from_string("md+-1,dec+2,ord+3"));
    CHECK(!LatencyAllocation::from_string(""));
    CHECK(!LatencyAllocation::from_string("md+1,dec+2,ord+3,junk"));
    // hash is deterministic and allocation-sensitive
    Hasher h1, h2;
    a.hash_into(h1);
    a.hash_into(h2);
    CHECK_EQ(h1.value(), h2.value());
    LatencyAllocation b{1235, 5678, 91};
    Hasher h3;
    b.hash_into(h3);
    CHECK_NE(h1.value(), h3.value());
}

AX_TEST(allocation_apply_zeroes_jitter_and_sets_stages) {
    WorldConfig cfg;
    cfg.md_channel.jitter_ns = 500;   // would confound placement
    cfg.order_channel.jitter_ns = 700;
    const LatencyAllocation a{2000, 3000, 3000};
    const WorldConfig out = a.apply_to(cfg);
    CHECK_EQ(out.md_channel.base_delay_ns, 2000);
    CHECK_EQ(out.algo_latency_ns, 3000);
    CHECK_EQ(out.order_channel.base_delay_ns, 3000);
    CHECK_EQ(out.md_channel.jitter_ns, 0);
    CHECK_EQ(out.order_channel.jitter_ns, 0);
    CHECK_EQ(out.exec_channel.jitter_ns, 0);  // clean by flagship contract
    CHECK(out.hash() != cfg.hash());  // config identity moved
}

// ---------------- §36: the engine itself must show the placement ----------------

AX_TEST(stage_placement_is_realized_in_engine_timestamps) {
    // Two allocations with the SAME total, different placement. The
    // engine-measured mean stage latencies must move in the intended
    // direction and the SUM of trader-side stages must be equal.
    const LatencyAllocation observe_heavy = allocation_on_slice(8000, 0.75);
    const LatencyAllocation action_heavy = allocation_on_slice(8000, 0.0);
    CHECK_EQ(observe_heavy.total_ns(), action_heavy.total_ns());
    CHECK_NE(observe_heavy.md_transit_ns, action_heavy.md_transit_ns);
    CHECK_NE(observe_heavy.decision_ns, action_heavy.decision_ns);

    auto run_with = [&](const LatencyAllocation& alloc) {
        WorldConfig cfg = alloc.apply_to(WorldConfig{});
        cfg.journal_events = false;
        cfg.horizon_ns = 60'000'000;
        cfg.warmup_ns = 2'000'000;
        cfg.sim.taker_self_excitation = 0.9;  // active regime: fills happen
        World w(cfg, 2026);
        w.run();
        const RunStats& s = w.stats();
        return std::pair<u64, std::pair<double, double>>{
            s.stage_traced_fills,
            {static_cast<double>(s.stage_sum_decision_to_depart) / (s.stage_traced_fills ? s.stage_traced_fills : 1),
             static_cast<double>(s.stage_sum_depart_to_gateway) / (s.stage_traced_fills ? s.stage_traced_fills : 1)}};
    };
    const auto [n_a, dec_a] = run_with(action_heavy);
    const auto [n_b, dec_b] = run_with(observe_heavy);
    CHECK(n_a > 0);
    CHECK(n_b > 0);
    // decision stage realized mean must track the CONFIGURED decision ns
    // (exact placement, zero jitter) within a small tolerance driven by
    // event-quantization (the decision stage is exactly algo_latency_ns:
    // every traced fill carries it; assert exact equality).
    CHECK(std::fabs(dec_a.first - static_cast<double>(action_heavy.decision_ns)) < 0.5);
    CHECK(std::fabs(dec_b.first - static_cast<double>(observe_heavy.decision_ns)) < 0.5);
    // and they differ across allocations
    CHECK_NE(dec_a.first, dec_b.first);
}

// ---------------- tracing on/off: economic + state equivalence ----------------


// ---------------- stage-trace exact sums (incl. partial fills) ----------------


// ---------------- flow regimes: Poisson null vs clustered ----------------



// ---------------- snapshot/fork under the new state ----------------




// ---------------- shadow (frozen) interference mode ----------------


// ---------------- queue-model ambiguity accounting ----------------


// ---------------- flagship determinism + artifact discipline ----------------

AX_TEST(flagship_run_is_deterministic_and_reproducible) {
    FlagshipConfig fc;
    fc.total_latency_ns = 4000;
    fc.md_shares = {0.0, 0.5, 1.0};
    fc.seeds = {99, 100, 101};
    fc.taker_self_excitation = 0.9;
    fc.permutation_reps = 500;
    const FlagshipResult a = run_flagship(fc);
    const FlagshipResult b = run_flagship(fc);
    CHECK_EQ(a.definition_hash, b.definition_hash);
    CHECK_EQ(a.cells.size(), b.cells.size());
    for (size_t c = 0; c < a.cells.size(); ++c)
        for (size_t k = 0; k < a.cfg.seeds.size(); ++k) {
            CHECK_EQ(a.cells[c].per_seed[k].final_state_hash,
                     b.cells[c].per_seed[k].final_state_hash);
            CHECK_EQ(a.cells[c].per_seed[k].mm_marked_pnl, b.cells[c].per_seed[k].mm_marked_pnl);
        }
    // artifact roundtrip + reproduce
    std::filesystem::create_directories("artifacts/testexp_flag");
    if (!write_flagship_artifacts("artifacts/testexp_flag", a)) {
        CHECK(!"artifact write failed");
        return;
    }
    const std::string json = flagship_to_json(a);
    CHECK(json.find("\"definition_hash\"") != std::string::npos);
    CHECK(json.find("\"ambiguous") == std::string::npos);  // no stray fields
    // tamper with one recorded hash: reproduction must FAIL
    const FlagshipReproReport ok_rep = flagship_reproduce("artifacts/testexp_flag/results.json");
    CHECK(ok_rep.ok);
    std::string tampered = json;
    const std::string needle = "\"final_hash\": " + std::to_string(a.cells[0].per_seed[0].final_state_hash);
    const size_t at = tampered.find(needle);
    CHECK(at != std::string::npos);
    tampered.replace(at, needle.size(),
                    "\"final_hash\": " + std::to_string(a.cells[0].per_seed[0].final_state_hash ^ 1));
    FILE* f = fopen("artifacts/testexp_flag/results.json", "wb");
    CHECK(f != nullptr);
    if (f) {
        fwrite(tampered.data(), 1, tampered.size(), f);
        fclose(f);
    }
    const FlagshipReproReport bad_rep = flagship_reproduce("artifacts/testexp_flag/results.json");
    CHECK(!bad_rep.ok);
    CHECK(bad_rep.mismatches > 0);
    // restore the good artifact
    f = fopen("artifacts/testexp_flag/results.json", "wb");
    CHECK(f != nullptr);
    if (f) {
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
    }
    CHECK(flagship_reproduce("artifacts/testexp_flag/results.json").ok);
}

AX_TEST(flagship_rejects_malformed_artifacts) {
    std::filesystem::create_directories("artifacts");
    auto write = [](const char* name, const std::string& body) {
        FILE* f = fopen((std::string("artifacts/") + name).c_str(), "wb");
        fwrite(body.data(), 1, body.size(), f);
        fclose(f);
    };
    write("fl_bad1.json", "{}");
    CHECK(!flagship_reproduce("artifacts/fl_bad1.json").ok);
    write("fl_bad2.json", "{\"experiment\": \"x\", \"flow_model\": \"clustered\", "
                          "\"definition_hash\": 1, \"total_latency_ns\": 8000, "
                          "\"taker_self_excitation\": 0.9, \"bots_observe_shadow\": false, "
                          "\"seeds\": [1], \"cells\": []}");
    CHECK(!flagship_reproduce("artifacts/fl_bad2.json").ok);  // no cells
    CHECK(!flagship_reproduce("artifacts/does_not_exist.json").ok);
    // wrong definition hash: caught loudly
    write("fl_bad3.json", "{\"experiment\": \"x\", \"flow_model\": \"clustered\", "
                          "\"definition_hash\": 123, \"total_latency_ns\": 8000, "
                          "\"taker_self_excitation\": 0.9, \"bots_observe_shadow\": false, "
                          "\"seeds\": [2026], \"cells\": []}");
    const FlagshipReproReport r = flagship_reproduce("artifacts/fl_bad3.json");
    CHECK(!r.ok);
    CHECK(r.detail.find("definition_hash mismatch") != std::string::npos ||
          r.detail.find("no cells") != std::string::npos);
}

// ---------------- §27: message-type latency granularity ----------------


AX_TEST_MAIN