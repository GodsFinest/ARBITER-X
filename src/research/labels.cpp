// Labels: horizon returns, direction, thresholded moves, markouts.
#include "arbiter/labels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace arbiter {
namespace labels {

double mid_at(const std::vector<MidSample>& s, i64 as_of_ns) {
    // last sample with ts <= as_of (series ts-ordered)
    double m = 0.0;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        const size_t md = lo + (hi - lo) / 2;
        if (s[md].ts_ns <= as_of_ns) {
            m = s[md].mid;
            lo = md + 1;
        } else {
            hi = md;
        }
    }
    return m;
}

double mid_at_or_after(const std::vector<MidSample>& s, i64 target_ns) {
    for (const auto& sm : s) {
        if (sm.ts_ns >= target_ns) return sm.mid;
    }
    return 0.0;
}

double mid_at_interp(const std::vector<MidSample>& s, i64 t_ns) {
    if (s.empty()) return 0.0;
    if (t_ns <= s.front().ts_ns) return s.front().mid;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i].ts_ns >= t_ns) {
            const double m0 = s[i - 1].mid, m1 = s[i].mid;
            const i64 t0 = s[i - 1].ts_ns, t1 = s[i].ts_ns;
            if (!(m0 > 0.0 && m1 > 0.0) || t1 <= t0) return m1 > 0.0 ? m1 : m0;
            const double f = static_cast<double>(t_ns - t0) / static_cast<double>(t1 - t0);
            return m0 + f * (m1 - m0);
        }
    }
    return s.back().mid;
}

bool missing(double mid_t, double mid_th) { return !(mid_t > 0.0 && mid_th > 0.0); }

double log_return(double mid_t, double mid_th) {
    if (missing(mid_t, mid_th)) return 0.0;
    return std::log(mid_th / mid_t);
}

int direction(double ret) {
    if (ret > 0.0) return 1;
    if (ret < 0.0) return -1;
    return 0;
}

int thresholded(double ret, double spread_rel, double k) {
    if (spread_rel <= 0.0) return direction(ret);
    const double thr = k * spread_rel;
    if (ret > thr) return 1;
    if (ret < -thr) return -1;
    return 0;
}

double markout(double fill_px, Side fill_side, double mid_th) {
    if (!(fill_px > 0.0 && mid_th > 0.0)) return 0.0;
    const double s = is_buy(fill_side) ? 1.0 : -1.0;
    return s * (mid_th - fill_px) / fill_px;
}

LabelSet label_at(const std::vector<MidSample>& s, i64 t_ns, i64 horizon_ns,
                  double spread_rel_at_t, double thresh_k) {
    LabelSet l;
    const double mt = mid_at(s, t_ns);
    const double mh = mid_at_or_after(s, t_ns + horizon_ns);
    if (missing(mt, mh)) return l;
    l.valid = true;
    l.ret = log_return(mt, mh);
    l.dir = direction(l.ret);
    l.thresh = thresholded(l.ret, spread_rel_at_t, thresh_k);
    return l;
}

std::string horizon_name(i64 horizon_ns) {
    char buf[32];
    if (horizon_ns % 1'000'000'000 == 0)
        std::snprintf(buf, sizeof buf, "%llds", (long long)(horizon_ns / 1'000'000'000));
    else if (horizon_ns % 1'000'000 == 0)
        std::snprintf(buf, sizeof buf, "%lldms", (long long)(horizon_ns / 1'000'000));
    else
        std::snprintf(buf, sizeof buf, "%lldus", (long long)(horizon_ns / 1000));
    return std::string(buf);
}

}  // namespace labels
}  // namespace arbiter