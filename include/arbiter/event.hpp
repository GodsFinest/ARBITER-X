#pragma once
// Canonical event model. Every meaningful occurrence in ARBITER-X is an Event
// with a global sequence number and logical time; journals, the wire protocol,
// and state hashing all share this representation.
//
// Payload convention: instead of a tagged union, each event type documents
// which generic slots it uses (a/b/c for ids/quantities, p for price, plus the
// order fields side/otype/tif for order-bearing events). Fixed-size events
// keep journaling and replay simple and make corruption detection reliable.

#include "arbiter/codec.hpp"
#include "arbiter/order.hpp"
#include "arbiter/types.hpp"

namespace arbiter {

enum class EvType : u16 {
    // ---- order flow into the book (inputs) ----
    BookAdd = 1,
    BookCancel = 2,
    BookReplace = 3,
    // ---- matching outputs ----
    TradePrint = 4,     // a fill; a= taker id, b= maker id, c= maker account, p= price, qty slot below
    // ---- market data feed (what strategies observe) ----
    MdNew = 5,          // a= md_id, side/p/qty
    MdCancel = 6,       // a= md_id
    MdTrade = 7,        // p= price, a= qty
    // ---- infrastructure ----
    TimerFire = 8,      // a= timer_id, b= kind (0=gtd expiry)
    RiskDecision = 9,   // a= order_id, b= decision (1 accept, 2 reject), c= reason code
    Fault = 10,         // a= fault kind, b= param
    Checkpoint = 11,    // a= event seq, b= state hash
    Session = 12,       // a= subtype (1 connect, 2 disconnect, 3 recovery start, 4 recovered)
    Halt = 13,
};

const char* to_cstr(EvType t) noexcept;

// Encoded event size: seq 8 + t 8 + type 2 + account 4 + side/otype/tif/pad 4
// + a 8 + b 8 + c 8 + p 8.
inline constexpr size_t kEncodedEventSize = 58;

// Slot for quantities in TradePrint/MdNew/MdTrade events (b is taken by maker
// id for TradePrint, so qty uses c for prints; documented per type):
//   TradePrint: a=taker_id, b=maker_id, c=qty, p=price, account=taker account
//   MdNew:      a=md_id, c=qty, p=price, side
//   MdCancel:   a=md_id
//   MdTrade:    a=qty, p=price

struct Event {
    u64 seq = 0;
    LogicalNs t = 0;
    EvType type = EvType::Halt;
    u32 account = 0;
    Side side = Side::Buy;
    OrderType otype = OrderType::Limit;
    Tif tif = Tif::GTC;
    u64 a = 0, b = 0, c = 0;
    i64 p = 0;
};

void encode_event(ByteWriter& w, const Event& e);
Result<Event> decode_event(const u8* data, size_t len);
u64 event_hash(const Event& e);  // canonical per-event hash (for trace comparison)

// Factory helpers (avoid field-slot mistakes at call sites).
Event ev_book_add(u64 seq, LogicalNs t, const Order& o);
Event ev_book_cancel(u64 seq, LogicalNs t, u64 order_id);
Event ev_book_replace(u64 seq, LogicalNs t, u64 orig_id, const Order& replacement);
Event ev_trade_print(u64 seq, LogicalNs t, const Fill& f);
Event ev_md_new(u64 seq, LogicalNs t, u64 md_id, Side s, Price p, Qty q);
Event ev_md_cancel(u64 seq, LogicalNs t, u64 md_id);
Event ev_md_trade(u64 seq, LogicalNs t, Price p, Qty q);

}  // namespace arbiter