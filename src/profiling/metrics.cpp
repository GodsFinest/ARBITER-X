#include "arbiter/metrics.hpp"

#include <cstdio>
#include <string>

namespace arbiter {

std::string runstats_to_json(const RunStats& s, const std::string& name) {
    char buf[4096];
    std::string out = "{\n";
    out += "  \"name\": \"" + name + "\",\n";
    std::snprintf(buf, sizeof buf, "  \"events_processed\": %llu,\n", (unsigned long long)s.events_processed);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"journal_records\": %llu,\n", (unsigned long long)s.journal_records);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"logical_end_ns\": %lld,\n", (long long)s.logical_end);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"wall_ns\": %llu,\n", (unsigned long long)s.wall_ns);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"trades\": %llu,\n", (unsigned long long)s.trades);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"volume\": %llu,\n", (unsigned long long)s.volume);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"bot_fills\": %llu,\n", (unsigned long long)s.bot_fills);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"strategy_fills\": %llu,\n", (unsigned long long)s.strategy_fills);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"strategy_orders_sent\": %llu,\n", (unsigned long long)s.strategy_orders_sent);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"risk_accepted\": %llu,\n", (unsigned long long)s.risk_accepted);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"risk_rejected\": %llu,\n", (unsigned long long)s.risk_rejected);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"md_published\": %llu,\n", (unsigned long long)s.md_published);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"md_delivered\": %llu,\n", (unsigned long long)s.md_delivered);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"md_dropped\": %llu,\n", (unsigned long long)s.md_dropped);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"md_duplicated\": %llu,\n", (unsigned long long)s.md_duplicated);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"md_gap_events\": %llu,\n", (unsigned long long)s.md_gap_events);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"md_recovered\": %llu,\n", (unsigned long long)s.md_recovered);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"mm_inventory\": %lld,\n", (long long)s.mm_inventory);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"mm_cash_ticks_qty\": %lld,\n", (long long)s.mm_cash);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"mm_marked_pnl_ticks_qty\": %lld,\n",
                  (long long)(s.mm_cash + s.mm_inventory * s.final_mid));
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"mm_inference_fills\": %llu,\n", (unsigned long long)s.mm_inference_fill_qty);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"mm_real_filled_qty\": %llu,\n", (unsigned long long)s.mm_real_filled_qty);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"mm_ambiguous_fill_qty\": %llu,\n", (unsigned long long)s.mm_ambiguous_fill_qty);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"mm_max_drawdown\": %lld,\n", (long long)s.mm_max_drawdown);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"mm_marked_pnl_peak\": %lld,\n", (long long)s.mm_marked_pnl_peak);
    out += buf;
    std::snprintf(buf, sizeof buf, "  \"final_mid\": %lld,\n", (long long)s.final_mid);
    out += buf;
    const auto fl = s.fill_latency.summarize();
    std::snprintf(buf, sizeof buf,
                  "  \"fill_latency_ns\": {\"count\": %llu, \"p50\": %.0f, \"p99\": %.0f, \"max\": %.0f},\n",
                  (unsigned long long)fl.count, fl.p50, fl.p99, fl.mx);
    out += buf;
    const auto sd = s.stage_decision_to_depart.summarize();
    const auto sg = s.stage_depart_to_gateway.summarize();
    const auto sm = s.stage_gateway_to_match.summarize();
    const auto sf = s.stage_match_to_fill.summarize();
    std::snprintf(buf, sizeof buf,
                   "  \"stage_ns\": {\"decision_to_depart\": {\"count\": %llu, \"p50\": %.0f, \"p99\": %.0f},\n"
                   "                \"depart_to_gateway\": {\"count\": %llu, \"p50\": %.0f, \"p99\": %.0f},\n"
                   "                \"gateway_to_match\": {\"count\": %llu, \"p50\": %.0f, \"p99\": %.0f},\n"
                   "                \"match_to_fill\": {\"count\": %llu, \"p50\": %.0f, \"p99\": %.0f}},\n",
                   (unsigned long long)sd.count, sd.p50, sd.p99,
                   (unsigned long long)sg.count, sg.p50, sg.p99,
                   (unsigned long long)sm.count, sm.p50, sm.p99,
                   (unsigned long long)sf.count, sf.p50, sf.p99);
    out += buf;
    const auto ad = s.adverse_100us.summarize();
    std::snprintf(buf, sizeof buf,
                  "  \"adverse_100us\": {\"count\": %llu, \"mean\": %.1f},\n",
                  (unsigned long long)ad.count, ad.mean);
    out += buf;
    const auto ad10 = s.adverse_10ms.summarize();
    std::snprintf(buf, sizeof buf,
                  "  \"adverse_10ms\": {\"count\": %llu, \"mean\": %.1f},\n",
                  (unsigned long long)ad10.count, ad10.mean);
    out += buf;
    const auto mad = s.mom_adverse_100us.summarize();
    std::snprintf(buf, sizeof buf,
                  "  \"mom_adverse_100us\": {\"count\": %llu, \"mean\": %.1f},\n",
                  (unsigned long long)mad.count, mad.mean);
    out += buf;
    const auto mad10 = s.mom_adverse_10ms.summarize();
    std::snprintf(buf, sizeof buf,
                  "  \"mom_adverse_10ms\": {\"count\": %llu, \"mean\": %.1f}\n",
                  (unsigned long long)mad10.count, mad10.mean);
    out += buf;
    out += "}";
    return out;
}

