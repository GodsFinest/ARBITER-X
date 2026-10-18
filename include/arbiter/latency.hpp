#pragma once
// Typed fixed-total latency allocation (flagship research control).
//
// The flagship question: for a FIXED total execution-latency budget L, does
// redistributing latency across the trader-side pipeline stages change
// execution economics? This type makes the intervention first-class:
//
//   LatencyAllocation { md_transit_ns, decision_ns, order_transit_ns }
//
// with the invariant
//
//   md_transit_ns + decision_ns + order_transit_ns == total_ns
//
// enforced at construction; illegal allocations cannot be built.
//
// SEMANTIC DEFINITIONS (each maps to exactly one engine stage; see
// docs/EXECUTION_MODEL.md "Stage semantics"):
//
//   md_transit_ns    observation latency: exchange MD publication -> arrival
//                    at the strategy feed handler (md_channel base delay).
//                    What decays: the book view the strategy acts on.
//                    During it the market can move (takers/providers act);
//                    the trader cannot react (no information yet).
//
//   decision_ns      action latency: strategy decision made -> order frame
//                    departs toward the exchange (algo_latency). What
//                    decays: the quote being placed/canceled. The trader has
//                    already decided; the market moves on while the intent is
//                    in flight internally.
//
//   order_transit_ns order latency: frame departure -> gateway arrival
//                    (order_channel base delay). Same information-decay
//                    structure as decision_ns from the trader's perspective,
//                    but a different owner (network vs strategy process) and
//                    a different position in the causal chain.
//
// The exchange-side stages (exchange_processing_ns, publish_latency_ns,
// exec_channel) are NOT part of the allocation: the flagship question is
// about the trader's own pipeline, and those stages are common to every
// variant (held fixed across allocations).
//
// Determinism: the allocation maps onto pure config deltas (md_channel
// .base_delay_ns, algo_latency_ns, order_channel.base_delay_ns); jitter on
// the md/order channels is zeroed by the flagship experiment so that stage
// placement is exact (no stochastic confound between allocation cells).
// The typed object serializes canonically and hashes into the experiment
// definition hash.

#include "arbiter/hash.hpp"
#include "arbiter/types.hpp"
#include "arbiter/world.hpp"

#include <limits>
#include <string>

namespace arbiter {

struct LatencyAllocation {
    i64 md_transit_ns = 0;
    i64 decision_ns = 0;
    i64 order_transit_ns = 0;

    // Total budget L (the invariant; exact, integer ns).
    i64 total_ns() const noexcept { return md_transit_ns + decision_ns + order_transit_ns; }

    // Canonical serialization: "md+k,dec+k,ord+k", deterministic, human
    // readable, round-trips exactly.
    std::string to_string() const;
    static Result<LatencyAllocation> from_string(std::string_view s);

    // Hash into an experiment definition (allocation identity).
    void hash_into(Hasher& h) const;

    // Build a WorldConfig that realizes this allocation: applies the three
    // stage delays and ZEROES md/order-channel jitter so the placement is
    // exact (documented: the flagship isolates placement, not jitter).
    // Exchange-side stages are left at their defaults (held fixed).
    WorldConfig apply_to(WorldConfig cfg) const;
};

// Validation: non-negative stages, sum == expected total, no overflow.
// Returns Errc::InvalidArgument (ctx: 0=negative, 1=sum-mismatch, 2=overflow)
// on violation.
inline Status validate_allocation(const LatencyAllocation& a, i64 expected_total_ns) {
    if (a.md_transit_ns < 0 || a.decision_ns < 0 || a.order_transit_ns < 0)
        return fail(Errc::InvalidArgument, 0);
    // overflow-safe sum check (all non-negative, i64 domain)
    if (a.md_transit_ns > std::numeric_limits<i64>::max() - a.decision_ns ||
        a.md_transit_ns + a.decision_ns > std::numeric_limits<i64>::max() - a.order_transit_ns)
        return fail(Errc::InvalidArgument, 2);
    if (a.total_ns() != expected_total_ns) return fail(Errc::InvalidArgument, 1);
    return good();
}

// Allocation share x in [0,1] on the md_transit axis of the 3-stage simplex
// slice: md = x*L, decision = (1-x)*L/2, order = (1-x)*L/2. (The 2-D slice
// through the simplex: vary observation share with the action stages
// split evenly.) Exact integer ns; the residual from rounding is assigned
// to decision so the invariant holds exactly.
LatencyAllocation allocation_on_slice(i64 total_ns, double md_share);

}  // namespace arbiter