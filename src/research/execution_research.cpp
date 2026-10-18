// Execution economics: queue models, cost accounting, decision gates.
#include "arbiter/execution_research.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace arbiter {
namespace execresearch {

const char* to_cstr(QueueModel m) noexcept {
    switch (m) {
        case QueueModel::Conservative: return "conservative";
        case QueueModel::Neutral: return "neutral";
        case QueueModel::Aggressive: return "aggressive";
    }
    return "?";
}

Qty queue_fill_lots(QueueModel m, Qty ahead, Qty our_qty, Qty traded_qty) {
    if (our_qty == 0) return 0;
    Qty need = 0;
    switch (m) {
        case QueueModel::Conservative: need = 2 * (ahead + our_qty); break;
        case QueueModel::Neutral: need = ahead + our_qty; break;
        case QueueModel::Aggressive: need = ahead; break;
    }
    if (traded_qty < need) {
        // partial credit: filled fraction scales with prints past `ahead`
        if (traded_qty <= ahead) return 0;
        const double frac =
            static_cast<double>(traded_qty - ahead) / static_cast<double>(need - ahead);
        Qty f = static_cast<Qty>(frac * static_cast<double>(our_qty) + 0.5);  // round-half-up
        return f > our_qty ? our_qty : f;
    }
    return our_qty;
}

bool should_trade(double expected_move_ticks, double spread_cost_ticks, double fee_ticks,
                  double fill_prob, double confidence) {
    return confidence * fill_prob * expected_move_ticks > spread_cost_ticks + fee_ticks;
}

ExecReport evaluate(const std::vector<Opportunity>& opps, const std::vector<double>& horizon_mid,
                    const std::vector<double>& markout_mid, QueueModel q, CostModel c) {
    ExecReport r;
    r.queue = q;
    r.opportunities = opps.size();
    std::vector<double> per_trade;
    per_trade.reserve(opps.size());
    double peak = 0.0, cur = 0.0, dd = 0.0, adv_sum = 0.0;
    size_t adv_n = 0, gross_w = 0, gross_l = 0;
    for (size_t i = 0; i < opps.size(); ++i) {
        const Opportunity& o = opps[i];
        const Qty fq = queue_fill_lots(q, o.ahead, o.qty, o.traded_through);
        if (fq == 0) continue;  // missed fill: opportunity cost is implicit (no position)
        const double hm = i < horizon_mid.size() ? horizon_mid[i] : o.exec_px;
        const double s = is_buy(o.side) ? 1.0 : -1.0;
        const double gross = s * (hm - static_cast<double>(o.exec_px)) * static_cast<double>(fq);
        const double fees = static_cast<double>(c.fee_per_lot) * static_cast<double>(fq);
        const double slip = c.slippage_ticks * static_cast<double>(fq);
        const double net = gross - fees - slip;
        r.fills++;
        r.filled_lots += fq;
        r.gross_pnl += static_cast<i64>(std::llround(gross));
        r.fees += static_cast<i64>(std::llround(fees + slip));
        per_trade.push_back(net);
        cur += net;
        if (cur > peak) peak = cur;
        if (peak - cur > dd) dd = peak - cur;
        if (net > 0) {
            gross_w += static_cast<u64>(net);
        } else if (net < 0) {
            gross_l += static_cast<u64>(-net);
        }
        if (i < markout_mid.size() && markout_mid[i] > 0.0 && o.exec_px > 0) {
            // adverse selection: negative signed markout = price moved against us
            adv_sum += s * (markout_mid[i] - static_cast<double>(o.exec_px));
            ++adv_n;
        }
    }
    r.net_pnl = r.gross_pnl - r.fees;
    r.turnover_lots = static_cast<double>(r.filled_lots);
    if (r.opportunities) r.fill_rate = static_cast<double>(r.fills) / r.opportunities;
    if (r.fills) r.avg_trade = static_cast<double>(r.net_pnl) / r.fills;
    if (!per_trade.empty()) {
        // hit rate on filled trades
        size_t h = 0;
        double m = 0.0;
        for (double v : per_trade) {
            m += v;
            if (v > 0) ++h;
        }
        m /= per_trade.size();
        r.hit_rate = static_cast<double>(h) / per_trade.size();
        double sd = 0.0;
        for (double v : per_trade) sd += (v - m) * (v - m);
        sd = per_trade.size() > 1 ? std::sqrt(sd / (per_trade.size() - 1)) : 0.0;
        // DESCRIPTIVE ONLY: samples overlap in event time; do not use for inference.
        r.sharpe_like = sd > 0.0 ? m / sd * std::sqrt(252.0) : 0.0;
        r.profit_factor = gross_l > 0 ? static_cast<double>(gross_w) / gross_l : (gross_w > 0 ? 1e9 : 0.0);
    }
    r.max_drawdown = dd;
    r.adverse_ticks = adv_n ? adv_sum / adv_n : 0.0;
    return r;
}

std::string ExecReport::to_string() const {
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "queue=%s opps=%zu fills=%zu lots=%llu fill_rate=%.3f gross=%lld fees=%lld "
                  "net=%lld avg=%.2f hit=%.3f pf=%.2f dd=%.1f adverse=%+.4f ticks sharpe~%.2f(descriptive)",
                  to_cstr(queue), opportunities, fills, (unsigned long long)filled_lots,
                  fill_rate, (long long)gross_pnl, (long long)fees, (long long)net_pnl, avg_trade,
                  hit_rate, profit_factor, max_drawdown, adverse_ticks, sharpe_like);
    return std::string(buf);
}

std::string LatencyBudget::to_string() const {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "md=%lld feat=%lld infer=%lld decide=%lld ser=%lld net=%lld gw=%lld risk=%lld "
                  "match=%lld total=%lld ns",
                  (long long)md_ingest_ns, (long long)feature_ns, (long long)inference_ns,
                  (long long)decision_ns, (long long)serialize_ns, (long long)network_ns,
                  (long long)gateway_ns, (long long)risk_ns, (long long)matching_ns,
                  (long long)total());
    return std::string(buf);
}

}  // namespace execresearch
}  // namespace arbiter