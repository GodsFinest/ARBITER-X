#include "arbiter/journal.hpp"
#include "arbiter/codec.hpp"
#include "arbiter/crc32.hpp"
#include "arbiter/errors.hpp"

#include <unistd.h>

namespace arbiter {

namespace {
constexpr char kMagic[4] = {'A', 'R', 'X', 'J'};
constexpr size_t kFileHeaderSize = 24;  // magic 4 + version 4 + seed 8 + cfg 8
}  // namespace

// ---------------- Writer ----------------

Result<JournalWriter> JournalWriter::create(const std::string& path, u64 master_seed, u64 config_hash) {
    JournalWriter w;
    w.f_ = std::fopen(path.c_str(), "wb");
    if (!w.f_) return ErrT<JournalWriter>(Errc::IoError, static_cast<u64>(errno));
    ByteWriter hw;
    hw.put_bytes(kMagic, 4);
    hw.put_u32(1);
    hw.put_u64(master_seed);
    hw.put_u64(config_hash);
    if (std::fwrite(hw.data().data(), 1, hw.size(), w.f_) != hw.size()) {
        std::fclose(w.f_);
        w.f_ = nullptr;
        return ErrT<JournalWriter>(Errc::IoError, 1);
    }
    w.buf_.reserve(1u << 16);
    return OkT(std::move(w));
}

JournalWriter::~JournalWriter() { close(); }

JournalWriter::JournalWriter(JournalWriter&& o) noexcept { *this = std::move(o); }

JournalWriter& JournalWriter::operator=(JournalWriter&& o) noexcept {
    if (this != &o) {
        close();
        f_ = o.f_;
        o.f_ = nullptr;
        buf_ = std::move(o.buf_);
        records_ = o.records_;
        next_seq_ = o.next_seq_;
        unflushed_ = o.unflushed_;
    }
    return *this;
}

Status JournalWriter::append(const Event& e) {
    if (!f_) return fail(Errc::IoError, 0);
    Event with_seq = e;
    with_seq.seq = next_seq_++;  // journal owns sequence numbering
    ByteWriter payload;
    encode_event(payload, with_seq);
    const std::string& pl = payload.data();
    ByteWriter frame;
    frame.put_u32(static_cast<u32>(pl.size()));
    frame.put_u32(crc32(pl.data(), pl.size()));
    frame.put_bytes(pl.data(), pl.size());
    const std::string& fr = frame.data();
    buf_.insert(buf_.end(), fr.begin(), fr.end());
    unflushed_ += fr.size();
    ++records_;
    if (unflushed_ >= (1u << 16)) return flush();
    return good();
}

Status JournalWriter::flush() {
    if (!f_) return fail(Errc::IoError, 0);
    if (!buf_.empty()) {
        if (std::fwrite(buf_.data(), 1, buf_.size(), f_) != buf_.size()) return fail(Errc::IoError, 2);
        buf_.clear();
        unflushed_ = 0;
    }
    // Durability contract: flush() means the bytes have reached the OS, so a
    // concurrent reader (or post-crash reader) sees every flushed frame.
    if (std::fflush(f_) != 0) return fail(Errc::IoError, 5);
    return good();
}

Status JournalWriter::sync() {
    if (Status s = flush(); !s) return s;
    if (std::fflush(f_) != 0) return fail(Errc::IoError, 3);
    if (::fsync(::fileno(f_)) != 0) return fail(Errc::IoError, 4);
    return good();
}

void JournalWriter::close() noexcept {
    if (f_) {
        std::fflush(f_);
        std::fclose(f_);
        f_ = nullptr;
    }
}

// ---------------- Reader ----------------

Result<JournalReader> JournalReader::open(const std::string& path) {
    JournalReader r;
    r.f_ = std::fopen(path.c_str(), "rb");
    if (!r.f_) return ErrT<JournalReader>(Errc::NotFound, static_cast<u64>(errno));
    char magic[4];
    if (std::fread(magic, 1, 4, r.f_) != 4 || std::memcmp(magic, kMagic, 4) != 0) {
        std::fclose(r.f_);
        r.f_ = nullptr;
        return ErrT<JournalReader>(Errc::JournalCorrupt, 0);
    }
    u32 version = 0;
    if (std::fread(&version, 1, 4, r.f_) != 4) {
        std::fclose(r.f_);
        r.f_ = nullptr;
        return ErrT<JournalReader>(Errc::JournalCorrupt, 4);
    }
    if (version != 1) {
        std::fclose(r.f_);
        r.f_ = nullptr;
        return ErrT<JournalReader>(Errc::JournalCorrupt, 4);
    }
    if (std::fread(&r.hdr_.master_seed, 1, 8, r.f_) != 8 ||
        std::fread(&r.hdr_.config_hash, 1, 8, r.f_) != 8) {
        std::fclose(r.f_);
        r.f_ = nullptr;
        return ErrT<JournalReader>(Errc::JournalCorrupt, 12);
    }
    r.hdr_.version = version;
    r.pos_ = kFileHeaderSize;
    r.buf_.resize(1u << 16);
    return OkT(std::move(r));
}

bool JournalReader::has_next() {
    if (eof_ || failed_) return false;
    if (peeked_) return true;
    u32 len = 0;
    const size_t got = std::fread(&len, 1, 4, f_);
    if (got == 0) {
        eof_ = true;  // clean EOF: the boundary is a valid stopping point
        return false;
    }
    if (got != 4) {
        failed_ = true;
        error_ = fail(Errc::JournalCorrupt, pos_);
        return false;
    }
    peeked_len_ = len;
    peeked_ = true;
    return true;
}

Result<Event> JournalReader::next() {
    if (eof_ || failed_) return ErrT<Event>(error_.code == Errc::Ok ? Errc::Internal : error_.code, error_.context);
    u32 len = 0, crc = 0;
    if (peeked_) {
        len = peeked_len_;
        peeked_ = false;
    } else {
        const size_t got = std::fread(&len, 1, 4, f_);
        if (got == 0) {
            eof_ = true;  // clean EOF: the boundary is a valid stopping point
            return ErrT<Event>(Errc::NotFound, pos_);
        }
        if (got != 4) {
            failed_ = true;
            error_ = fail(Errc::JournalCorrupt, pos_);
            return ErrT<Event>(Errc::JournalCorrupt, pos_);
        }
    }
    pos_ += 4;
    if (std::fread(&crc, 1, 4, f_) != 4) {
        failed_ = true;
        error_ = fail(Errc::JournalCorrupt, pos_);
        return ErrT<Event>(Errc::JournalCorrupt, pos_);
    }
    pos_ += 4;
    if (len == 0 || len > (1u << 24)) {  // sanity bound: 16 MiB per event max
        failed_ = true;
        error_ = fail(Errc::JournalCorrupt, pos_);
        return ErrT<Event>(Errc::JournalCorrupt, pos_);
    }
    if (buf_.size() < len) buf_.resize(len);
    if (std::fread(buf_.data(), 1, len, f_) != len) {
        failed_ = true;
        error_ = fail(Errc::JournalCorrupt, pos_);  // truncated frame
        return ErrT<Event>(Errc::JournalCorrupt, pos_);
    }
    const u32 actual = crc32(buf_.data(), len);
    if (actual != crc) {
        failed_ = true;
        error_ = fail(Errc::ChecksumMismatch, pos_);
        return ErrT<Event>(Errc::ChecksumMismatch, pos_);
    }
    pos_ += len;
    auto ev = decode_event(reinterpret_cast<const u8*>(buf_.data()), len);
    if (!ev) {
        failed_ = true;
        error_ = fail(Errc::ProtocolInvalid, pos_);
        return ErrT<Event>(Errc::ProtocolInvalid, pos_);
    }
    // strict sequence continuity
    if (ev.value.seq != last_seq_ + 1) {
        failed_ = true;
        error_ = fail(Errc::SequenceGap, ev.value.seq);
        return ErrT<Event>(Errc::SequenceGap, pos_);
    }
    last_seq_ = ev.value.seq;
    ++records_;
    return OkT(ev.value);
}

// ---------------- verify / recover ----------------

namespace {

// Scan without building the full state; shared by verify and recover.
JournalVerifyReport scan_journal(const std::string& path) {
    JournalVerifyReport rep;
    auto rd = JournalReader::open(path);
    if (!rd) {
        rep.clean = false;
        rep.first_problem = Status{Errc::JournalCorrupt, 0};
        return rep;
    }
    JournalReader& r = rd.value;
    while (r.has_next()) {
        auto ev = r.next();
        if (!ev) {
            rep.clean = false;
            rep.first_problem = ev.status;
            rep.problem_offset = ev.status.context;
            return rep;
        }
        rep.records = r.records_read();
        rep.last_seq = ev.value.seq;
    }
    if (r.failed()) {
        rep.clean = false;
        rep.first_problem = r.error();
        rep.problem_offset = r.error().context;
    }
    return rep;
}

}  // namespace

JournalVerifyReport journal_verify(const std::string& path) {
    return scan_journal(path);
}

Result<u64> journal_recover(const std::string& path) {
    JournalVerifyReport rep = scan_journal(path);
    if (rep.clean) return OkT(rep.records);
    // Replay to find the byte offset after the last good frame. The reader's
    // offset() is exactly that after each successful next().
    auto rd = JournalReader::open(path);
    if (!rd) return ErrT<u64>(Errc::JournalCorrupt, 0);
    JournalReader& r = rd.value;
    u64 good_end = kFileHeaderSize;
    u64 kept = 0;
    while (r.has_next()) {
        auto ev = r.next();
        if (!ev) break;
        good_end = r.offset();
        kept = r.records_read();
    }
    if (::truncate(path.c_str(), static_cast<off_t>(good_end)) != 0)
        return ErrT<u64>(Errc::IoError, static_cast<u64>(errno));
    return OkT(kept);
}

}  // namespace arbiter