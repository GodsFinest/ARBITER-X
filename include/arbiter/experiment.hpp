#pragma once
// Counterfactual experiment engine.
//
// An experiment defines a baseline World configuration and a set of named
// single-variable deltas (latency, jitter, loss, strategy/risk parameters).
// Every variant runs with the SAME master seed, so all variants share the
// same random draw streams: histories are identical until the changed
// variable reorders events, which is measured by comparing the state-hash
// timelines (first divergent checkpoint). Economic outcomes (fills, PnL,
// adverse selection) are compared in a table and dumped as JSON + markdown
// artifacts.
//
// Every artifact records the experiment's
// definition hash, a digest of (name, seed, baseline config hash, variant
// names + config hashes), plus the BranchIdentity genealogy of each variant
// branch genealogy. `experiment_reproduce` re-runs the recorded grid and verifies
// every recorded hash bit-for-bit.

#include "arbiter/config.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/metrics.hpp"
#include "arbiter/world.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <string>
#include <vector>

namespace arbiter {

struct Variant {
    std::string name;
    WorldConfig cfg;  // full config (base + the one change)
    // reproducibility: the exact override set that produced cfg from the
    // baseline (dotted-key -> raw string), in deterministic order
    std::vector<std::pair<std::string, std::string>> overrides;
};

struct ExperimentResult {
    std::vector<std::string> names;
    std::vector<RunStats> stats;
    std::vector<u64> final_hashes;
    // for each variant pair (baseline, variant): first divergent checkpoint
    // index into the baseline's hash timeline (size_t max = never diverged)
    std::vector<u64> first_divergence_event;
    // Reproducibility and branch genealogy.
    std::string experiment_name;
    u64 seed = 0;
    u64 definition_hash = 0;                     // hash of the grid definition
    LogicalNs base_horizon_ns = 0;               // baseline horizon (grid rebuild input)
    std::vector<u64> config_hashes;             // per-variant config hash
    std::vector<BranchIdentity> branches;       // per-variant genealogy
    // per-variant override set (dotted-key, raw value), what `reproduce`
    // re-applies to rebuild the exact grid
    std::vector<std::vector<std::pair<std::string, std::string>>> overrides;
};

class Experiment {
public:
    Experiment() = default;  // ErrT<Experiment> needs default-constructibility
    explicit Experiment(WorldConfig base) : base_(std::move(base)) {}

    void add_variant(const std::string& name, const std::function<void(WorldConfig&)>& mutate) {
        Variant v;
        v.name = name;
        v.cfg = base_;
        mutate(v.cfg);
        variants_.push_back(std::move(v));
    }

    // Config-driven variant: records the override set for reproduction.
    // When this is the FIRST variant, its overrides are applied to the base
    // too (it defines the baseline), keeping base_ == variant-0 cfg exactly.
    void add_variant(const std::string& name,
                     std::vector<std::pair<std::string, std::string>> overrides) {
        Variant v;
        v.name = name;
        v.cfg = base_;
        std::string err;
        const bool is_baseline = variants_.empty();
        for (const auto& [k, val] : overrides) {
            apply_key_pub(v.cfg, k, val, err);
            if (is_baseline) apply_key_pub(base_, k, val, err);
        }
        v.overrides = std::move(overrides);
        variants_.push_back(std::move(v));
    }

    // Fully-formed variant (from_config baseline path: pre-applied cfg).
    void add_raw_variant(Variant v) { variants_.push_back(std::move(v)); }

    static bool apply_key_pub(WorldConfig& c, const std::string& k, const std::string& raw,
                             std::string& err);

