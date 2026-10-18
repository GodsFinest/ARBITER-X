# Data

Market-data ingestion, normalization, validation, and replay semantics.
Schema: `mktdata_v1` (bump on any column/semantic change).

## CSV format

Header (required):
`exchange_ts_ns,venue,instrument,type,order_id,side,price,qty,seq[,receive_ts_ns,session,level]`

- `type ∈ {trade, quote_l1, depth_l2, add, cancel, execution}`
- `side ∈ {buy, sell, bid, ask}` (bid≡buy-side, ask≡sell-side)
- `level ∈ {L1, L2, L3}`; defaults: L1 for trades/quotes, L2 for depth,
  L3 for add/cancel/execution **only when order_id is present**, else L2.
- No proprietary data is required or bundled. `data/golden/` holds tiny
  hand-verifiable fixtures; bring your own licensed captures for real studies.

## Rules (enforced, tested in tests/test_quant.cpp)

1. Timestamps/sequence/source/symbol/session are preserved end-to-end.
2. Nothing is silently repaired. `normalize()` logs every action
   (stable sort, receive-ts fill/clamp) into `NormalizationLog`, which is
   hashed into `dataset_hash` (`ARBITERX/DATASET/1`).
3. L2 is never promoted to L3. Without order ids the level is L2, and the
   queue truth is carried as the 3-model spread in execution research.
4. Malformed rows are skipped AND counted in `DataQualityReport`
   (monotonicity, duplicate seq, gaps, bad price/qty/side/type).
5. Splits are chronological (`chrono_split`); shuffling HF time series is a
   documented error (see RESEARCH.md).

## L2 reconstruction semantics (pipeline::build_series)

- `depth_l2` / `quote_l1`: ABSOLUTE level snapshot (price→qty is set; qty 0 removes).
- `add`: incremental (+qty; L3 ids tracked).
- `cancel`: L3 removes by id; L2 subtracts floored at 0.
- `trade` / `execution`: consumes qty at the traded price on the opposite
  side of the aggressor, floored at 0.
- Crossing insertions clear crossed opposite levels (they would have traded),
  which also retires stale levels when the touch migrates.
- This is an L2 APPROXIMATION (no per-order queues) — stated wherever it matters.

## CLI

- `arbiter research validate --csv F`, quality + normalization report.
- `arbiter research features --csv F --out FEATS.csv`: `features_v1` export
  for the Python layer (`tools/research/`).
- `arbiter research calibrate` / `tools/research/calibrate.py`, simulator fit.

## Public-data guidance

Any licensed L1/L2 capture (e.g. exchange historical data programs) converts
to this CSV with a thin script; sequence gaps and session boundaries must be
kept, not interpolated. True L3 (order-id) feeds are rare in public data,
when absent, say L2.