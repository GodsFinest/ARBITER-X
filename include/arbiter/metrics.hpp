#pragma once
// Run metrics + artifact output. Integer money (ticks*qty) everywhere in the
// deterministic core; wall-clock measurements only in the perf lab.

#include "arbiter/histogram.hpp"
#include "arbiter/types.hpp"

#include <string>
#include <vector>

namespace arbiter {

struct RunStats {
    // scale
    u64 events_processed = 0;
    u64 journal_records = 0;
    LogicalNs logical_end = 0;
    u64 wall_ns = 0;  // host wall-clock of the run (instrumentation only)

    // exchange
    u64 bot_fills = 0;
    u64 strategy_fills = 0;
    u64 momentum_fills = 0;
    u64 trades = 0;
    Qty volume = 0;

    // orders
    u64 strategy_orders_sent = 0;
    u64 strategy_cancels_sent = 0;
    u64 order_frames_dropped = 0;
    u64 risk_accepted = 0;
    u64 risk_rejected = 0;

    // per-channel fault accounting; corruption is the shared
    // wire_frames_corrupted counter, *_duplicated counts receiver-suppressed
    // duplicate deliveries (see the counter contract above)
    u64 exec_frames_dropped = 0;
    u64 exec_frames_duplicated = 0;
    u64 order_frames_duplicated = 0;

    // network. COUNTER CONTRACT (identical rules on all three channels):
    //   encoded  = unique frame creations (a duplicated delivery re-transmits
    //              the same frame and is NOT re-encoded; a recovery retransmit
    //              frame IS a new frame and is counted);
    //   decoded  = CRC-valid deliveries ACCEPTED for processing (post
    //              transport-dedupe — suppressed duplicates are not counted);
    //   corrupted= receiver attempts rejected by CRC/structure, PER ATTEMPT
    //              (a duplicated corrupted frame counts once per attempt);
    //   dropped  = frames lost before reaching the receiver (decision side);
    //   duplicated = duplicate delivery attempts SUPPRESSED at the receiver
    //              (for MD this includes racing retransmits; the publish-side
    //              duplication decision is visible as a second scheduled
    //              delivery, never as an extra encode).
    u64 md_published = 0;
    u64 md_delivered = 0;
    u64 md_dropped = 0;
    u64 md_duplicated = 0;
    u64 md_reordered = 0;   // reorder decisions (MD only, decision side)
    u64 md_gap_events = 0;
    u64 md_recovered = 0;   // gap-recovery retransmit packets scheduled
    u64 md_snapshot_recoveries = 0;

    // wire protocol (live path: order/exec frames encoded + decoded)
    u64 wire_frames_encoded = 0;
    u64 wire_frames_decoded = 0;
    u64 wire_frames_corrupted = 0;  // rejected by CRC at the receiver

    // strategy economics (integer, ticks*qty; marked PnL uses final mid)
    i64 mm_inventory = 0, mm_cash = 0;
    i64 mom_inventory = 0, mom_cash = 0;
    // exchange-authoritative economics (updated at MATCH time from the
    // exchange ledger, per execution leg). These survive exec-channel faults:
    // a lost/corrupted execution report can stale the notification-driven
    // strategy-local views above, never these. Self-trades record BOTH legs
    // (position nets to zero, both lots count as executed volume).
    i64 mm_auth_inventory = 0, mm_auth_cash = 0, mm_auth_fees = 0;
    i64 mom_auth_inventory = 0, mom_auth_cash = 0, mom_auth_fees = 0;
    u64 mm_auth_filled_qty = 0, mom_auth_filled_qty = 0;
    u64 mm_quote_updates = 0;
    // queue-model accuracy (same unit: LOTS, on both sides):
    // inference = queue-model estimate; real = exchange-authoritative fills;
    // ambiguous = trade lots the model structurally CANNOT attribute (raced
    // our own MdNew confirmation), the honest uncertainty bound on inference
    u64 mm_inference_fill_qty = 0;
    u64 mm_real_filled_qty = 0;
    u64 mm_ambiguous_fill_qty = 0;
    u64 mom_filled_qty = 0;  // taker-archetype fill LOTS (authoritative)
    Price final_mid = 0;
    // drawdown: peak-to-trough of the MM's marked PnL across the fill
    // timeline (integer ticks*qty; peak marked at each fill delivery)
    i64 mm_marked_pnl_peak = 0;
    i64 mm_max_drawdown = 0;

    // logical latency of strategy orders: decision -> fill (ns).
    // DELIVERED-FILL CONTRACT: these histograms (and the stage decomposition
    // below) cover only fills whose execution report was actually delivered
    // and decoded. A fill report that is dropped or corrupted in transit
    // never contributes, so under nonzero exec-channel faults the histogram
    // populations are a subset of the authoritative fill counts. Trace state
    // is cleaned up at exchange terminal time, so lost terminal reports do
    // not leak per-order tables.
    Histogram fill_latency;
    // stage decomposition of decision->fill (ns), measured per completed
    // fill (taker and maker roles both recorded):
    //   decision->depart  (strategy reaction + frame encode)
    //   depart->gateway    (order channel transit incl. algo latency)
    //   gateway->match     (exchange processing)
    //   match->publish     (fill frame encode + exec channel transit)
    // The sum of the four equals fill_latency for the same fill. EVERY fill
    // of a traced order is recorded (partial maker fills included) until the
    // order reaches a terminal state (fully filled / cancel-confirmed /
    // rejected / IOC remainder), so the per-stage sums are over the SAME
    // fill set as fill_latency.
    Histogram stage_decision_to_depart;
    Histogram stage_depart_to_gateway;
    Histogram stage_gateway_to_match;
    Histogram stage_match_to_fill;
    // exact integer sums backing the histograms (verification: the four stage
    // sums must add up to the decision->fill sum for the same set of fills)
    u64 stage_sum_decision_to_depart = 0;
    u64 stage_sum_depart_to_gateway = 0;
    u64 stage_sum_gateway_to_match = 0;
    u64 stage_sum_match_to_fill = 0;
    u64 fill_latency_sum = 0;
    u64 stage_traced_fills = 0;
    u64 stage_traced_orders = 0;  // orders that entered the trace table
    // mark-out after each strategy fill: mid(+dt) - fill price, signed by side.
    // Separate histograms per strategy archetype (MM = maker-style exposure,
    // momentum = taker-style exposure), the two have structurally different
    // adverse-selection profiles and must never be pooled.
    Histogram adverse_100us;
    Histogram adverse_10ms;
    Histogram mom_adverse_100us;
    Histogram mom_adverse_10ms;

    // state-hash timeline for divergence detection: (every hash_every events)
    u64 hash_every = 256;
    std::vector<std::pair<u64, u64>> hash_timeline;  // (event_seq, state_hash)
};

std::string runstats_to_json(const RunStats& s, const std::string& name);
std::string runstats_summary(const RunStats& s, const std::string& name);

}  // namespace arbiter
