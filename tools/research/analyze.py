#!/usr/bin/env python3
"""Experiment analysis: tables + (optional) plots from results.json.

Usage: python3 tools/research/analyze.py experiments/EXP-001 [--plot]
--plot needs matplotlib (optional); without it, prints tables only.
Stdlib otherwise. Reads the machine-written results.json; never edits it.
"""
import json
import sys


def main():
    exp = sys.argv[1] if len(sys.argv) > 1 else "experiments/EXP-001"
    with open(f"{exp}/results.json") as f:
        r = json.load(f)
    print(f"== {exp} ==")
    print("manifest:", json.dumps(r["manifest"], indent=1)[:400], "...")
    print("\nkey metrics:")
    for k in sorted(r["metrics"]):
        if any(s in k for s in ("_test_", "perm_", "exec_", "queue_", "decay_imb1_10ms")):
            print(f"  {k:38s} {r['metrics'][k]}")
    print("\nfindings:")
    for x in r["findings"]:
        print(f"  - {x}")
    print(f"\nresult_hash: {r['result_hash']}")
    if "--plot" in sys.argv:
        try:
            import matplotlib

            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            print("matplotlib not installed; skipping plots")
            return
        hs = ["1ms", "5ms", "10ms", "25ms", "50ms", "100ms", "250ms", "500ms", "1s"]
        xs = list(range(len(hs)))
        ys = [r["metrics"].get(f"decay_imb1_{h}_cond", float("nan")) for h in hs]
        plt.figure()
        plt.plot(xs, ys, marker="o")
        plt.xticks(xs, hs, rotation=30)
        plt.axhline(0, color="k", linewidth=0.8)
        plt.title(f"{exp}: imb1 decay (conditional mean future ret | imb1>0)")
        plt.tight_layout()
        out = f"{exp}/decay_imb1.png"
        plt.savefig(out)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()