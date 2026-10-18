#pragma once
// Deterministic network emulation.
//
// Channels are logical, not physical: a message departing at logical time t
// arrives at t + delay, or is dropped/duplicated/reordered according to
// seeded draws. The draw sequence depends only on (master_seed, channel_id)
// and the message index, so two runs with the same seed draw the SAME unit
// variates, a counterfactual that changes only the latency parameters
// changes delays but not the underlying randomness (docs/COUNTERFACTUALS.md).
//
// Fault model per message (FIXED stochastic process, see next_delivery):
//   draw order is ALWAYS: loss, burst-arm, dup, reorder-decide,
//   reorder-magnitude, corrupt, jitter-magnitude (7 draws per message).
//   Profile values gate INTERPRETATION only; draws for disabled dimensions
//   are consumed and discarded. Consequence: changing one profile dimension
//   (e.g. enabling loss) never realigns the RNG for unrelated dimensions,
//   so counterfactuals that vary one network variable keep all other
//   randomness identical (docs/COUNTERFACTUALS.md, docs/REPRODUCIBILITY.md).
//
// Wall-clock, OS sockets, and real NICs are NOT involved anywhere; this is a
// simulation layer and makes no claim of fidelity to any specific venue.

#include "arbiter/rng.hpp"
#include "arbiter/types.hpp"

namespace arbiter {

struct NetProfile {
    i64 base_delay_ns = 100;
    i64 jitter_ns = 100;
    double loss_rate = 0.0;
    double dup_rate = 0.0;
    u32 reorder_window = 0;       // 0 = disabled
    i64 reorder_slot_ns = 200;
    u32 burst_loss_len = 0;       // consecutive extra losses once a burst starts
    double burst_loss_rate = 0.0;
    double corrupt_rate = 0.0;    // in-transit single-byte corruption (CRC catches it)
};

struct DeliveryPlan {
    bool drop = false;
    bool duplicate = false;
    bool corrupt = false;      // flip one payload byte in flight
    i64 delay_ns = 0;        // primary delivery
    i64 dup_extra_ns = 0;    // duplicate arrives this much later
    i64 reorder_extra_ns = 0;
};

class Channel {
public:
    Channel() = default;
    Channel(u8 id, const NetProfile& prof, u64 master_seed)
        : prof_(prof), rng_(Rng::stream(master_seed, 0x4E455400ull + id)) {}

    // Consume the draws for one message. Deterministic per message index.
    // FIXED-DRAW SEMANTICS: every message consumes exactly 7 RNG draws in a
    // fixed order (loss, burst-arm, dup, reorder-decide, reorder-magnitude,
    // corrupt, jitter-magnitude), whether or not the corresponding profile
    // dimension is enabled and whether or not the message is dropped. A
    // mid-burst message consumes the same 7 draws (discarded) before
    // returning drop=true. Profile values only decide how draws are
    // INTERPRETED, never whether they happen, so enabling loss (or dup,
    // reorder, corrupt, jitter) cannot shift the draw alignment of any
    // other dimension for this or any later message. Tests:
    // tests/test_quant.cpp (network_draw_alignment_*).
    DeliveryPlan next_delivery() {
        // All 7 draws first, in fixed order.
        const bool loss_draw = rng_.chance(prof_.loss_rate);
        const bool burst_draw = rng_.chance(prof_.burst_loss_rate);
        const bool dup_draw = rng_.chance(prof_.dup_rate);
        const bool reorder_decide = rng_.chance(0.5);
        const u64 reorder_mag =
            rng_.bounded(prof_.reorder_window > 0 ? prof_.reorder_window : 1u);
        const bool corrupt_draw = rng_.chance(prof_.corrupt_rate);
        const u64 jitter_mag =
            rng_.bounded(prof_.jitter_ns > 0 ? static_cast<u64>(prof_.jitter_ns) : 1u);
        DeliveryPlan d;
        // burst loss state machine (draws above already consumed)
        if (burst_remaining_ > 0) {
            --burst_remaining_;
            d.drop = true;
            return d;
        }
        if (loss_draw) {
            d.drop = true;
            if (prof_.burst_loss_len > 0 && burst_draw)
                burst_remaining_ = prof_.burst_loss_len;
            return d;
        }
        if (dup_draw) d.duplicate = true;
        if (prof_.reorder_window > 0 && reorder_decide)
            d.reorder_extra_ns =
                static_cast<i64>(reorder_mag) * prof_.reorder_slot_ns;
        if (corrupt_draw) d.corrupt = true;
        d.delay_ns = prof_.base_delay_ns;
        if (prof_.jitter_ns > 0) d.delay_ns += static_cast<i64>(jitter_mag);
        if (d.duplicate) d.dup_extra_ns = d.delay_ns + prof_.base_delay_ns;
        return d;
    }

    // Full channel state for snapshot/restore: the RNG stream position AND
    // the burst-loss state machine (a mid-burst snapshot must resume the
    // burst on restore, behavioral state, not just the RNG).
    u64 state() const noexcept { return rng_.state; }
    void set_state(u64 s) noexcept { rng_.state = s; }
    u32 burst_state() const noexcept { return burst_remaining_; }
    void set_burst_state(u32 b) noexcept { burst_remaining_ = b; }
    const NetProfile& profile() const noexcept { return prof_; }

private:
    NetProfile prof_;
    Rng rng_{0};
    u32 burst_remaining_ = 0;
};

}  // namespace arbiter