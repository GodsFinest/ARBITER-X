# EXP-002: How much does latency destroy short-horizon execution alpha?

## Question

Holding predictions and market path fixed (controlled random streams), how does decision->execution latency (0us..500us) change fills, queue position value and net P&L?

## Hypothesis

H2: net P&L decays as latency grows (edge half-life vs delay); the World pair shows a localized first divergent checkpoint with economic consequences.

## Dataset

name=synthetic:seed=2026:biased hash=7017470094089333355

## Methodology

Event-time sampling, as-of features (feat_v2), horizon labels at 1ms..1s, chronological train/valid/test split with sealed held-out tail, walk-forward stability, overlap-adaptive HAC inference, permutation nulls, 3-model queue execution accounting.

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
zero      test     acc=0.184 auc=0.500 ic=+0.000 hit=0.000 cond=+0.000000 [0, 0]
zero      heldout  acc=0.153 auc=0.500 ic=+0.000 hit=0.000 cond=+0.000000 [0, 0]
prev      test     acc=0.358 auc=0.480 ic=+0.073 hit=0.530 cond=+0.000396 [-6.94641e-05, 0.000862023]
prev      heldout  acc=0.354 auc=0.397 ic=-0.182 hit=0.459 cond=-0.000084 [-0.000388529, 0.000220016]
spread    test     acc=0.480 auc=0.490 ic=-0.014 hit=0.592 cond=+0.000344 [1.48724e-05, 0.000672397]
spread    heldout  acc=0.523 auc=0.509 ic=+0.032 hit=0.616 cond=+0.000285 [6.39551e-05, 0.000505864]
imb1      test     acc=0.394 auc=0.490 ic=-0.014 hit=0.643 cond=+0.000558 [6.64662e-05, 0.00104914]
imb1      heldout  acc=0.353 auc=0.509 ic=+0.032 hit=0.688 cond=+0.000488 [0.000168337, 0.000806705]
ofi       test     acc=0.386 auc=0.455 ic=+0.031 hit=0.483 cond=+0.000233 [-0.000153565, 0.000620129]
ofi       heldout  acc=0.422 auc=0.402 ic=-0.177 hit=0.504 cond=+0.000024 [-0.000248188, 0.00029675]
logistic  test     acc=0.566 auc=0.508 ic=+0.053 hit=0.695 cond=+0.000636 [0.000231751, 0.00104093]
logistic  heldout  acc=0.594 auc=0.521 ic=+0.074 hit=0.701 cond=+0.000511 [0.000189425, 0.000833155]
stump     test     acc=0.566 auc=0.500 ic=+0.000 hit=0.695 cond=+0.000636 [0.000231751, 0.00104093]
stump     heldout  acc=0.594 auc=0.500 ic=+0.000 hit=0.701 cond=+0.000511 [0.000189425, 0.000833155]
```

Queue-model net PnL spread:

```
conservative  net=+369.0
neutral       net=+1605.0
aggressive    net=+1605.0
```

Conditional (TEST, imb1 rule signed ret):

```
spread           tight  n=  197 mean=+0.000277 [-0.000397173, 0.000951032] hit=0.533
spread           mid    n=  227 mean=+0.000456 [-0.00038046, 0.00129326] hit=0.617
spread           wide   n=  221 mean=+0.001167 [7.81116e-05, 0.00225641] hit=0.769
realvol_quintile q1     n=  115 mean=+0.000147 [-0.000246999, 0.000540884] hit=0.583
realvol_quintile q2     n=  165 mean=+0.002122 [0.00134649, 0.00289677] hit=0.933
realvol_quintile q3     n=  110 mean=-0.000057 [-0.000621101, 0.000506687] hit=0.445
realvol_quintile q4     n=  119 mean=+0.000775 [0.000203875, 0.00134581] hit=0.798
realvol_quintile q5     n=  136 mean=-0.000270 [-0.000684685, 0.000144122] hit=0.368
```

### Latency sweep (pipeline leg, neutral queue)

```
latency       0ns  net=   +1655.0
latency    1000ns  net=   +1605.0
latency    5000ns  net=   +1605.0
latency   10000ns  net=   +1605.0
latency   25000ns  net=   +1605.0
latency   50000ns  net=   +1605.0
latency  100000ns  net=   +1648.0
latency  250000ns  net=   +1317.0
latency  500000ns  net=   +1533.0
```

### Latency pair (World leg, checkpoint divergence)

```
== experiment: exp002_latency_pair ==
  seed=2026 definition_hash=2359450862122252481
  variant                     events    trades  fills    PnL(ticks*qty)  inv     first-divergent-checkpoint
  baseline_800ns                 14183     1014     14            150    -150  identical
  slow_50000ns                   14167     1022     11            125    -125  @checkpoint(event 512)
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
  "git_commit": "de4b63d",
  "git_dirty": false,
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
  "config_hash": 4816988207365062720,
  "seed": 2026,
  "feature_version": "feat_v2",
  "model_version": "model_v2",
  "code_version": "quant_v2",
  "compiler": "clang 17.0.0 (clang-1700.6.3.2)",
  "platform": "arm64-apple",
  "created_utc": "Nov  1 2026 12:08:35 UTC",
  "manifest_hash": 5858694801072405923
}
```

result_hash: 4163705605711638385
