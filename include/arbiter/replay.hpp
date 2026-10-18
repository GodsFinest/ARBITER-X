#pragma once
// Journal replay: reconstruct execution state from a journal (optionally on
// top of a snapshot base), re-executing matching and cross-checking every
// journaled TradePrint against the recomputed fills. This is the mechanism
// behind crash recovery, time travel, and `arbiter verify`.

#include "arbiter/journal.hpp"
#include "arbiter/snapshot.hpp"
#include "arbiter/types.hpp"

namespace arbiter {

enum class BookImpl : u8 { Reference = 0, Optimized = 1 };

struct ReplayResult {
    Status status{};            // Ok, or the first problem found
    u64 events_seen = 0;        // total events read
    u64 events_applied = 0;     // inputs applied to the book
    u64 trade_mismatches = 0;   // journaled prints vs recomputed fills
    u64 checkpoint_mismatches = 0;
    u64 entry_outcomes_reproduced = 0;  // FOK rejects / missed cancels replayed identically
    u64 fills_recomputed = 0;
    u64 final_seq = 0;
    LogicalNs logical_t = 0;
    u64 state_hash = 0;         // canonical book hash after replay
    u64 next_order_id = 0;
};

// Replay `path` up to `until_seq` (0 = end of journal). If `base` is given,
// the book is initialized from it and events up to base->last_event_seq are
// skipped.
ReplayResult replay_journal(const std::string& path, BookImpl impl, u64 until_seq = 0,
                            const SnapshotData* base = nullptr);

}  // namespace arbiter