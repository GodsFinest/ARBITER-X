# Network model

The network layer (`include/arbiter/net.hpp`) is a **logical, deterministic
emulation**, no sockets, no kernel, no real latency measurements of any
physical medium. It makes no claim of fidelity to any specific venue.

## Channels

| channel | id | carries | faults modeled |
|---|---|---|---|
| md (1) | exchange → strategies | MdNew/MdCancel/MdTrade frames | delay, jitter, loss, dup, reorder, burst loss |
| order (2) | strategies → gateway | order/cancel frames | delay, jitter, reorder (loss counted, not recovered, v1) |
| exec (3) | exchange → strategies | fills, rejects, cancel confirms | delay, jitter, reorder (same v1 note) |
| recovery | exchange → strategy (gap path) | retransmitted MD / snapshot | deterministic base delay only (no loss, prevents recovery livelock) |

## Per-message model

```
delay    = base_delay_ns + uniform[0, jitter_ns)          (seeded draw)
loss     = Bernoulli(loss_rate); a loss may arm a burst of
           burst_loss_len further consecutive losses w.p. burst_loss_rate
dup      = Bernoulli(dup_rate) -> message delivered twice
reorder  = w.p. 1/2, extra delay of uniform[0, reorder_window) slots
```

Draws come from the channel's own `(master_seed, channel_id)` stream, so a
counterfactual that changes only `base_delay_ns` keeps the same unit
variates, the cleanest way to isolate latency as a variable.

## Feed handler semantics (strategy side)

Every MD frame carries a transport sequence (`md_seq`). The feed handler:

1. in-order delivery → apply to local view, then drain any held consecutive messages
2. `md_seq < expected` → duplicate (post-recovery) → dropped
3. `md_seq > expected` → gap → hold, schedule a GapRequest after 5µs logical
4. GapRequest → exchange retransmits missing seqs from its ring buffer
   (4096 messages) over the recovery channel
5. gap start older than the ring → **snapshot recovery**: the exchange sends
   the canonical ladder (minus the strategy's own orders), the view is
   rebuilt, and the stream resynchronizes

Convergence is TESTED: with 2% MD loss + heavy jitter, every dropped message
is recovered and delivered exactly once
(`tests/test_world.cpp: world_md_gap_recovery_converges`, measured
`md_recovered == md_dropped`, `md_delivered == md_published`).

## Session semantics

A drop always produces either a gap-and-recovery or a snapshot recovery;
nothing is silently swallowed. Every fault is counted in RunStats
(`md_dropped`, `md_duplicated`, `md_reordered`, `md_gap_events`,
`md_recovered`, `md_snapshot_recoveries`) and flows into the artifacts.