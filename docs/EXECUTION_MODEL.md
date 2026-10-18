# Execution model

## Two clocks, kept strictly apart

* **Logical time** (`LogicalNs`, int64 ns) — the deterministic simulation
  clock. ALL behavior is a function of logical time. Counterfactual
  experiments vary *logical* latencies.
* **Wall-clock time**, host measurement only, used by benchmarks and
  reported as `wall_ns` in artifacts. It NEVER influences simulation results.

This separation keeps the latency counterfactuals interpretable: the
"injected latency" is a real, deterministic pipeline delay in the logical
world, not a sleep().

## The pipeline (strategy path, fully instrumented)

```
strategy decision at t0
  | + algo_latency_ns            (strategy -> wire departure)
  | + order_channel delay        (seeded per-message draw: base + jitter,
  |                               optional reorder/dup/loss)
  v
gateway: decode, risk checks    (rejects echo back through exec channel)
  | + exchange_processing_ns
  v
matching engine (price-time priority)
  | fills produced
  | + publish_latency_ns
  v
exec_channel -> strategy on_fill
```

Market data flows the other way through the same emulated network, with a
feed handler on the strategy side (sequence tracking, gap detection,
retransmission, snapshot recovery).

Bots are co-located with the exchange: their orders skip the network and the
strategy-facing risk gateway. This is a deliberate, documented asymmetry,
the strategy path is the path under study.

## Stage semantics (the flagship's controlled stages)

Each stage below is a real, independently configurable logical delay with a
precise start/end event. The typed `LatencyAllocation` (include/arbiter/
latency.hpp) fixes the TOTAL and enforces `sum(stage_i) == total`.

| stage | starts at | ends at | owner | info available during it | market can move | trader can react |
|---|---|---|---|---|---|---|
| `md_transit_ns` | exchange MD publish | feed handler delivery | network | none to the strategy (data in flight) | yes | no |
| `decision_ns` (= `algo_latency_ns`) | strategy decision formed | order frame departs | strategy process | decision inputs (stale view) | yes | no (decision fixed) |
| `order_transit_ns` | frame departs | gateway arrival | network | decision (in flight) | yes | no |

Message-type granularity: CANCEL intents may carry their own decision
delay (`cancel_latency_ns`; defaults to "follow `algo_latency_ns`", which
is bit-identical to the historical shared behavior, test-locked). Cancel
economics differ structurally from order-entry economics (queue-exposure
termination under adverse flow), so experiments may now isolate them.
Exchange-side stages (`exchange_processing_ns`, `publish_latency_ns`,
`exec_channel`) are held FIXED across flagship cells: the flagship
question is the trader's own pipeline.

## Stage tracing (authoritative, exact)

Every strategy order is stage-traced from decision to terminal state:

```
decision -> depart -> gateway -> match -> fill
```

Stage durations are recorded per FILL (partial maker fills included) into
RunStats histograms backed by exact integer sums; the four stage sums equal
the end-to-end decision->fill sum over the same fill set (test-locked:
`stage_trace_sums_exact_with_partial_fills`). A maker fill's
`gateway->match` interval is the RESTING time until the taker arrived, the
decomposition stays exact; it is not a claim that the exchange spent that
time "processing".

Tracing is pure instrumentation: `stage_tracing = false` produces a
byte-identical state hash and identical economics (test-locked:
`world_tracing_off_matches_tracing_on`).

Known asymmetry (documented): `md_transit` precedes the decision
and is therefore not per-fill traceable; flagship cells record the
CONFIGURED md delay (channels are jitter-free by construction in flagship
runs, so configured == realized).

## Fill semantics

Price-time priority, strict FIFO within levels. Order types: LIMIT, MARKET;
TIF: GTC, IOC, FOK, GTD (expiry via logical timer events). Replace = cancel +
add (loses time priority, a documented, tested semantic). FOK is
all-or-nothing with zero side effects on rejection (tested).

## Queue-position model (no "price touched => filled" fiction)

The strategy maintains a local view of OTHERS' orders built from the MD
stream, and tracks for each of its resting orders the quantity ahead
(`ahead`) at its price level:

* own MdNew confirms the rest; `ahead` = others' qty already seen at that price
* MdTrade at a price consumes the view FIFO first, then our orders in
  confirmation order; the spillover is the *inferred* fill
* MdCancel at our price reduces `ahead` (documented approximation: observed
  removals are assumed to have been ahead)

The model's accuracy is measured against live runs, which compare inferred
fills vs real fills. The metric reports, in the same unit (lots):

* `mm_inference_fill_qty`, the point estimate,
* `mm_real_filled_qty`, the exchange-authoritative truth,
* `mm_ambiguous_fill_qty`, trade quantity that arrived while our own
  orders at that level were still UNCONFIRMED (own MdNew in flight). The
  model structurally cannot attribute those trades from its information
  set; this is the structural uncertainty bound on the point estimate.

Real fills remain authoritative for ALL economics; the inference is a model
accuracy metric only and is never used for accounting.

## Order-path limitation (v1)

The MD channel implements the full fault suite (loss/dup/reorder/burst).
The order and exec channels model delay/jitter/reorder only; a *dropped*
order or fill notification is counted but not recovered (a real client would
need send-timeouts / replay requests, future work). Defaults have zero loss
on those channels; the MD path is where faults are studied.