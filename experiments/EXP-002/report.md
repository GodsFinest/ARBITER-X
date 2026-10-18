# EXP-002: How much does latency destroy short-horizon execution alpha?

## Question

Holding predictions and market path fixed (controlled random streams), how does decision->execution latency (0us..500us) change fills, queue position value and net P&L?

## Hypothesis

H2: net P&L decays as latency grows (edge half-life vs delay); the World pair shows a localized first causal divergence with economic consequences.

## Dataset

name=synthetic:seed=2026:biased hash=7017470094089333355

## Methodology

Event-time sampling, as-of features (feat_v1), horizon labels at 1ms..1s, chronological train/valid/test split with sealed held-out tail, walk-forward stability, overlap-adaptive HAC inference, permutation nulls, 3-model queue execution accounting.

## Results

Decay (imb1):

```
   1ms n= 12159 cond=+0.000086 [7.39095e-05, 9.8875e-05] IC=+0.050
   5ms n= 12146 cond=+0.000320 [0.000252824, 0.000387393] IC=+0.029
  10ms n= 12121 cond=+0.000602 [0.000466181, 0.000738035] IC=+0.016
  25ms n= 12046 cond=+0.001467 [0.00112372, 0.00181089] IC=+0.012
  50ms n= 11936 cond=+0.002915 [0.00224539, 0.00358384] IC=+0.023
 100ms n= 11755 cond=+0.005742 [0.00448662, 0.00699766] IC=+0.013
 250ms n= 11148 cond=+0.013932 [0.012132, 0.0157318] IC=+0.074
 500ms n= 10187 cond=+0.027671 [0.0251076, 0.0302354] IC=+0.030
    1s n=  8196 cond=+0.056234 [0.0528762, 0.0595921] IC=+0.039
```

Model rows (test + sealed heldout):

```
zero      test     acc=0.184 auc=0.486 ic=+0.000 hit=0.000 cond=+0.000000 [0, 0]
zero      heldout  acc=0.177 auc=0.481 ic=+0.000 hit=0.000 cond=+0.000000 [0, 0]
prev      test     acc=0.362 auc=0.488 ic=+0.081 hit=0.519 cond=+0.000388 [-7.83396e-05, 0.000853442]
prev      heldout  acc=0.346 auc=0.394 ic=-0.191 hit=0.472 cond=-0.000067 [-0.000364922, 0.000230518]
spread    test     acc=0.469 auc=0.474 ic=-0.023 hit=0.578 cond=+0.000316 [-1.18457e-05, 0.000644689]
spread    heldout  acc=0.513 auc=0.517 ic=+0.034 hit=0.623 cond=+0.000289 [7.03097e-05, 0.000506799]
imb1      test     acc=0.377 auc=0.474 ic=-0.023 hit=0.626 cond=+0.000521 [1.61313e-05, 0.00102575]
imb1      heldout  acc=0.362 auc=0.517 ic=+0.034 hit=0.692 cond=+0.000483 [0.000173632, 0.000791559]
ofi       test     acc=0.375 auc=0.459 ic=+0.040 hit=0.469 cond=+0.000216 [-0.000172465, 0.000603837]
ofi       heldout  acc=0.417 auc=0.392 ic=-0.186 hit=0.514 cond=+0.000036 [-0.000234475, 0.000306023]
logistic  test     acc=0.560 auc=0.444 ic=-0.008 hit=0.687 cond=+0.000617 [0.000214315, 0.00102052]
logistic  heldout  acc=0.577 auc=0.503 ic=+0.019 hit=0.702 cond=+0.000503 [0.000184393, 0.000821035]
stump     test     acc=0.560 auc=0.486 ic=+0.000 hit=0.687 cond=+0.000617 [0.000214315, 0.00102052]
stump     heldout  acc=0.577 auc=0.481 ic=+0.000 hit=0.702 cond=+0.000503 [0.000184393, 0.000821035]
```

Queue-model net PnL spread:

```
conservative  net=+301.0
neutral       net=+1325.0
aggressive    net=+1325.0
```

Conditional (TEST, imb1 rule signed ret):

```
spread           tight  n=  200 mean=+0.000233 [-0.000432549, 0.000897722] hit=0.515
spread           mid    n=  232 mean=+0.000471 [-0.000362575, 0.00130526] hit=0.625
spread           wide   n=  197 mean=+0.001150 [-7.83927e-05, 0.0023779] hit=0.741
realvol_quintile q1     n=  112 mean=+0.000192 [-0.000535401, 0.000919301] hit=0.545
realvol_quintile q2     n=  161 mean=+0.002011 [0.00112092, 0.00290097] hit=0.950
realvol_quintile q3     n=  113 mean=+0.000266 [-0.000671858, 0.00120306] hit=0.460
realvol_quintile q4     n=  107 mean=+0.000410 [7.3137e-05, 0.000746591] hit=0.729
realvol_quintile q5     n=  136 mean=-0.000270 [-0.000684685, 0.000144122] hit=0.368
```

### Latency sweep (pipeline leg, neutral queue)

```
latency       0ns  net=   +1380.0
latency    1000ns  net=   +1325.0
latency    5000ns  net=   +1325.0
latency   10000ns  net=   +1325.0
latency   25000ns  net=   +1325.0
latency   50000ns  net=   +1325.0
latency  100000ns  net=   +1368.0
latency  250000ns  net=   +1057.0
latency  500000ns  net=   +1273.0
```

### Latency pair (World leg, causal divergence)

```
== experiment: exp002_latency_pair ==
  seed=2026 definition_hash=15735438348634966192
  variant                     events    trades  fills    PnL(ticks*qty)  inv     first-divergence
  baseline_800ns                 14088     1014     14            150    -150  identical
  slow_50000ns                   14078     1022     11            125    -125  @event 512
```

## Robustness

Walk-forward means, spread-conditioned buckets, and the permutation null are in results.json metrics/counters; any CI spanning zero is reported as INCONCLUSIVE/FAILED, never as alpha.

## Failure analysis

See findings in results.json (negative results preserved).

## Conclusion

IMB1@10ms TEST: SUPPORTED (robust CI above zero, pre-cost)

## Limitations

L2 level-set book approximation (no per-order queues); queue truth carried as 3-model spread; synthetic streams are stylized, not calibrated market data; Sharpe-like ratios descriptive only (overlapping samples).



## Reproducibility manifest

```json
{
  "run_id": "EXP-002/seed2026",
  "experiment_id": "EXP-002",
  "git_commit": "c08914a",
  "dataset_hash": 7017470094089333355,
  "dataset_name": "synthetic:seed=2026:biased",
  "dataset_kind": "synthetic",
  "synth_seed": 2026,
  "synth_events": 6000,
  "synth_bias": 0.59999999999999998,
  "imbalance_thresh": 0.20000000000000001,
  "exec_latency_ns": 10000,
  "fee_per_lot": 0,
  "sample_every": 1,
  "max_samples": 20000,
  "config_hash": 14435352717611632755,
  "seed": 2026,
  "feature_version": "feat_v1",
  "model_version": "model_v1",
  "code_version": "quant_v1",
  "compiler": "clang 17.0.0 (clang-1700.6.3.2)",
  "platform": "arm64-apple",
  "created_utc": "Oct 16 2026 13:34:06 UTC",
  "manifest_hash": 47474253573188289
}
```

result_hash: 3481679209062717807