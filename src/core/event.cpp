#include "arbiter/event.hpp"
#include "arbiter/hash.hpp"

namespace arbiter {

const char* to_cstr(EvType t) noexcept {
    switch (t) {
        case EvType::BookAdd: return "BookAdd";
        case EvType::BookCancel: return "BookCancel";
        case EvType::BookReplace: return "BookReplace";
        case EvType::TradePrint: return "TradePrint";
        case EvType::MdNew: return "MdNew";
        case EvType::MdCancel: return "MdCancel";
        case EvType::MdTrade: return "MdTrade";
        case EvType::TimerFire: return "TimerFire";
        case EvType::RiskDecision: return "RiskDecision";
        case EvType::Fault: return "Fault";
        case EvType::Checkpoint: return "Checkpoint";
        case EvType::Session: return "Session";
        case EvType::Halt: return "Halt";
    }
    return "?";
}

void encode_event(ByteWriter& w, const Event& e) {
    w.put_u64(e.seq);
    w.put_i64(e.t);
    w.put_u16(static_cast<u16>(e.type));
    w.put_u32(e.account);
    w.put_u8(static_cast<u8>(e.side));
    w.put_u8(static_cast<u8>(e.otype));
    w.put_u8(static_cast<u8>(e.tif));
    w.put_u8(0);  // padding reserved
    w.put_u64(e.a);
    w.put_u64(e.b);
    w.put_u64(e.c);
    w.put_i64(e.p);
}

Result<Event> decode_event(const u8* data, size_t len) {
    if (len != kEncodedEventSize) return ErrT<Event>(Errc::ProtocolInvalid, len);
    ByteReader r(data, len);
    Event e;
    e.seq = r.get_u64();
    e.t = r.get_i64();
    const u16 ty = r.get_u16();
    if (ty < static_cast<u16>(EvType::BookAdd) || ty > static_cast<u16>(EvType::Halt))
        return ErrT<Event>(Errc::ProtocolInvalid, ty);
    e.type = static_cast<EvType>(ty);
    e.account = r.get_u32();
    const u8 side = r.get_u8();
    const u8 otype = r.get_u8();
    const u8 tif = r.get_u8();
    r.get_u8();  // padding
    if (side > 1 || otype > 1 || tif > 3) return ErrT<Event>(Errc::ProtocolInvalid, side);
    e.side = static_cast<Side>(side);
    e.otype = static_cast<OrderType>(otype);
    e.tif = static_cast<Tif>(tif);
    e.a = r.get_u64();
    e.b = r.get_u64();
    e.c = r.get_u64();
    e.p = r.get_i64();
    if (!r.ok() || r.remaining() != 0) return ErrT<Event>(Errc::ProtocolInvalid, r.pos());
    return OkT(e);
}

u64 event_hash(const Event& e) {
    Hasher h;
    h.tag("ARBITERX/EVENT/1");
    h.u64v(e.seq);
    h.i64v(e.t);
    h.u16v(static_cast<u16>(e.type));
    h.u32v(e.account);
    h.u8v(static_cast<u8>(e.side));
    h.u8v(static_cast<u8>(e.otype));
    h.u8v(static_cast<u8>(e.tif));
    h.u64v(e.a);
    h.u64v(e.b);
    h.u64v(e.c);
    h.i64v(e.p);
    return h.value();
}

Event ev_book_add(u64 seq, LogicalNs t, const Order& o) {
    Event e;
    e.seq = seq;
    e.t = t;
    e.type = EvType::BookAdd;
    e.account = o.account;
    e.side = o.side;
    e.otype = o.type;
    e.tif = o.tif;
    e.a = o.id;
    e.b = static_cast<u64>(o.expiry);  // absolute logical expiry (GTD; 0 = none)
    e.c = o.qty;
    e.p = o.price;
    return e;
}

Event ev_book_cancel(u64 seq, LogicalNs t, u64 order_id) {
    Event e;
    e.seq = seq;
    e.t = t;
    e.type = EvType::BookCancel;
    e.a = order_id;
    return e;
}

Event ev_book_replace(u64 seq, LogicalNs t, u64 orig_id, const Order& r) {
    Event e;
    e.seq = seq;
    e.t = t;
    e.type = EvType::BookReplace;
    e.account = r.account;
    e.side = r.side;
    e.otype = r.type;
    e.tif = r.tif;
    e.a = orig_id;
    e.b = r.id;
    e.c = r.qty;
    e.p = r.price;
    return e;
}

Event ev_trade_print(u64 seq, LogicalNs t, const Fill& f) {
    Event e;
    e.seq = seq;
    e.t = t;
    e.type = EvType::TradePrint;
    e.account = f.taker_account;
    e.a = f.taker_order_id;
    e.b = f.maker_order_id;
    e.c = f.qty;
    e.p = f.price;
    return e;
}

Event ev_md_new(u64 seq, LogicalNs t, u64 md_id, Side s, Price p, Qty q) {
    Event e;
    e.seq = seq;
    e.t = t;
    e.type = EvType::MdNew;
    e.side = s;
    e.a = md_id;
    e.c = q;
    e.p = p;
    return e;
}

Event ev_md_cancel(u64 seq, LogicalNs t, u64 md_id) {
    Event e;
    e.seq = seq;
    e.t = t;
    e.type = EvType::MdCancel;
    e.a = md_id;
    return e;
}

Event ev_md_trade(u64 seq, LogicalNs t, Price p, Qty q) {
    Event e;
    e.seq = seq;
    e.t = t;
    e.type = EvType::MdTrade;
    e.a = q;
    e.p = p;
    return e;
}

}  // namespace arbiter