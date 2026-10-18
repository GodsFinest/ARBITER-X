#pragma once
// Append-only deterministic event journal.
//
// File layout (v1):
//   FileHeader: magic "ARXJ" | version u32 | master_seed u64 | config_hash u64
//   Frame:      payload_len u32 | crc32(payload) u32 | payload
//   payload = encode_event(Event): 56 bytes, includes the event's sequence.
//
// Durability semantics (documented contract, tested in tests/test_journal.cpp):
//   * append() buffers in userspace; data is only crash-durable after flush()
//     (writes to OS) or sync() (fsync). A crash boundary is therefore the last
//     flushed frame; recovery truncates to the last frame that verifies.
//   * Reader validates magic, version, frame length bounds, CRC32, event
//     decoding, and strict sequence continuity; first failure is reported with
//     its byte offset. It never crashes on corrupt input (fuzz-tested).

#include "arbiter/event.hpp"
#include "arbiter/types.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace arbiter {

struct JournalHeader {
    u32 version = 1;
    u64 master_seed = 0;
    u64 config_hash = 0;
};

class JournalWriter {
public:
    static Result<JournalWriter> create(const std::string& path, u64 master_seed, u64 config_hash);
    ~JournalWriter();
    JournalWriter(JournalWriter&& o) noexcept;
    JournalWriter& operator=(JournalWriter&& o) noexcept;
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    Status append(const Event& e);
    Status flush();  // userspace buffer -> OS
    Status sync();   // flush + fsync
    void close() noexcept;

    u64 records() const noexcept { return records_; }
    u64 next_seq() const noexcept { return next_seq_; }

    // Default ctor = "invalid handle" state; needed because Result<T> must be
    // able to construct T for error returns.
    JournalWriter() = default;

private:
    std::FILE* f_ = nullptr;
    std::vector<char> buf_;
    u64 records_ = 0;
    u64 next_seq_ = 1;
    size_t unflushed_ = 0;
};

class JournalReader {
public:
    static Result<JournalReader> open(const std::string& path);
    // True iff another full frame is available (peeks at the next frame
    // length, so a clean EOF is distinguishable from a corrupt frame).
    bool has_next();
    Result<Event> next();
    bool eof() const noexcept { return eof_; }
    bool failed() const noexcept { return failed_; }
    Status error() const noexcept { return error_; }
    u64 offset() const noexcept { return pos_; }  // byte offset after last good frame
    u64 records_read() const noexcept { return records_; }
    const JournalHeader& header() const noexcept { return hdr_; }

    // Default ctor = "invalid handle" state (see JournalWriter).
    JournalReader() = default;

private:
    std::FILE* f_ = nullptr;
    JournalHeader hdr_{};
    u64 pos_ = 0;       // byte offset after last good frame
    u64 records_ = 0;
    u64 last_seq_ = 0;
    bool eof_ = false;
    bool failed_ = false;
    bool peeked_ = false;
    u32 peeked_len_ = 0;
    Status error_{};
    std::vector<char> buf_;
};

struct JournalVerifyReport {
    u64 records = 0;
    bool clean = true;
    Status first_problem{};   // Errc::Ok if clean
    u64 problem_offset = 0;
    u64 last_seq = 0;
};

JournalVerifyReport journal_verify(const std::string& path);

// Truncate the file to the last verifiable frame boundary; returns records kept.
// A clean journal is returned unchanged (records == full count).
Result<u64> journal_recover(const std::string& path);

}  // namespace arbiter