    ExperimentResult run() const {
        ExperimentResult res;
        res.experiment_name = name_;
        res.seed = seed_;
        res.definition_hash = definition_hash();
        res.base_horizon_ns = base_.horizon_ns;
        res.overrides.resize(variants_.size());
        auto run_one = [&](const Variant& v) {
            World w(v.cfg, seed_);
            RunStats s = w.run();
            return std::make_pair(s, w.state_hash());
        };
        // baseline first (variant 0 = unmodified)
        auto [s0, h0] = run_one(variants_[0]);
        const u64 parent_world_id = World(variants_[0].cfg, seed_).world_id();
        res.names.push_back(variants_[0].name);
        res.stats.push_back(s0);
        res.final_hashes.push_back(h0);
        res.first_divergence_event.push_back(~u64{0});
        res.config_hashes.push_back(variants_[0].cfg.hash());
        res.overrides[0] = variants_[0].overrides;
        {
            // the baseline is the root branch (fork_event 0, no parent)
            BranchIdentity bi{};
            bi.world_id = parent_world_id;
            bi.parent_world_id = 0;
            bi.fork_event = 0;
            bi.config_hash = variants_[0].cfg.hash();
            bi.initial_state_hash = 0;
            res.branches.push_back(bi);
        }
        for (size_t i = 1; i < variants_.size(); ++i) {
            auto [s, h] = run_one(variants_[i]);
            res.names.push_back(variants_[i].name);
            res.stats.push_back(s);
            res.final_hashes.push_back(h);
            // first divergent checkpoint vs baseline
            u64 div = ~u64{0};
            const size_t common = std::min(s0.hash_timeline.size(), s.hash_timeline.size());
            for (size_t k = 0; k < common; ++k) {
                if (s0.hash_timeline[k].second != s.hash_timeline[k].second) {
                    div = s0.hash_timeline[k].first;
                    break;
                }
            }
            res.first_divergence_event.push_back(div);
            res.config_hashes.push_back(variants_[i].cfg.hash());
            res.overrides[i] = variants_[i].overrides;
            {
                // genealogy: variant branched from the baseline at event 0
                // (full re-run with the same seed; identical history until
                // the changed variable acts)
                BranchIdentity bi{};
                bi.world_id = World(variants_[i].cfg, seed_).world_id();
                bi.parent_world_id = parent_world_id;
                bi.fork_event = div == ~u64{0} ? 0 : div;
                bi.config_hash = variants_[i].cfg.hash();
                bi.initial_state_hash = 0;
                res.branches.push_back(bi);
            }
        }
        return res;
    }

    void set_seed(u64 seed) noexcept { seed_ = seed; }
    void set_name(const std::string& n) { name_ = n; }

    // Deterministic identity of the grid DEFINITION: (name, seed, base
    // config hash, per-variant (name, config hash)). Any change to any of
    // these changes the hash; identical definitions hash identically.
    u64 definition_hash() const {
        Hasher h;
        h.tag("ARBITERX/EXPDEF/2");
        for (char c : name_) h.u8v(static_cast<u8>(c));
        h.u64v(seed_);
        h.u64v(base_.hash());
        h.u64v(variants_.size());
        for (const auto& v : variants_) {
            for (char c : v.name) h.u8v(static_cast<u8>(c));
            h.u64v(v.cfg.hash());
            h.u64v(v.overrides.size());
            for (const auto& [k, val] : v.overrides) {
                for (char c : k) h.u8v(static_cast<u8>(c));
                for (char c : val) h.u8v(static_cast<u8>(c));
            }
        }
        return h.value();
    }

    const WorldConfig& base_config() const noexcept { return base_; }
    const std::vector<Variant>& variants() const noexcept { return variants_; }

    // ---- file-driven experiment definitions (docs/COUNTERFACTUALS.md) ----
    // Grammar (Config sectioned key=value; section names are DOTTED):
    //   [experiment]
    //   name = my_grid        # optional (file stem used otherwise)
    //   seed = 2026
    //   horizon_ms = 40
    //   [baseline]
    //   # baseline WorldConfig overrides
    //   [variant.NAME]        # one variable or several per variant
    //   algo_latency_ns = 3000
    //   md_channel.loss_rate = 0.02
    // Returns a failed Status on unknown keys, reproduction must never
    // guess.
    static Result<Experiment> from_config(const Config& cfg, const std::string& fallback_name);

private:
    WorldConfig base_;
    std::vector<Variant> variants_;
    u64 seed_ = 42;
    std::string name_ = "experiment";
};

// Artifact writers (JSON + human-readable comparison table).
std::string experiment_to_json(const ExperimentResult& r, const std::string& name);
std::string experiment_summary(const ExperimentResult& r, const std::string& name);

// Parse an artifact JSON, re-run the recorded grid,
// verify every recorded hash. Returns [ok, detail].
struct ReproduceReport {
    bool ok = false;
    u64 variants_checked = 0;
    u64 hash_mismatches = 0;
    std::string detail;
};
ReproduceReport experiment_reproduce(const std::string& artifact_path);

}  // namespace arbiter