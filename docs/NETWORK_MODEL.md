# Network model

The network layer (`include/arbiter/net.hpp`) is a **logical, deterministic
emulation**, no sockets, no kernel, no real latency measurements of any
physical medium. It makes no claim of fidelity to any specific venue.

## Channels

| channel | id | carries | faults modeled |
|---|---|---|---|
| md (1) | exchange → strategies | CRC-framed MdNew/MdCancel/MdTrade | delay, jitter, loss, dup, reorder, burst loss, corruption |
| order (2) | strategies → gateway | CRC-framed order/cancel | delay, jitter, loss, dup, reorder, burst loss, corruption |
| exec (3) | exchange → strategies | CRC-framed fills, rejects, cancel confirms | delay, jitter, loss, dup, reorder, burst loss, corruption |
| recovery | exchange → strategy (gap path) | retransmitted MD / snapshot | deterministic base delay only (no loss, prevents recovery livelock) |

## Per-message model

```
delay    = base_delay_ns + uniform[0, jitter_ns)          (seeded draw)
loss     = Bernoulli(loss_rate); a loss may arm a burst of
           burst_loss_len further consecutive losses w.p. burst_loss_rate
dup      = Bernoulli(dup_rate) -> message delivered twice
reorder  = w.p. 1/2, extra delay of uniform[0, reorder_window) slots
corrupt  = Bernoulli(corrupt_rate) -> payload bit flip, rejected by CRC
```

Draws come from the channel's own `(master_seed, channel_id)` stream, so a
counterfactual that changes only `base_delay_ns` keeps the same unit
variates, the cleanest way to isolate latency as a variable.

## Transport sequences and packet attempts

Every frame on every channel carries a monotonic per-channel transport
sequence in its header: `md_seq` for market data (assigned at publish), and
dedicated order/exec-channel sequence counters for order intents and
execution reports. A duplicated delivery re-transmits the SAME frame with the
SAME sequence; the receiver deduplicates by decoded channel+sequence, so a
transport duplicate is dropped idempotently and never surfaces as a
business-level duplicate-order reject. Receiver dedupe state (accepted
sequences per channel) is snapshotted and hashed.

On the MD channel every transmission is a distinct PACKET ATTEMPT: the
original, a duplicate, and a retransmit are immutable packets identified by
packet id (the scheduled delivery event carries the id). A retransmit can
therefore never alias the original's delivery slot — the original's event
delivers the original frame, and the retransmit's own event delivers the
retransmit frame after its full recovery delay. Frames in transit and frames
that have already arrived at the feed handler are distinct states
(`md_packets_` vs `pending_md_`), both snapshotted and hashed.

## Feed handler semantics (strategy side)

Every MD frame carries a transport sequence (`md_seq`). The feed handler:

1. in-order delivery → apply to local view, then drain any held consecutive messages
2. `md_seq < expected` (or already held) → duplicate (retransmit race or
   post-recovery replay) → dropped
3. `md_seq > expected` → gap → hold, schedule a GapRequest after 5µs logical
4. GapRequest → exchange retransmits each missing seq as a NEW packet attempt
   from its ring buffer (4096 messages), arriving at base delay later —
   recovery latency is real, not bypassed
5. gap start older than the ring → **snapshot recovery**: the exchange sends
   the canonical ladder (minus the strategy's own orders), the view is
   rebuilt, and the stream resynchronizes

A frame enters the strategy receive buffer only after its scheduled delivery
event fires AND its CRC validates. A CRC-rejected frame is counted, journals
a Fault, and triggers the gap path like a loss. Order and execution
duplicates are receiver-deduplicated (see above); loss/corruption on those
two channels is counted but not recovered in v1.

Convergence is TESTED: with 2% MD loss + heavy jitter, every dropped message
is recovered and delivered exactly once
(`tests/test_world.cpp: world_md_gap_recovery_converges`), and the full
transport state round-trips exactly through snapshot/restore under
duplicates, loss, reordering, corruption and mid-flight recovery
(`tests/test_worldsnap.cpp: worldrestore_equivalent_under_transport_faults`).

## Fault accounting

Every fault is counted in RunStats and flows into the artifacts:

Counter contract — the SAME rules on all three channels (test-locked in
`tests/test_world.cpp: transport_counters_follow_the_contract`):

| counter | meaning |
|---|---|
| `wire_frames_encoded` | unique frame CREATIONS: a duplicated delivery re-transmits the same frame and is NOT re-encoded; a recovery retransmit IS a new frame and is counted. Dropped frames are still counted (created before loss). |
| `wire_frames_decoded` | CRC-valid deliveries ACCEPTED for processing (post transport-dedupe; suppressed duplicates are not counted) |
| `wire_frames_corrupted` | SHARED counter: receiver attempts rejected by CRC/structure, PER ATTEMPT (a duplicated corrupted frame counts once per attempt) |
| `*_dropped` (`md_`, `order_frames_`, `exec_frames_`) | frames lost before reaching the receiver (decision side) |
| `md_duplicated` / `order_frames_duplicated` / `exec_frames_duplicated` | duplicate delivery attempts SUPPRESSED at the receiver (for MD this includes racing recovery retransmits) |
| `md_reordered` | reorder decisions (MD only, decision side) |
| `md_gap_events` / `md_recovered` / `md_snapshot_recoveries` | gap detection / retransmit packets scheduled / snapshot recovery |

With `dup_rate = 1` on a channel, every frame arrives twice and the second
attempt is suppressed: `order_frames_duplicated == strategy_orders_sent +
strategy_cancels_sent` exactly, and `md_duplicated <= md_published` with the
difference bounded by the horizon tail. A corrupted frame that is ALSO
duplicated yields two corruption rejections (per attempt) for one encode.

`md_delivered <= md_published` holds at all times (a message is delivered at
most once).
