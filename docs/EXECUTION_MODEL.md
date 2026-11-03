# Execution model

## Two clocks, kept strictly apart

* **Logical time** (`LogicalNs`, int64 ns) — the deterministic simulation
  clock. All behavior is a function of logical time. Counterfactual
  experiments vary *logical* latencies.
* **Wall-clock time**, host measurement only, used by benchmarks and
  reported as `wall_ns` in artifacts. It never influences simulation results.

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
add (loses time priority, a documented, tested semantic; the original order,
its queue position and its hash are preserved unchanged when the replacement
is invalid). FOK is all-or-nothing with zero side effects on rejection
(tested); a FOK replacement excludes the original's own contra liquidity from
its availability check. A fully executable resting order is admitted even
when the book is at its active-order cap; a resting entry that would not
fully execute is rejected with zero fills and zero mutation.

## Exchange-authoritative vs strategy-local state

Two views are kept deliberately apart:

* **Exchange-authoritative (match time).** Gateway risk positions and the
  per-account economics ledger (`*_auth_*` metrics: position, cash, fees,
  filled quantity) are updated for every fill leg at MATCH time, whether or
  not the execution report survives the exec channel. A lost or corrupted
  report can never make these stale. Self-trades record BOTH legs truthfully.
  Working-order exposure is RESERVED at gateway acceptance (full quantity,
  per account and order id) and consumed/released at exchange time (fill,
  IOC/FOK remainder, cancel, GTD expiry, book-admission reject) — so two
  working orders cannot jointly breach the position cap, and reservation
  release never depends on report delivery.
* **Strategy-local (notification-driven).** Strategy inventory/cash, queue
  model and pending-order bookkeeping update only on successfully decoded
  reports. Under an intentionally faulty exec channel they may go stale;
  that staleness is a measured fault effect, not a bug, and it can influence
  subsequent strategy decisions (which is the point of the fault study).

The authoritative economics fields (`mm_auth_*`, `mom_auth_*` in the JSON and the
`MM/MOM auth (exchange ledger)` summary rows) come from the authoritative
ledger; the strategy-local fields are labeled as the behavioral view.
The preserved fixed-total flagship uses strategy-local cash and inventory at
its horizon for its marked-PnL endpoint (`run_one` in flagship.cpp), alongside
separately reported fill quantities. It does not silently substitute the
exchange ledger or drain all in-flight reports. Changing that endpoint would
require a separate experimental definition and new results.

## Terminal semantics and trace cleanup

Every order reaches an exchange-side terminal state (fully filled, IOC/FOK
remainder canceled, cancel confirmed, GTD expiry, rejected). That terminal
bookkeeping happens at the exchange, NOT on report delivery. Per-order
latency-trace state is cleaned up at exchange terminal time when no fill
report for the order is still in flight, so lost or corrupted terminal
reports cannot leak `stages_`/`order_sent_t_` even at 100% exec loss
(test-locked: `world_100pct_exec_loss_bounds_per_order_trace_state`). A
delivered fill report arriving after a terminal report is still processed
exactly once (transport dedupe), and terminal reports are sent to both
gateway strategies (momentum IOC remainders included).

Delivery-latency histograms (`fill_latency`, the stage decomposition, and
the adverse-selection markout probes) cover only fills whose execution
report was actually delivered and decoded — a documented, tested contract
(`metrics.hpp` "DELIVERED-FILL CONTRACT"). Under nonzero exec faults their
populations are a subset of the authoritative fill counts.

## Book-admission rejects are terminal

Risk bounds and book bounds are configured independently, so a risk-accepted
order can fail `book_.add_order` (out-of-span price, duplicate, capacity).
Such a reject is terminal and observable: it is NOT journaled as a BookAdd,
it journals a `Fault` (kind 4) carrying the order id and reason, a terminal
reject report is sent to the strategy, the risk reservation is released, and
exchange-side trace state is cleaned up immediately (test-locked:
`world_book_admission_reject_is_terminal`). A lost reject report may leave
the strategy's local view stale, never the authoritative exchange/risk state.

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

All three channels implement delay, jitter, loss, duplication, reordering,
burst loss, and CRC-detected corruption. MD gaps are recovered from a
retransmit ring or snapshot. A dropped/corrupted order or execution report is
not recovered in v1 (a real client would need send-timeouts or replay
requests); duplicates are receiver-deduplicated. Defaults keep order/exec
loss at zero, and the MD path remains the primary fault-study path.
