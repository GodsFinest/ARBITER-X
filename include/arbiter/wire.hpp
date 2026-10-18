#pragma once
// Wire protocol: fixed-layout binary frames with CRC integrity, sequence
// numbers, and channel ids. The payload of an event frame is the canonical
// encoded Event (58 bytes), the SAME representation the journal uses, so
// what crosses the network is byte-identical to what is journaled.
//
// Frame layout (v1, 28-byte header):
//   magic u32 'ARXF' | version u16 | flags u16 | md_seq u64 | channel u8
//   reserved u8[3] | payload_len u32 | crc32(payload) u32 | payload
//
// flags: bit0 RETRANSMIT (gap recovery re-send), bit1 SNAPSHOT (payload is a
// state snapshot chunk, not an Event), bit2 EXEC_REPORT (payload is a
// fixed 51-byte execution report; see ExecReport below).
//
// Live-path conventions (docs/EXECUTION_MODEL.md):
//   * order intents (strategy -> exchange, channel 2) ride as Event payloads:
//       Send   -> BookAdd convention: a=id, b=expiry, c=qty, p=price,
//                 account/side/otype/tif as-is
//       Cancel -> BookCancel convention: a=target order id, account=owner
//     The gateway reconstructs the order from the DECODED event only.
//   * execution reports (exchange -> strategy, channel 3) ride as
//     kFlagExecReport frames (they carry maker account and both sides, which
//     the 58-byte Event does not).
//
// decode never throws and never reads out of bounds; every field is
// validated. Fuzzed in tests/test_world.cpp and via `arbiter fuzz protocol`.

#include "arbiter/codec.hpp"
#include "arbiter/event.hpp"
#include "arbiter/types.hpp"

#include <vector>

namespace arbiter {

inline constexpr u32 kFrameMagic = 0x46585241u;  // 'ARXF' little-endian
inline constexpr u16 kFrameVersion = 1;
inline constexpr size_t kFrameHeaderSize = 28;

enum FrameFlags : u16 {
    kFlagNone = 0,
    kFlagRetransmit = 1,
    kFlagSnapshot = 2,
    kFlagExecReport = 4,
};

std::string wire_encode(u8 channel, u64 md_seq, u16 flags, const Event& ev);
// Snapshot frames carry arbitrary validated payloads (e.g. encoded ladders).
std::string wire_encode_snapshot(u8 channel, u64 md_seq, const void* payload, size_t n);

// Execution report carried between exchange and strategy on the exec channel.
// kind: 0 = fill, 1 = risk reject (reason set), 2 = IOC remainder canceled,
// 3 = cancel confirmed.
struct ExecReport {
    u8 kind = 0;
    u64 reason = 0;        // RiskReason when kind == 1
    u64 taker_id = 0, maker_id = 0;
    u32 taker_acct = 0, maker_acct = 0;
    i64 price = 0;
    u64 qty = 0;
    Side taker_side = Side::Buy, maker_side = Side::Sell;
};

std::string wire_encode_exec(u8 channel, u64 seq, const ExecReport& r);

struct DecodedFrame {
    u16 flags = 0;
    u8 channel = 0;
    u64 md_seq = 0;
    Event ev{};  // meaningful when !(flags & (kFlagSnapshot | kFlagExecReport))
    std::vector<u8> snapshot_payload;  // meaningful when (flags & kFlagSnapshot)
    ExecReport exec{};                 // meaningful when (flags & kFlagExecReport)
};

Result<DecodedFrame> wire_decode(const u8* data, size_t len);

}  // namespace arbiter