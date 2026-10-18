# Storage

Two storage systems, both hand-rolled, both integrity-checked, neither a
database.

## Event journal (`journal/journal.cpp`)

Append-only, frame-based, designed for sequential replay:

```
file header (24B): magic "ARXJ" | version u32 | master_seed u64 | config_hash u64
frame:            payload_len u32 | crc32(payload) u32 | payload
payload:          canonical encoded Event (58B, fixed layout)
```

* journal assigns strict sequence numbers (gaps = corruption, detected)
* `flush()` → OS page cache; `sync()` → fsync. Crash boundary = last flush
  (tested at flush granularity; truncation is the modeled crash)
* reader validates magic, version, length bounds, CRC, event decoding, and
  sequence continuity; reports first failure with byte offset, never crashes
  on corrupt input
* `journal_recover` truncates to the last verifiable frame boundary
* ~66B/event measured on disk

## Snapshots / checkpoints (`journal/snapshot.cpp`)

Canonical, implementation-independent: built from the `LadderView`, so
either book implementation restores from the same file.

```
magic "ARXS" | version u32 | body_len u32 | crc32 u32 | body
body: config_hash | last_event_seq | logical_t | next_order_id |
      next_book_seq | bid levels | ask levels
```

`CheckpointStore` manages a bounded ring of `ck_<seq>.snap` files. Recovery =
newest checkpoint ≤ target + journal tail replay. Verified against full
replay at every checkpoint in the test suite.

## Wire frames (`protocol/wire.cpp`)

28B header (magic, version, flags, md_seq, channel, payload_len, CRC32) +
payload. Event frames carry the SAME canonical event encoding as the journal
— what crosses the network is byte-identical to what is written to disk,
which makes protocol fuzzing and journal fuzzing share a code path.

## Corruption policy

Every structure fails loudly with a typed error (`ChecksumMismatch`,
`JournalCorrupt`, `SnapshotInvalid`, `SequenceGap`, `ProtocolInvalid`) and a
localizing context (byte offset, sequence). Tested: bit flips, truncations,
bad magic, sequence gaps, garbage.

## Analytical output

The repository does not implement a custom columnar database. Journals and
snapshots cover deterministic execution state; research output uses explicit
versioned JSON and CSV schemas that remain easy to inspect and reproduce.