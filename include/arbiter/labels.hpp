#pragma once
// Labeling framework: multiple research targets at multiple horizons.
//
// HORIZONS_NS is the documented horizon grid (1ms..1s). Every label is
// defined PURELY from mids strictly AFTER the feature time t (t+h) versus
// information AT t, the labeler takes an explicit mid series and an index,
// so leakage is structurally visible: label_at(i, h) may only read indices
// >= i. Tests mutate post-horizon mids and require pre-horizon features to
// be unchanged (and vice versa: mutating the future must change the label).
//
// Targets:
//   ret_h      = log(mid[t+h]/mid[t])            (double, NaN->missing if either mid<=0)
//   dir_h      = sign(ret_h) in {-1,0,+1}
//   thresh_h   = +1 if ret > +k*spread_rel, -1 if ret < -k*spread_rel else 0
//   markout_h  = (mid[t+h]-px)*side_sign / px    (execution research: signed
//                relative markout of a fill at price px)

#include "arbiter/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arbiter {
namespace labels {

inline constexpr i64 kHorizonsNs[] = {1'000'000,   5'000'000,  10'000'000, 25'000'000,
                                      50'000'000,  100'000'000, 250'000'000, 500'000'000,
                                      1'000'000'000};
inline constexpr size_t kNumHorizons = 9;
// Markout horizons (subset relevant to execution/adverse selection).
inline constexpr i64 kMarkoutHorizonsNs[] = {1'000'000,  5'000'000,  10'000'000, 50'000'000,
                                             100'000'000, 500'000'000, 1'000'000'000};
inline constexpr size_t kNumMarkouts = 7;

struct MidSample {
    i64 ts_ns = 0;
    double mid = 0.0;  // ticks (double for log math; source is integer ticks)
};

// As-of mid lookup: last sample with ts <= as_of (binary search; series must
// be ts-ordered). Returns 0.0 when none exists.
double mid_at(const std::vector<MidSample>& s, i64 as_of_ns);
// First sample with ts >= target (for horizon reads). Returns 0.0 when none.
double mid_at_or_after(const std::vector<MidSample>& s, i64 target_ns);
// Linear interpolation between bracketing samples (EXECUTION PRICING ONLY:
// real markets move continuously, so sub-sample latencies need sub-sample
// prices; statistics/labels always use sampled mids, never this).
double mid_at_interp(const std::vector<MidSample>& s, i64 t_ns);

double log_return(double mid_t, double mid_th);  // NaN-safe wrapper (missing -> 0 + flag)
bool missing(double mid_t, double mid_th);
int direction(double ret);                        // {-1,0,+1}
int thresholded(double ret, double spread_rel, double k = 0.5);
// Signed relative markout of a fill: side Buy=+1.
double markout(double fill_px, Side fill_side, double mid_th);

struct LabelSet {
    bool valid = false;
    double ret = 0.0;
    int dir = 0;
    int thresh = 0;
};

// Labels for feature time t against horizon h (series must extend past t+h;
// valid=false when either endpoint is missing).
LabelSet label_at(const std::vector<MidSample>& s, i64 t_ns, i64 horizon_ns,
                  double spread_rel_at_t, double thresh_k = 0.5);

std::string horizon_name(i64 horizon_ns);  // e.g. "10ms", "1s"

}  // namespace labels
}  // namespace arbiter