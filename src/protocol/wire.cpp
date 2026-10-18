#include "arbiter/wire.hpp"
#include "arbiter/crc32.hpp"
#include "arbiter/errors.hpp"

#include <cstring>
#include <vector>

namespace arbiter {

std::string wire_encode(u8 channel, u64 md_seq, u16 flags, const Event& ev) {
    ByteWriter payload;
    encode_event(payload, ev);
    const std::string& pl = payload.data();
    ByteWriter h;
    h.put_u32(kFrameMagic);
    h.put_u16(kFrameVersion);
    h.put_u16(flags);
    h.put_u64(md_seq);
    h.put_u8(channel);
    h.put_u8(0);
    h.put_u8(0);
    h.put_u8(0);
    h.put_u32(static_cast<u32>(pl.size()));
    h.put_u32(crc32(pl.data(), pl.size()));
    std::string out = h.data();
    out += pl;
    return out;
}

std::string wire_encode_snapshot(u8 channel, u64 md_seq, const void* payload, size_t n) {
    ByteWriter h;
    h.put_u32(kFrameMagic);
    h.put_u16(kFrameVersion);
    h.put_u16(kFlagSnapshot);
    h.put_u64(md_seq);
    h.put_u8(channel);
    h.put_u8(0);
    h.put_u8(0);
    h.put_u8(0);
    h.put_u32(static_cast<u32>(n));
    h.put_u32(crc32(payload, n));
    std::string out = h.data();
    out.append(static_cast<const char*>(payload), n);
    return out;
}

std::string wire_encode_exec(u8 channel, u64 seq, const ExecReport& r) {
    ByteWriter p;
    p.put_u8(r.kind);
    p.put_u64(r.reason);
    p.put_u64(r.taker_id);
    p.put_u64(r.maker_id);
    p.put_u32(r.taker_acct);
    p.put_u32(r.maker_acct);
    p.put_i64(r.price);
    p.put_u64(r.qty);
    p.put_u8(static_cast<u8>(r.taker_side));
    p.put_u8(static_cast<u8>(r.maker_side));
    const std::string& pl = p.data();
    ByteWriter h;
    h.put_u32(kFrameMagic);
    h.put_u16(kFrameVersion);
    h.put_u16(kFlagExecReport);
    h.put_u64(seq);
    h.put_u8(channel);
    h.put_u8(0);
    h.put_u8(0);
    h.put_u8(0);
    h.put_u32(static_cast<u32>(pl.size()));
    h.put_u32(crc32(pl.data(), pl.size()));
    std::string out = h.data();
    out += pl;
    return out;
}

Result<DecodedFrame> wire_decode(const u8* data, size_t len) {
    if (len < kFrameHeaderSize) return ErrT<DecodedFrame>(Errc::ProtocolInvalid, len);
    ByteReader r(data, len);
    DecodedFrame f;
    const u32 magic = r.get_u32();
    if (magic != kFrameMagic) return ErrT<DecodedFrame>(Errc::ProtocolInvalid, 0);
    const u16 version = r.get_u16();
    if (version != kFrameVersion) return ErrT<DecodedFrame>(Errc::ProtocolInvalid, 4);
    f.flags = r.get_u16();
    if (f.flags & ~static_cast<u16>(kFlagRetransmit | kFlagSnapshot | kFlagExecReport))
        return ErrT<DecodedFrame>(Errc::ProtocolInvalid, 6);
    f.md_seq = r.get_u64();
    f.channel = r.get_u8();
    u8 reserved[3];
    r.get_bytes(reserved, 3);
    const u32 payload_len = r.get_u32();
    const u32 crc = r.get_u32();
    if (!r.ok()) return ErrT<DecodedFrame>(Errc::ProtocolInvalid, r.pos());
    if (payload_len != len - kFrameHeaderSize) return ErrT<DecodedFrame>(Errc::ProtocolInvalid, 24);
    if (crc32(data + kFrameHeaderSize, payload_len) != crc)
        return ErrT<DecodedFrame>(Errc::ChecksumMismatch, kFrameHeaderSize);

    if (f.flags & kFlagSnapshot) {
        f.snapshot_payload.assign(data + kFrameHeaderSize, data + kFrameHeaderSize + payload_len);
        return OkT(std::move(f));
    }
    if (f.flags & kFlagExecReport) {
        constexpr size_t kExecReportSize = 51;  // 1+8+8+8+4+4+8+8+1+1
        if (payload_len != kExecReportSize)
            return ErrT<DecodedFrame>(Errc::ProtocolInvalid, 24);
        ByteReader p(data + kFrameHeaderSize, payload_len);
        ExecReport x;
        x.kind = p.get_u8();
        x.reason = p.get_u64();
        x.taker_id = p.get_u64();
        x.maker_id = p.get_u64();
        x.taker_acct = p.get_u32();
        x.maker_acct = p.get_u32();
        x.price = p.get_i64();
        x.qty = p.get_u64();
        const u8 ts = p.get_u8(), ms = p.get_u8();
        if (!p.ok() || p.remaining() != 0 || ts > 1 || ms > 1 || x.kind > 3)
            return ErrT<DecodedFrame>(Errc::ProtocolInvalid, 24);
        x.taker_side = static_cast<Side>(ts);
        x.maker_side = static_cast<Side>(ms);
        f.exec = x;
        return OkT(std::move(f));
    }
    if (payload_len != kEncodedEventSize) return ErrT<DecodedFrame>(Errc::ProtocolInvalid, 24);
    auto ev = decode_event(data + kFrameHeaderSize, payload_len);
    if (!ev) return ErrT<DecodedFrame>(ev.status.code, ev.status.context);
    f.ev = ev.value;
    return OkT(std::move(f));
}

}  // namespace arbiter