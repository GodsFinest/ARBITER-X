#pragma once
// Canonical state snapshots, the checkpoint mechanism for crash recovery and
// time travel. A snapshot is implementation-independent: it is built from the
// canonical LadderView, so either book implementation can be restored from it.
//
// Layout (v1): magic "ARXS" | version u32 | body_len u32 | crc32(body) u32 | body
//   body: config_hash u64 | last_event_seq u64 | logical_t i64 | next_order_id u64
//         | next_book_seq u64 | bid levels, ask levels (canonical encoding)

#include "arbiter/book_common.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/types.hpp"

#include <string>
#include <vector>

namespace arbiter {

struct SnapshotData {
    u64 config_hash = 0;
    u64 last_event_seq = 0;
    LogicalNs logical_t = 0;
    u64 next_order_id = 0;
    u64 next_book_seq = 0;
    LadderView ladder;
};

Status write_snapshot(const std::string& path, const SnapshotData& snap);
Result<SnapshotData> read_snapshot(const std::string& path);

// Directory of ck_<seq>.snap files with a bounded retention ring.
class CheckpointStore {
public:
    CheckpointStore(std::string dir, size_t keep = 8);

    Status save(const SnapshotData& snap);  // names file by last_event_seq
    // Newest snapshot with last_event_seq <= seq. NotFound if none.
    Result<SnapshotData> load_at_or_before(u64 seq) const;
    std::vector<u64> available_seqs() const;
    const std::string& dir() const noexcept { return dir_; }

private:
    std::string dir_;
    size_t keep_;
};

}  // namespace arbiter