std::string runstats_summary(const RunStats& s, const std::string& name) {
    char buf[512];
    std::string out = "== " + name + " ==\n";
    std::snprintf(buf, sizeof buf, "  events: %llu  logical_end: %lld ns  wall: %.1f ms\n",
                  (unsigned long long)s.events_processed, (long long)s.logical_end,
                  s.wall_ns / 1e6);
    out += buf;
    std::snprintf(buf, sizeof buf, "  trades: %llu (vol %llu)  bot fills: %llu  strategy fills: %llu\n",
                  (unsigned long long)s.trades, (unsigned long long)s.volume,
                  (unsigned long long)s.bot_fills, (unsigned long long)s.strategy_fills);
    out += buf;
    std::snprintf(buf, sizeof buf, "  md: pub %llu / del %llu / drop %llu / dup %llu / gaps %llu / recov %llu\n",
                  (unsigned long long)s.md_published, (unsigned long long)s.md_delivered,
                  (unsigned long long)s.md_dropped, (unsigned long long)s.md_duplicated,
                  (unsigned long long)s.md_gap_events, (unsigned long long)s.md_recovered);
    out += buf;
    std::snprintf(buf, sizeof buf, "  risk: accept %llu / reject %llu\n",
                  (unsigned long long)s.risk_accepted, (unsigned long long)s.risk_rejected);
    out += buf;
    std::snprintf(buf, sizeof buf, "  MM: inv %lld  cash %lld  marked PnL %lld (ticks*qty)\n",
                  (long long)s.mm_inventory, (long long)s.mm_cash,
                  (long long)(s.mm_cash + s.mm_inventory * s.final_mid));
    out += buf;
    if (s.mm_max_drawdown > 0 || s.mm_marked_pnl_peak != 0) {
        std::snprintf(buf, sizeof buf, "  MM drawdown: max %lld (peak %lld, marked at each fill)\n",
                      (long long)s.mm_max_drawdown, (long long)s.mm_marked_pnl_peak);
        out += buf;
    }
    // queue-model accuracy: both sides in LOTS; ambiguity = the lots the
    // model cannot attribute (races with our own MdNew); the band is reported
    if (s.mm_inference_fill_qty || s.mm_real_filled_qty) {
        const double ratio = s.mm_real_filled_qty
                                 ? static_cast<double>(s.mm_inference_fill_qty) /
                                       static_cast<double>(s.mm_real_filled_qty)
                                 : 0.0;
        std::snprintf(buf, sizeof buf,
                      "  queue model: inferred %llu / real %llu lots (ratio %.2f; 1.0 = exact; ambiguous %llu)\n",
                      (unsigned long long)s.mm_inference_fill_qty,
                      (unsigned long long)s.mm_real_filled_qty, ratio,
                      (unsigned long long)s.mm_ambiguous_fill_qty);
        out += buf;
    }
    const auto fl = s.fill_latency.summarize();
    if (fl.count) {
        std::snprintf(buf, sizeof buf, "  fill latency ns: p50 %.0f p99 %.0f p99.9 %.0f max %.0f (n=%llu)\n",
                      fl.p50, fl.p99, fl.p999, fl.mx, (unsigned long long)fl.count);
        out += buf;
    }
    const auto ad = s.adverse_100us.summarize();
    if (ad.count) {
        std::snprintf(buf, sizeof buf, "  markout 100us: mean %+.1f p50 %+.1f (ticks, + = fill was good)\n",
                      ad.mean, ad.p50);
        out += buf;
    }
    const auto mad = s.mom_adverse_100us.summarize();
    if (mad.count) {
        std::snprintf(buf, sizeof buf, "  taker markout 100us: mean %+.1f p50 %+.1f (ticks, + = fill was good)\n",
                      mad.mean, mad.p50);
        out += buf;
    }
    const auto sd = s.stage_decision_to_depart.summarize();
    if (sd.count) {
        const auto sg = s.stage_depart_to_gateway.summarize();
        const auto sm = s.stage_gateway_to_match.summarize();
        const auto sf = s.stage_match_to_fill.summarize();
        std::snprintf(buf, sizeof buf,
                      "  stage ns: d->dep %.0f/%.0f  dep->gw %.0f/%.0f  "
                      "gw->match %.0f/%.0f  match->fill %.0f/%.0f (p50/p99, n=%llu)\n",
                      sd.p50, sd.p99, sg.p50, sg.p99, sm.p50, sm.p99, sf.p50, sf.p99,
                      (unsigned long long)sd.count);
        out += buf;
    }
    return out;
}

}  // namespace arbiter