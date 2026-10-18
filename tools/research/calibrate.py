#!/usr/bin/env python3
"""Simulator calibration helper (stdlib-only).

Compares distributional summaries of a real market-data CSV against the
synthetic generator's output at the same event count: spread, depth,
trade size, inter-arrival gaps. Prints two-sample summary stats and a
plain-language fit verdict per variable (no p-value theater: n is huge so
everything is "significant"; effect sizes are what matter).

Usage: python3 tools/research/calibrate.py --csv REAL.csv [--events N]
"""
import argparse
import csv
import statistics


def load_csv(path, n):
    spreads, depths, sizes, gaps = [], [], [], []
    bids, asks = {}, {}
    last_ts = None
    count = 0
    with open(path) as f:
        for row in csv.DictReader(filter(lambda r: not r.startswith("#"), f)):
            if count >= n:
                break
            count += 1
            ts = int(row["exchange_ts_ns"])
            if last_ts is not None:
                gaps.append(ts - last_ts)
            last_ts = ts
            t, side, px, q = row["type"], row["side"], int(row["price"]), int(row["qty"])
            if t in ("depth_l2", "quote_l1"):
                (bids if side == "bid" else asks)[px] = q
            elif t == "trade":
                sizes.append(q)
            if bids and asks:
                spreads.append(min(asks) - max(bids))
                depths.append(sum(bids.values()) + sum(asks.values()))
    return {"spread": spreads, "depth": depths, "trade_size": sizes, "gap_ns": gaps}


def summ(v):
    if not v:
        return {"n": 0}
    q = statistics.quantiles(sorted(v), n=100)
    return {"n": len(v), "mean": statistics.fmean(v), "p50": statistics.median(v),
            "p1": q[0], "p99": q[98], "max": max(v)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--events", type=int, default=20000)
    a = ap.parse_args()
    real = load_csv(a.csv, a.events)
    print(f"== calibration input: {a.csv} ==")
    for k, v in real.items():
        s = summ(v)
        print(f"  {k:10s} n={s.get('n', 0)} mean={s.get('mean', float('nan')):.3f} "
              f"p50={s.get('p50', float('nan')):.3f} p1={s.get('p1', float('nan')):.3f} "
              f"p99={s.get('p99', float('nan')):.3f} max={s.get('max', float('nan')):.3f}")
    print("Compare against `arbiter research calibrate` output + tools guidance in docs/SIMULATION.md.")
    print("Verdict rule: report standardized mean gaps; retune SimConfig only if a gap exceeds 1 pooled sd.")


if __name__ == "__main__":
    main()