#include "arbiter/world.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/rng.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace arbiter {

u64 WorldConfig::hash() const {
    Hasher h;
    h.tag("ARBITERX/WORLDCFG/3");  // /3: now covers risk.enabled, mm.enabled, momentum.enabled
    h.i64v(algo_latency_ns);
    h.i64v(cancel_latency_ns);
    h.i64v(exchange_processing_ns);
    h.i64v(publish_latency_ns);
    auto net = [&](const NetProfile& np) {
        h.i64v(np.base_delay_ns);
        h.i64v(np.jitter_ns);
        h.u64v(static_cast<u64>(np.loss_rate * 1e9));  // quantized draw, deterministic
        h.u64v(static_cast<u64>(np.dup_rate * 1e9));
        h.u64v(np.reorder_window);
        h.i64v(np.reorder_slot_ns);
        h.u64v(np.burst_loss_len);
        h.u64v(static_cast<u64>(np.burst_loss_rate * 1e9));
        h.u64v(static_cast<u64>(np.corrupt_rate * 1e9));
    };
    net(md_channel);
    net(order_channel);
    net(exec_channel);
    h.i64v(sim.start_mid);
    h.u64v(sim.provider_qty);
    h.u64v(sim.taker_qty);
    h.u64v(static_cast<u64>(sim.provider_arrival_per_ms * 1e9));
    h.u64v(static_cast<u64>(sim.taker_arrival_per_ms * 1e9));
    h.u64v(static_cast<u64>(sim.cancel_fraction * 1e9));
    h.u64v(static_cast<u64>(sim.taker_self_excitation * 1e9));  // clustering knob (quantized)
    h.i64v(sim.touch_offset_ticks);
    h.i64v(sim.deep_offset_ticks);
    h.i64v(sim.regime_dwell_ns);
    h.u64v(static_cast<u64>(sim.volatile_rate_multiplier * 1e9));
    h.u64v(sim.volatile_qty_multiplier);
    h.u64v(sim.provider_accounts);
    h.i64v(sim.min_price);
    h.i64v(sim.max_price);
    h.i64v(horizon_ns);
    h.u64v(max_events);
    h.i64v(fee_per_lot);
    h.boolean(bots_observe_shadow);  // interference mode is behavior-relevant
    h.boolean(risk.enabled);
    h.i64v(risk.min_price);
    h.i64v(risk.max_price);
    h.u64v(risk.max_order_qty);
    h.i64v(risk.max_position);
    h.i64v(risk.max_notional_per_order);
    h.u64v(risk.max_msgs_per_window);
    h.i64v(risk.rate_window_ns);
    h.boolean(risk.kill_switch);
    h.i64v(book_min_price);
    h.i64v(book_max_price);
    h.u32v(mm.account);
    h.u64v(mm.quote_size);
    h.i64v(mm.half_spread_ticks);
    h.i64v(mm.skew_denominator);
    h.i64v(mm.skew_ticks);
    h.i64v(mm.max_inventory);
    h.i64v(mm.requote_interval_ns);
    h.boolean(mm.enabled);
    h.u32v(momentum.account);
    h.u64v(momentum.size);
    h.i64v(momentum.min_signal);
    h.u64v(momentum.window);
    h.i64v(momentum.cooldown_ns);
    h.i64v(momentum.max_inventory);
    h.u64v(momentum.id_base);  // id-space layout: behavior-relevant (gateway dedup)
    h.boolean(momentum.enabled);
    h.i64v(warmup_ns);
    return h.value();
}

u64 World::world_id() const noexcept {
    return Rng::mix(cfg_hash_ ^ Rng::mix(master_seed_));
}

World::World(const WorldConfig& cfg, u64 master_seed)
    : cfg_(cfg),
      cfg_hash_(cfg.hash()),
      master_seed_(master_seed),
      book_(OptBook::Options{cfg.book_min_price, cfg.book_max_price, 1u << 14}),
      sim_(cfg.sim, master_seed),
      mm_(cfg.mm),
      mom_(cfg.momentum),
      risk_(cfg.risk),
      md_ch_(1, cfg.md_channel, master_seed),
      order_ch_(2, cfg.order_channel, master_seed),
      exec_ch_(3, cfg.exec_channel, master_seed),
      corrupt_rng_(Rng::stream(master_seed, 0x4300'5250'4E47ull)) {
    // Fail LOUDLY on a degenerate book span instead of attempting a huge
    // ladder allocation in OptBook (2 x span x 32B): the ladder needs
    // book_min_price < book_max_price and a representable span.
    if (cfg_.book_min_price < 1 || cfg_.book_max_price <= cfg_.book_min_price ||
        static_cast<u64>(cfg_.book_max_price) - static_cast<u64>(cfg_.book_min_price) >
            (1ull << 32)) {
        std::fprintf(stderr, "arbiter: invalid book price span [%lld, %lld]\n",
                     (long long)cfg_.book_min_price, (long long)cfg_.book_max_price);
        std::abort();
    }
    if (cfg_.journal_events) {
        std::string path = cfg_.journal_path.empty()
                               ? "artifacts/world_" + std::to_string(master_seed_) + ".journal"
                               : cfg_.journal_path;
        auto w = JournalWriter::create(path, master_seed_, 0);
        if (w) {
            journal_ = std::make_unique<JournalWriter>(std::move(w.value));
            journal_path_ = path;
        }
    }
}

World::~World() = default;

void World::schedule_seed_events() {
    schedule(sim_.next_action_time(), RankSim, EvSimAction);
    schedule(cfg_.warmup_ns + cfg_.mm.requote_interval_ns, RankIntent, EvIntentDue, 0, 0, 0);
    schedule(cfg_.warmup_ns + cfg_.momentum.cooldown_ns, RankIntent, EvIntentDue, 1, 0, 0);
}

void World::jappend(const Event& e) {
    // journal_records counts only events actually written to a journal,
    // a journal-off run must report 0, not "events that would have been
    // journalled"
    if (journal_) {
        journal_->append(e);
        ++stats_.journal_records;
    }
}

void World::schedule(LogicalNs t, u8 rank, EvType type, u64 a, u64 b, i64 p) {
    queue_.push(WorldEvent{t, rank, tiebreak_++, type, a, b, 0, p});
}

// ---------------- main loop ----------------

RunStats World::run(u64 max_additional_events) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!started_) {
        schedule_seed_events();
        started_ = true;
    }
    u64 processed_here = 0;

    while (!queue_.empty() && !halted_) {
        if (events_ >= cfg_.max_events) break;
        if (processed_here >= max_additional_events) break;
        ++processed_here;
        WorldEvent ev = queue_.top();
        queue_.pop();
        if (ev.t > cfg_.horizon_ns) break;
        now_ = ev.t;
        process(ev);
        ++events_;
        if (cfg_.checkpoint_every_events && events_ % cfg_.checkpoint_every_events == 0) {
            Event cp;
            cp.t = now_;
            cp.type = EvType::Checkpoint;
            cp.b = state_hash();
            jappend(cp);
        }
        if (cfg_.hash_every_events && events_ % cfg_.hash_every_events == 0)
            stats_.hash_timeline.emplace_back(events_, state_hash());
    }

    stats_.events_processed = events_;
    stats_.logical_end = now_;
    stats_.wall_ns =
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count());
    const Price bb = book_.best_bid(), ba = book_.best_ask();
    stats_.final_mid = (bb && ba) ? (bb + ba) / 2 : (bb ? bb : ba);
    stats_.mm_inventory = mm_.inventory();
    stats_.mm_cash = mm_.cash();
    stats_.mm_quote_updates = mm_.quote_updates();
    stats_.mm_inference_fill_qty = mm_.inference_fill_qty();
    stats_.mm_real_filled_qty = mm_.real_filled_qty();
    stats_.mm_ambiguous_fill_qty = mm_.ambiguous_fill_qty();
    stats_.mom_inventory = mom_.inventory();
    stats_.mom_cash = mom_.cash();
    stats_.mom_filled_qty = mom_.filled_qty();
    stats_.risk_accepted = risk_.state().accepted;
    stats_.risk_rejected = risk_.state().rejected;
    if (journal_) journal_->sync();
    return stats_;
}

// ---------------- event processing ----------------

void World::process(const WorldEvent& ev) {
    switch (static_cast<u16>(ev.type)) {
        case static_cast<u16>(EvSimAction): {
            sim_.advance(now_);
            if (cfg_.bots_observe_shadow) {
                // Interference control: bots see the touch EXCLUDING strategy
                // orders, their quoting input is then exogenous to the
                // strategies, so strategy-side counterfactuals cannot feed
                // back into bot behavior (docs/COUNTERFACTUALS.md).
                sim_.observe_shadow(shadow_best_bid(), shadow_best_ask());
            } else {
                sim_.observe(book_.best_bid(), book_.best_ask());
            }
            BotAction a = sim_.next_action();
            switch (a.kind) {
                case BotAction::Kind::Add:
                case BotAction::Kind::Taker:
                    enter_book(a.ord, now_);
                    break;
                case BotAction::Kind::Cancel:
                    cancel_in_book(a.target, now_);
                    break;
                case BotAction::Kind::None:
                    break;
            }
            schedule(sim_.next_action_time(), RankSim, EvSimAction);
            break;
        }
        case static_cast<u16>(EvMdDeliver):
            feed_handler_md(ev.b);
            break;
        case static_cast<u16>(EvIntentDue): {
            std::vector<StrategyAction> acts = ev.a == 0 ? mm_.on_timer() : mom_.on_timer();
            for (const StrategyAction& act : acts) route_intent(act, now_);
            const LogicalNs period =
                ev.a == 0 ? cfg_.mm.requote_interval_ns : cfg_.momentum.cooldown_ns;
            schedule(now_ + period, RankIntent, EvIntentDue, ev.a, 0, 0);
            break;
        }
        case static_cast<u16>(EvGatewayArrival): {
            Msg& m = inflight_[ev.b];
            // GATEWAY DECODE: the order is reconstructed from the wire frame
            // only: CRC failure (in-transit corruption) rejects the frame,
            // journals a Fault, and the world continues (path
            // integrity; the message is gone, like a real gateway would see).
            bool is_cancel = false, ok = false;
            const Order decoded = decode_order_frame(m.frame, is_cancel, ok);
            if (!ok) {
                // CRC/structure failure: the frame is unintelligible, a real
                // gateway could not attribute it either.
                ++stats_.wire_frames_corrupted;
                Event flt;
                flt.t = now_;
                flt.type = EvType::Fault;
                flt.a = 2;  // fault kind: order frame rejected at gateway
                flt.b = m.frame.size();
                jappend(flt);
                drop_stage_trace(m.ord.id);
                break;
            }
            m.ord = decoded;  // gateway operates on the decoded order
            m.is_cancel = is_cancel;
            ++stats_.wire_frames_decoded;
            if (m.is_cancel) {
                // cancels bypass risk (documented); straight to the exchange
                cancel_in_book(m.ord.id, now_);
                break;
            }
            const bool dup = gateway_known_ids_.count(m.ord.id) != 0;
            const RiskVerdict v = risk_.check(m.ord, now_, dup);
            if (v.accept) {
                gateway_known_ids_.insert(m.ord.id);
                {
                    auto st = stages_.find(m.ord.id);
                    if (st != stages_.end()) st->second.gateway = now_;
                }
                schedule(now_ + cfg_.exchange_processing_ns, RankGateway, EvMatchDue, 0, ev.b, 0);
            } else {
                drop_stage_trace(m.ord.id);
                Event rd;
                rd.t = now_;
                rd.type = EvType::RiskDecision;
                rd.account = m.ord.account;
                rd.side = m.ord.side;
                rd.otype = m.ord.type;
                rd.tif = m.ord.tif;
                rd.a = m.ord.id;
                rd.b = static_cast<u64>(v.reason);
                rd.c = 2;  // decision = reject
                jappend(rd);
                Notify n;
                n.kind = 1;
                n.fill.taker_order_id = m.ord.id;
                n.fill.taker_account = m.ord.account;
                n.fill.maker_order_id = static_cast<u64>(v.reason);
                n.frame = encode_notify_frame(n);
                notifies_.push_back(std::move(n));
                const DeliveryPlan d = exec_ch_.next_delivery();
                if (!d.drop) {
                    std::string& fr = notifies_.back().frame;
                    if (d.corrupt) corrupt_frame(fr, corrupt_rng_);
                    ++stats_.wire_frames_encoded;
                    schedule(now_ + cfg_.publish_latency_ns + d.delay_ns + d.reorder_extra_ns,
                             RankExec, EvExecNotify, 1, notifies_.size() - 1, 0);
                }
            }
            break;
        }
        case static_cast<u16>(EvMatchDue):
            enter_book(inflight_[ev.b].ord, now_);
            break;
        case static_cast<u16>(EvExecNotify): {
            Notify& n = notifies_[ev.b];
            // STRATEGY FEED DECODE: the notification is reconstructed from
            // its wire frame; a corrupted frame is dropped (counted), the
            // strategy never sees it, identical to a real feed handler.
            DecodedFrame df;
            const bool decoded_ok = [&] {
                auto d = wire_decode(reinterpret_cast<const u8*>(n.frame.data()), n.frame.size());
                if (!d) return false;
                df = std::move(d.value);
                return true;
            }();
            if (!decoded_ok || (df.flags & kFlagExecReport) == 0) {
                ++stats_.wire_frames_corrupted;
                Event flt;
                flt.t = now_;
                flt.type = EvType::Fault;
                flt.a = 3;  // fault kind: exec frame rejected at strategy feed
                flt.b = n.frame.size();
                jappend(flt);
                break;
            }
            ++stats_.wire_frames_decoded;
            const ExecReport& x = df.exec;
            if (x.kind == 0) {
                Fill f;
                f.taker_order_id = x.taker_id;
                f.maker_order_id = x.maker_id;
                f.maker_account = x.maker_acct;
                f.taker_account = x.taker_acct;
                f.price = x.price;
                f.qty = x.qty;
                f.t = now_;
                f.taker_side = x.taker_side;
                f.maker_side = x.maker_side;
                deliver_fill(f, now_);
            } else if (x.kind == 1) {
                if (x.taker_acct == cfg_.mm.account) mm_.on_reject(x.taker_id);
                drop_stage_trace(x.taker_id);
            } else if (x.kind == 2) {
                if (x.taker_acct == cfg_.mm.account) mm_.on_reject(x.taker_id);
                drop_stage_trace(x.taker_id);
            } else if (x.kind == 3) {
                if (x.taker_acct == cfg_.mm.account) mm_.on_cancel_confirmed(x.taker_id);
                drop_stage_trace(x.taker_id);
            }
            break;
        }
        case static_cast<u16>(EvGtdExpiry):
            cancel_in_book(ev.a, now_);
            break;
        case static_cast<u16>(EvGapRequestDue):
            gap_request_pending_ = false;
            gap_recover();
            break;
        case static_cast<u16>(EvMarkout): {
            // Adverse-selection probe: mid has moved dt past a strategy fill;
            // record mid - fill price signed by the strategy's side (buy
            // fill: positive = price moved up in our favor after buying).
            // The probe's account routes the sample to the owning strategy's
            // histogram (MM vs momentum, never pooled).
            if (ev.b < markouts_.size()) {
                const MarkoutProbe& p = markouts_[ev.b];
                const Price bb = book_.best_bid(), ba = book_.best_ask();
                const Price mid = (bb && ba) ? (bb + ba) / 2 : (bb ? bb : ba);
                if (mid > 0) {
                    const double mark = static_cast<double>(mid - p.fill_px);
                    const bool is_mm = p.account == cfg_.mm.account;
                    if (ev.a == 0)
                        (is_mm ? stats_.adverse_100us : stats_.mom_adverse_100us)
                            .record(is_buy(p.side) ? mark : -mark);
                    else
                        (is_mm ? stats_.adverse_10ms : stats_.mom_adverse_10ms)
                            .record(is_buy(p.side) ? mark : -mark);
                }
            }
            break;
        }
        default:
            break;
    }
}

// ---------------- md retransmit ring (O(1) insert/evict) ----------------

void World::md_ring_push(u64 seq, const Event& e) {
    const size_t slot = (md_head_ + md_count_) % kMdBufferCap;
    md_ring_[slot] = {seq, e};
    if (md_count_ < kMdBufferCap) {
        ++md_count_;
    } else {
        md_head_ = (md_head_ + 1) % kMdBufferCap;  // overwrite the oldest
    }
}

const Event* World::md_ring_find(u64 seq) const {
    if (md_count_ == 0) return nullptr;
    const u64 oldest = md_ring_[md_head_].first;
    if (seq < oldest || seq >= oldest + md_count_) return nullptr;
    const size_t slot = (md_head_ + static_cast<size_t>(seq - oldest)) % kMdBufferCap;
    return md_ring_[slot].first == seq ? &md_ring_[slot].second : nullptr;
}

std::vector<std::pair<u64, Event>> World::md_ring_dump() const {
    std::vector<std::pair<u64, Event>> out;
    out.reserve(md_count_);
    for (size_t i = 0; i < md_count_; ++i)
        out.push_back(md_ring_[(md_head_ + i) % kMdBufferCap]);
    return out;
}

// ---------------- exchange core ----------------

void World::enter_book(Order o, LogicalNs t) {
    o.arrival = t;
    {
        auto st = stages_.find(o.id);
        if (st != stages_.end()) st->second.match = t;
    }
    std::vector<Fill> fills;
    const Status s = book_.add_order(o, fills, nullptr);
    jappend(ev_book_add(0, t, o));

    Qty taker_filled = 0;
    for (const Fill& f : fills) taker_filled += f.qty;
    bool rests = false;
    if (s) {
        rests = o.type == OrderType::Limit && (o.tif == Tif::GTC || o.tif == Tif::GTD);
        // FOK: fills either happen fully or the entry was rejected
    }
    const Qty taker_canceled =
        (!s || rests) ? 0 : o.qty - taker_filled;

    for (const Fill& f : fills) {
        jappend(ev_trade_print(0, t, f));
        sim_.note_trade(f.price);
        stats_.trades++;
        stats_.volume += f.qty;
        const bool for_mm = f.taker_account == cfg_.mm.account || f.maker_account == cfg_.mm.account;
        const bool for_mom = f.taker_account == cfg_.momentum.account ||
                             f.maker_account == cfg_.momentum.account;
        if (for_mm || for_mom) {
            Fill copy = f;
            copy.taker_side = o.side;
            copy.maker_side = is_buy(o.side) ? Side::Sell : Side::Buy;
            Notify n;
            n.kind = 0;
            n.fill = copy;
            n.frame = encode_notify_frame(n);
            if (for_mm) ++stats_.strategy_fills;
            if (for_mom) ++stats_.momentum_fills;
            notifies_.push_back(std::move(n));
            const DeliveryPlan d = exec_ch_.next_delivery();
            if (!d.drop) {
                if (d.corrupt) corrupt_frame(notifies_.back().frame, corrupt_rng_);
                ++stats_.wire_frames_encoded;
                schedule(t + cfg_.publish_latency_ns + d.delay_ns + d.reorder_extra_ns,
                         RankExec, EvExecNotify, 0, notifies_.size() - 1, 0);
            }
        } else {
            ++stats_.bot_fills;
        }
    }

    std::vector<Event> mds;
    for (const Fill& f : fills) {
        mds.push_back(ev_md_trade(0, t, f.price, f.qty));
        if (!book_.find(f.maker_order_id))  // maker fully consumed
            mds.push_back(ev_md_cancel(0, t, f.maker_order_id));
    }
    if (rests && s) {
        const Order* ro = book_.find(o.id);
        if (ro) mds.push_back(ev_md_new(0, t, o.id, o.side, o.price, ro->leaves));
        if (o.tif == Tif::GTD && o.expiry > t) schedule(o.expiry, RankGtd, EvGtdExpiry, o.id, 0, 0);
    }
    publish_md(mds, t);

    if (taker_canceled > 0 && o.account == cfg_.mm.account) {
        Notify n;
        n.kind = 2;  // IOC remainder: pending record removed
        n.fill.taker_order_id = o.id;
        n.fill.taker_account = o.account;
        n.fill.qty = taker_canceled;
        n.frame = encode_notify_frame(n);
        notifies_.push_back(std::move(n));
        const DeliveryPlan d = exec_ch_.next_delivery();
        if (!d.drop) {
            if (d.corrupt) corrupt_frame(notifies_.back().frame, corrupt_rng_);
            ++stats_.wire_frames_encoded;
            schedule(t + cfg_.publish_latency_ns + d.delay_ns + d.reorder_extra_ns,
                     RankExec, EvExecNotify, 2, notifies_.size() - 1, 0);
        }
    }
}

void World::cancel_in_book(u64 order_id, LogicalNs t) {
    const Order* o = book_.find(order_id);
    if (!o) return;  // already gone (filled / unknown): nothing to publish
    const u32 owner = o->account;
    if (Status s = book_.cancel(order_id); s) {
        jappend(ev_book_cancel(0, t, order_id));
        std::vector<Event> mds;
        mds.push_back(ev_md_cancel(0, t, order_id));
        publish_md(mds, t);
        if (owner == cfg_.mm.account) {
            Notify n;
            n.kind = 3;
            n.fill.taker_order_id = order_id;
            n.fill.taker_account = owner;
            n.frame = encode_notify_frame(n);
            notifies_.push_back(std::move(n));
            const DeliveryPlan d = exec_ch_.next_delivery();
            if (!d.drop) {
                if (d.corrupt) corrupt_frame(notifies_.back().frame, corrupt_rng_);
                ++stats_.wire_frames_encoded;
                schedule(t + cfg_.publish_latency_ns + d.delay_ns + d.reorder_extra_ns,
                         RankExec, EvExecNotify, 3, notifies_.size() - 1, 0);
            }
        }
    }
}

void World::publish_md(std::vector<Event>& mds, LogicalNs t) {
    for (const Event& e : mds) {
        const u64 seq = md_seq_++;
        md_ring_push(seq, e);
        jappend(e);
        ++stats_.md_published;
        const DeliveryPlan d = md_ch_.next_delivery();
        if (d.drop) {
            ++stats_.md_dropped;
            continue;
        }
        if (d.duplicate) ++stats_.md_duplicated;
        if (d.reorder_extra_ns) ++stats_.md_reordered;
        pending_md_.emplace(seq, e);
        const i64 delay = d.delay_ns + d.reorder_extra_ns;
        schedule(t + delay, RankMd, EvMdDeliver, 0, seq, 0);
        if (d.duplicate)
            schedule(t + d.dup_extra_ns + d.reorder_extra_ns, RankMd, EvMdDeliver, 0, seq, 0);
    }
}

// ---------------- feed handler (strategy side) ----------------

void World::feed_handler_md(u64 md_seq) {
    if (md_seq < expected_md_seq_) return;  // duplicate after recovery
    if (md_seq > expected_md_seq_) {
        // arrived out of order: hold it (it is already in pending_md_) and
        // request retransmission of whatever is missing
        ++stats_.md_gap_events;
        maybe_schedule_gap_request();
        return;
    }
    deliver_expected();
}

void World::deliver_expected() {
    // deliver expected_md_seq_ and any consecutive held messages after it
    while (true) {
        auto it = pending_md_.find(expected_md_seq_);
        if (it == pending_md_.end()) break;
        const Event& e = it->second;
        ++expected_md_seq_;
        ++stats_.md_delivered;
        switch (e.type) {
            case EvType::MdNew:
                mm_.on_md_new(e.a, e.side, e.p, e.c);
                break;
            case EvType::MdCancel:
                mm_.on_md_cancel(e.a);
                break;
            case EvType::MdTrade:
                mm_.on_md_trade(e.p, e.a);
                mom_.on_md_trade(e.p);
                break;
            default:
                break;
        }
        pending_md_.erase(it);
    }
}

void World::maybe_schedule_gap_request() {
    if (gap_request_pending_) return;
    gap_request_pending_ = true;
    schedule(now_ + 5'000, RankGap, EvGapRequestDue, 0, 0, 0);
}

void World::gap_recover() {
    // Missing seqs are [expected_md_seq_, md_seq_). Messages already held
    // client-side just need their predecessors; truly dropped messages are
    // re-sent from the exchange ring. Beyond the ring -> snapshot recovery.
    const u64 oldest_buffered = md_count_ == 0 ? md_seq_ : md_ring_[md_head_].first;
    if (expected_md_seq_ < oldest_buffered) {
        // snapshot recovery: rebuild the view and resynchronize the stream
        // (the ladder payload is consumed directly via the snapshot path,
        // the wire-frame ladder encoding is future work, documented in
        // docs/NETWORK_MODEL.md)
        mm_.rebuild_view(book_ladder_without_own());
        expected_md_seq_ = md_seq_;
        for (auto it = pending_md_.begin(); it != pending_md_.end();) {
            if (it->first < expected_md_seq_)
                it = pending_md_.erase(it);
            else
                ++it;
        }
        ++stats_.md_snapshot_recoveries;
        ++stats_.md_recovered;
        deliver_expected();
        return;
    }
    for (u64 seq = expected_md_seq_; seq < md_seq_ && seq < oldest_buffered + kMdBufferCap; ++seq) {
        if (pending_md_.count(seq)) continue;  // already in flight client-side
        if (const Event* ev = md_ring_find(seq)) {
            pending_md_.emplace(seq, *ev);
            // deterministic recovery path: base delay, no loss/dup
            schedule(now_ + cfg_.md_channel.base_delay_ns, RankMd, EvMdDeliver, 0, seq, 0);
            ++stats_.md_recovered;
        }
    }
    deliver_expected();
}

// The snapshot payload omits the strategy's own orders; the feed handler
// rebuilds a view of everyone else.
LadderView World::book_ladder_without_own() const {
    LadderView lv = book_.to_ladder();
    auto strip = [&](std::vector<LadderLevel>& levels) {
        for (LadderLevel& lvl : levels) {
            std::vector<LadderOrder> keep;
            for (LadderOrder& o : lvl.orders)
                if (gateway_known_ids_.count(o.id) == 0) keep.push_back(o);
            lvl.orders = std::move(keep);
        }
    };
    strip(lv.bids);
    strip(lv.asks);
    return lv;
}

// ---- interference-control shadow touch ----
// Strategy resting orders are exactly the book ids the gateway accepted
// (gateway_known_ids_); bots' ids never enter that set. The shadow best is
// the best price at which a NON-strategy order rests (0 if none). The
// canonical ladder lists bids best-first (descending) and asks best-first
// (ascending), one scan order covers both sides.
namespace {
Price shadow_best(const std::vector<LadderLevel>& levels,
                  const std::unordered_set<u64>& strategy_ids) {
    for (const LadderLevel& lvl : levels) {
        for (const LadderOrder& o : lvl.orders)
            if (strategy_ids.count(o.id) == 0) return lvl.price;
    }
    return 0;
}
}  // namespace

Price World::shadow_best_bid() const {
    const LadderView lv = book_.to_ladder();
    return shadow_best(lv.bids, gateway_known_ids_);
}

Price World::shadow_best_ask() const {
    const LadderView lv = book_.to_ladder();
    return shadow_best(lv.asks, gateway_known_ids_);
}

void World::deliver_fill(const Fill& f, LogicalNs t) {
    // exchange fee schedule: flat fee per lot, charged to each strategy
    // participant of this fill (bots are documented as fee-exempt v1).
    const auto charge_fee = [&](u32 account, Qty qty) {
        if (cfg_.fee_per_lot == 0) return;
        if (account == cfg_.mm.account) {
            mm_.on_fee(cfg_.fee_per_lot * static_cast<i64>(qty));
        } else if (account == cfg_.momentum.account) {
            mom_.on_fee(cfg_.fee_per_lot * static_cast<i64>(qty));
        }
    };
    if (f.taker_account == cfg_.mm.account || f.maker_account == cfg_.mm.account)
        charge_fee(cfg_.mm.account, f.qty);
    if (f.taker_account == cfg_.momentum.account || f.maker_account == cfg_.momentum.account)
        charge_fee(cfg_.momentum.account, f.qty);

    const auto note_latency = [&](u64 order_id) {
        auto it = order_sent_t_.find(order_id);
        if (it != order_sent_t_.end()) {
            // e2e decision->fill for EVERY fill of the order (partials
            // included); the entry dies only at terminal state so the fill
            // set matches the stage-trace fill set exactly.
            stats_.fill_latency.record(static_cast<double>(t - it->second));
            stats_.fill_latency_sum += static_cast<u64>(t - it->second);
            const Order* resting = book_.find(order_id);
            if (!resting || resting->leaves == 0) order_sent_t_.erase(it);
        }
        note_stage_trace(order_id, t);
    };
    // drawdown tracking: mark the MM's PnL to the book mid at every fill
    // delivery it participates in (peak-to-trough, integer ticks*qty).
    // A momentarily empty book (mid 0) is not a mark point.
    const auto note_drawdown = [&]() {
        const Price bb = book_.best_bid(), ba = book_.best_ask();
        const Price mid = (bb && ba) ? (bb + ba) / 2 : (bb ? bb : ba);
        if (mid <= 0) return;
        const i64 pnl = mm_.cash() + mm_.inventory() * mid;
        if (pnl > stats_.mm_marked_pnl_peak) stats_.mm_marked_pnl_peak = pnl;
        const i64 dd = stats_.mm_marked_pnl_peak - pnl;
        if (dd > stats_.mm_max_drawdown) stats_.mm_max_drawdown = dd;
    };
    const auto note_risk = [&](u32 account, Side ours, Qty qty) {
        risk_.note_fill(is_buy(ours) ? static_cast<i64>(qty) : -static_cast<i64>(qty), account);
    };

    // adverse-selection probes: sample the mid 100us and 10ms after each
    // strategy fill delivery (markout vs fill price, signed by side); the
    // probe carries the account so samples land in the right archetype
    // histogram.
    const auto schedule_markouts = [&](u32 account, Side ours, Price fill_px) {
        if (markouts_.size() > (1u << 20)) return;  // bounded
        MarkoutProbe p;
        p.fill_t = t;
        p.fill_px = fill_px;
        p.side = ours;
        p.account = account;
        markouts_.push_back(p);
        schedule(t + 100'000, RankMarkout, EvMarkout, 0, markouts_.size() - 1, 0);
        schedule(t + 10'000'000, RankMarkout, EvMarkout, 1, markouts_.size() - 1, 0);
    };
    if (f.taker_account == cfg_.mm.account) {
        mm_.on_fill(f.taker_order_id, f.taker_side, f.price, f.qty);
        note_risk(cfg_.mm.account, f.taker_side, f.qty);
        note_latency(f.taker_order_id);
        note_drawdown();
        schedule_markouts(cfg_.mm.account, f.taker_side, f.price);
    } else if (f.maker_account == cfg_.mm.account) {
        mm_.on_fill(f.maker_order_id, f.maker_side, f.price, f.qty);
        note_risk(cfg_.mm.account, f.maker_side, f.qty);
        note_latency(f.maker_order_id);
        note_drawdown();
        schedule_markouts(cfg_.mm.account, f.maker_side, f.price);
    }
    if (f.taker_account == cfg_.momentum.account) {
        mom_.on_fill(f.taker_order_id, f.taker_side, f.price, f.qty);
        note_risk(cfg_.momentum.account, f.taker_side, f.qty);
        note_latency(f.taker_order_id);
        schedule_markouts(cfg_.momentum.account, f.taker_side, f.price);
    } else if (f.maker_account == cfg_.momentum.account) {
        mom_.on_fill(f.maker_order_id, f.maker_side, f.price, f.qty);
        note_risk(cfg_.momentum.account, f.maker_side, f.qty);
        note_latency(f.maker_order_id);
        schedule_markouts(cfg_.momentum.account, f.maker_side, f.price);
    }
}

// ---------------- intent routing ----------------

void World::route_intent(const StrategyAction& act, LogicalNs t) {
    const DeliveryPlan d = order_ch_.next_delivery();
    if (d.drop) {
        ++stats_.order_frames_dropped;
        return;  // documented v1 limitation: no client-side send timeout
    }
    Msg m;
    m.frame = encode_order_frame(act, cfg_.mm.account);
    // message-type granularity: cancels use their own decision latency when
    // configured (cancel_latency_ns >= 0), else the shared algo latency
    const bool is_cancel = act.kind == StrategyAction::Kind::Cancel;
    const LogicalNs decision_latency = cfg_.decision_latency_for(is_cancel);
    const LogicalNs depart_t = t + decision_latency;
    if (act.kind == StrategyAction::Kind::Send) {
        m.is_cancel = false;
        m.ord = act.ord;
        order_sent_t_[act.ord.id] = t;
        if (cfg_.stage_tracing) {
            stages_[act.ord.id] = StageTimes{t, depart_t, 0, 0};
            ++stats_.stage_traced_orders;
        }
        ++stats_.strategy_orders_sent;
        jappend(Event{0, t, EvType::Session, act.ord.account, act.ord.side, act.ord.type,
                      act.ord.tif, act.ord.id, 1, 0, 0});
    } else {
        m.is_cancel = true;
        m.ord.id = act.cancel_id;
        m.ord.account = cfg_.mm.account;
        ++stats_.strategy_cancels_sent;
        jappend(Event{0, t, EvType::Session, cfg_.mm.account, Side::Buy, OrderType::Limit,
                      Tif::GTC, act.cancel_id, 2, 0, 0});
    }
    // in-transit corruption (deterministic draw): the frame is damaged AFTER
    // encode; the gateway CRC rejects it and the intent is lost (counted).
    if (d.corrupt) corrupt_frame(m.frame, corrupt_rng_);
    ++stats_.wire_frames_encoded;
    const bool cancel = m.is_cancel;
    const size_t idx = inflight_.size();
    inflight_.push_back(std::move(m));
    schedule(t + decision_latency + d.delay_ns + d.reorder_extra_ns, RankGateway,
             EvGatewayArrival, cancel ? 1 : 0, idx, 0);
}

// ---------------- wire frame codecs (live path) ----------------

// Order-intent frame payload convention (58-byte canonical Event):
//   Send:   type=BookAdd, a=id, b=expiry, c=qty, p=price,
//           account/side/otype/tif as-is
//   Cancel: type=BookCancel, a=target id, account=owner
std::string World::encode_order_frame(const StrategyAction& act, u32 mm_account) {
    Event e;
    e.t = 0;
    if (act.kind == StrategyAction::Kind::Send) {
        e.type = EvType::BookAdd;
        e.account = act.ord.account;
        e.side = act.ord.side;
        e.otype = act.ord.type;
        e.tif = act.ord.tif;
        e.a = act.ord.id;
        e.b = static_cast<u64>(act.ord.expiry);  // slot-b bit pattern (expiry >= 0)
        e.c = act.ord.qty;
        e.p = act.ord.price;
    } else {
        e.type = EvType::BookCancel;
        e.account = mm_account;
        e.a = act.cancel_id;
    }
    return wire_encode(2 /* order channel */, 0, kFlagNone, e);
}

Order World::decode_order_frame(const std::string& frame, bool& is_cancel, bool& ok) {
    is_cancel = false;
    ok = false;
    auto d = wire_decode(reinterpret_cast<const u8*>(frame.data()), frame.size());
    if (!d || (d.value.flags & (kFlagSnapshot | kFlagExecReport)) != 0) return {};
    const Event& e = d.value.ev;
    Order o;
    o.arrival = 0;
    o.book_seq = 0;
    if (e.type == EvType::BookCancel) {
        is_cancel = true;
        o.id = e.a;
        o.account = e.account;
        ok = true;
        return o;
    }
    if (e.type != EvType::BookAdd) return {};
    o.id = e.a;
    o.account = e.account;
    o.side = e.side;
    o.type = e.otype;
    o.tif = e.tif;
    o.price = e.p;
    o.qty = e.c;
    o.leaves = e.c;
    o.expiry = static_cast<LogicalNs>(e.b);
    ok = true;
    return o;
}

std::string World::encode_notify_frame(const Notify& n) {
    ExecReport r;
    r.kind = n.kind;
    r.taker_id = n.fill.taker_order_id;
    r.maker_id = n.fill.maker_order_id;
    r.taker_acct = n.fill.taker_account;
    r.maker_acct = n.fill.maker_account;
    r.price = n.fill.price;
    r.qty = n.fill.qty;
    r.taker_side = n.fill.taker_side;
    r.maker_side = n.fill.maker_side;
    if (n.kind == 1) r.reason = n.fill.maker_order_id;  // reject reason rides the maker-id slot
    return wire_encode_exec(3 /* exec channel */, 0, r);
}

void World::corrupt_frame(std::string& frame, Rng& rng) {
    if (frame.size() <= kFrameHeaderSize) return;
    const u64 pos = kFrameHeaderSize + rng.bounded(frame.size() - kFrameHeaderSize);
    frame[static_cast<size_t>(pos)] = static_cast<char>(frame[static_cast<size_t>(pos)] ^ 0x5A);
}

// State-hash contract (tag ARBITERX/WORLD/2; see docs/REPRODUCIBILITY.md):
// the hash is a deterministic identity of the world's EVENT-RELEVANT state,
// everything that can influence future events. Covered: logical clock and
// progress counters, exchange book ladder + per-order account ownership,
// full strategy state (incl. local view ladder and queue-ahead), per-account
// risk state, sim dynamics + RNG-relevant progress, network channel RNG +
// burst states + corruption stream, feed-handler progress + pending buffer,
// retransmit-ring progress, gateway id set, in-flight order/notify frames,
// pending markout probes, scheduler contents. Deliberately EXCLUDED:
//   * journal bytes, RunStats histograms/timelines, wall-clock;
//   * order_sent_t_ / stages_: per-order latency-tracing tables that feed ONLY
//     RunStats histograms (never control flow). stages_ is additionally NOT
//     snapshot-persisted (cleared on restore by design), so hashing it would
//     break the restore contract; order_sent_t_ stays snapshot-persisted for
//     stats continuity but is not part of state identity.
//   * cfg_hash_: configuration identity belongs to config_hash()/world_id().
//     Binding it into state_hash would make fork-divergence assertions
//     vacuous (any fork would "diverge" trivially via the config tag).
// This is NOT cryptographic uniqueness, it is deterministic state identity
// under this documented serialization (FNV-1a 64, fixed field order, sorted
// map/set iteration).
u64 World::state_hash() const {
    Hasher h;
    h.tag("ARBITERX/WORLD/2");
    h.u64v(static_cast<u64>(now_));
    h.u64v(events_);
    h.u64v(tiebreak_);
    h.boolean(started_);
    h.boolean(halted_);
    canonical_book_hash(book_.to_ladder(), h);
    // Order-account ownership routes fills/cancels: identity matters.
    {
        auto accts = book_.order_accounts();
        std::sort(accts.begin(), accts.end(),
                  [](const auto& x, const auto& y) { return x.first < y.first; });
        h.tag("ACCT");
        h.u64v(accts.size());
        for (const auto& [id, acct] : accts) {
            h.u64v(id);
            h.u32v(acct);
        }
    }
    h.tag("MM");
    mm_.hash_into(h);
    h.tag("MOM");
    mom_.hash_into(h);
    // Per-account risk state (aggregates alone allow cross-account collision).
    h.tag("RISK");
    {
        u64 n = 0;
        risk_.for_each_state([&](u32, const RiskState&) { ++n; });
        h.u64v(n);
        risk_.for_each_state([&](u32 acct, const RiskState& st) {
            h.u32v(acct);
            h.i64v(st.position);
            h.u64v(st.accepted);
            h.u64v(st.rejected);
            for (size_t i = 0; i <= kRiskDuplicateId; ++i) h.u64v(st.rejects_by_reason[i]);
            h.u64v(st.window.size());
            for (LogicalNs t : st.window) h.i64v(t);
        });
    }
    // Simulator: regime + progress + RNG-relevant dynamics.
    h.tag("SIM");
    {
        const MarketSim::SimState ss = sim_.state();
        h.u64v(ss.provider_rng);
        h.u64v(ss.taker_rng);
        h.u64v(ss.regime_rng);
        h.u64v(ss.price_rng);
        h.i64v(ss.next_provider_t);
        h.i64v(ss.next_taker_t);
        h.i64v(ss.regime);
        h.i64v(ss.regime_until);
        h.u64v(ss.actions);
        h.i64v(ss.bias);
        h.i64v(ss.last_trade_px);
        h.u64v(ss.next_bot_id);
        h.i64v(ss.last_bb);
        h.i64v(ss.last_ba);
        h.i64v(ss.now);
        h.boolean(ss.taker_boost_active);  // self-excitation memory: behavior-relevant
        h.u8v(ss.last_taker_side);          // sign-persistence memory
        h.u64v(ss.live_provider_orders.size());
        for (u64 id : ss.live_provider_orders) h.u64v(id);
    }
    // Network: channel RNG + burst states + corruption stream.
    h.tag("NET");
    h.u64v(md_ch_.state());
    h.u64v(md_ch_.burst_state());
    h.u64v(order_ch_.state());
    h.u64v(order_ch_.burst_state());
    h.u64v(exec_ch_.state());
    h.u64v(exec_ch_.burst_state());
    h.u64v(corrupt_rng_.state);
    h.u64v(md_seq_);
    h.u64v(expected_md_seq_);
    h.boolean(gap_request_pending_);
    // Feed-handler pending buffer (sorted map: deterministic order).
    h.u64v(pending_md_.size());
    for (const auto& [seq, ev] : pending_md_) {
        h.u64v(seq);
        h.u64v(event_hash(ev));
    }
    // Retransmit ring progress: the snapshot persists the logical
    // oldest->newest CONTENTS (not the physical head index, which is a
    // rotation artifact of the circular buffer, restore rebuilds head=0
    // with identical logical order and identical eviction semantics). Hash
    // the logical contents in dump order.
    h.tag("MDRING");
    {
        const std::vector<std::pair<u64, Event>> dump = md_ring_dump();
        h.u64v(dump.size());
        for (const auto& [seq, ev] : dump) {
            h.u64v(seq);
            h.u64v(event_hash(ev));
        }
    }
    // Gateway id set (sorted: unordered_set iteration is not deterministic).
    {
        std::vector<u64> ids(gateway_known_ids_.begin(), gateway_known_ids_.end());
        std::sort(ids.begin(), ids.end());
        h.tag("GW");
        h.u64v(ids.size());
        for (u64 id : ids) h.u64v(id);
    }
    // In-flight order frames (authoritative bytes hashed).
    h.tag("FLIGHT");
    h.u64v(inflight_.size());
    for (const auto& m : inflight_) {
        h.boolean(m.is_cancel);
        h.u64v(m.ord.id);
        h.u32v(m.ord.account);
        h.u8v(static_cast<u8>(m.ord.side));
        h.u8v(static_cast<u8>(m.ord.type));
        h.u8v(static_cast<u8>(m.ord.tif));
        h.i64v(m.ord.price);
        h.u64v(m.ord.qty);
        h.u64v(m.ord.leaves);
        h.str(m.frame);
    }
    // In-flight execution notifications.
    h.tag("NOTIFY");
    h.u64v(notifies_.size());
    for (const auto& n : notifies_) {
        h.u8v(n.kind);
        h.u64v(n.fill.taker_order_id);
        h.u64v(n.fill.maker_order_id);
        h.u32v(n.fill.maker_account);
        h.u32v(n.fill.taker_account);
        h.i64v(n.fill.price);
        h.u64v(n.fill.qty);
        h.i64v(n.fill.t);
        h.str(n.frame);
    }
    // Pending markout probes (execution-research state).
    h.tag("MARKOUT");
    h.u64v(markouts_.size());
    for (const auto& m : markouts_) {
        h.i64v(m.fill_t);
        h.i64v(m.fill_px);
        h.u8v(static_cast<u8>(m.side));
    }
    // Decision-time and stage-trace tables are INSTRUMENTATION (they feed
    // only RunStats histograms, never control flow) and stages_ is not even
    // snapshot-persisted, both are intentionally outside state identity.
    // Scheduler contents in pop order (copy-then-drain: deterministic).
    h.tag("SCHED");
    {
        auto q = queue_;
        h.u64v(q.size());
        while (!q.empty()) {
            const WorldEvent e = q.top();
            q.pop();
            h.i64v(e.t);
            h.u8v(e.rank);
            h.u64v(e.tiebreak);
            h.u16v(static_cast<u16>(e.type));
            h.u64v(e.a);
            h.u64v(e.b);
            h.u64v(e.c);
            h.i64v(e.p);
        }
    }
    return h.value();
}

// ---------------- stage tracing (decision -> fill decomposition) ----------------

void World::note_stage_trace(u64 order_id, LogicalNs fill_t) {
    auto it = stages_.find(order_id);
    if (it == stages_.end()) return;
    const StageTimes& st = it->second;
    // A taker fill traverses all four hops. A maker fill rests between
    // gateway-accept and match: decision->depart->gateway still measure the
    // outbound path; gateway->match is the RESTING duration until the taker
    // arrived; match->fill the exec-channel return. The four sum to the
    // end-to-end decision->fill latency either way, for EVERY fill of the
    // order, including partial maker fills (the resting intervals overlap
    // across partial fills; each fill still sums exactly).
    const u64 d1 = static_cast<u64>(st.depart - st.decision);
    const u64 d2 = static_cast<u64>(st.gateway - st.depart);
    const u64 d3 = static_cast<u64>(st.match - st.gateway);
    const u64 d4 = static_cast<u64>(fill_t - st.match);
    stats_.stage_decision_to_depart.record(static_cast<double>(d1));
    stats_.stage_depart_to_gateway.record(static_cast<double>(d2));
    stats_.stage_gateway_to_match.record(static_cast<double>(d3));
    stats_.stage_match_to_fill.record(static_cast<double>(d4));
    stats_.stage_sum_decision_to_depart += d1;
    stats_.stage_sum_depart_to_gateway += d2;
    stats_.stage_sum_gateway_to_match += d3;
    stats_.stage_sum_match_to_fill += d4;
    // fill_latency_sum is owned by note_latency (the e2e record); it must
    // count each fill exactly once.
    ++stats_.stage_traced_fills;
    // Terminal state: the order is fully consumed (or never rested). A
    // partially-filled resting order keeps its trace for the next fill.
    const Order* resting = book_.find(order_id);
    if (!resting || resting->leaves == 0) stages_.erase(it);
}

void World::drop_stage_trace(u64 order_id) {
    // Terminal state reached without (further) fills: cancel confirmed,
    // risk reject, IOC remainder, corrupted frame. Both per-order
    // instrumentation tables die together so their fill sets stay aligned.
    stages_.erase(order_id);
    order_sent_t_.erase(order_id);
}

// ---------------- full-world checkpointing ----------------

namespace {
// Read-only access to a priority_queue's underlying container (standard
// protected-member access via a derived view; no mutation).
template <typename T, typename C>
const std::vector<T>& pq_underlying(const std::priority_queue<T, std::vector<T>, C>& q) {
    struct Accessor : std::priority_queue<T, std::vector<T>, C> {
        static const std::vector<T>& get(const std::priority_queue<T, std::vector<T>, C>& pq) {
            return pq.*(&Accessor::c);
        }
    };
    return Accessor::get(q);
}
}  // namespace

WorldSnapshot World::capture_snapshot() const {
    WorldSnapshot s;
    s.config_hash = cfg_hash_;
    s.master_seed = master_seed_;
    s.state_hash = state_hash();
    s.events = events_;
    s.tiebreak = tiebreak_;
    s.now = now_;
    s.md_seq = md_seq_;
    s.expected_md_seq = expected_md_seq_;
    s.gap_pending = gap_request_pending_;
    s.halted = halted_;
    s.next_book_seq = book_.next_book_seq();

    for (const auto& ev : pq_underlying(queue_))
        s.queue.push_back(SnapSchedEvent{ev.t, ev.rank, static_cast<i64>(ev.tiebreak),
                                         static_cast<i64>(ev.type), static_cast<i64>(ev.a),
                                         static_cast<i64>(ev.b), static_cast<i64>(ev.c), ev.p});

    s.book = book_.to_ladder();
    s.order_accounts = book_.order_accounts();
    s.sim = sim_.state();

    s.mm.inventory = mm_.inventory();
    s.mm.cash = mm_.cash();
    s.mm.fair = mm_.fair();
    s.mm.have_md = mm_.has_market_data();
    s.mm.next_order_id = mm_.next_order_id();
    s.mm.inference_fills = mm_.inference_fills();
    s.mm.real_fills = mm_.real_fills();
    s.mm.real_filled_qty = mm_.real_filled_qty();
    s.mm.ambiguous_qty = mm_.ambiguous_fill_qty();
    s.mm.quote_updates = mm_.quote_updates();
    s.mm.confirm_seq_counter = mm_.confirm_seq_counter();
    for (const auto& o : mm_.own_snapshot())
        s.mm.own.push_back(SnapOwn{o.id, static_cast<u8>(o.side), o.price, o.leaves, o.ahead,
                                   static_cast<u8>(o.confirmed ? 1 : 0), o.confirm_seq});
    s.mm.view = mm_.view_ladder();

    s.mom.inventory = mom_.inventory();
    s.mom.cash = mom_.cash();
    s.mom.last_px = mom_.last_px();
    s.mom.next_order_id = mom_.next_order_id();
    s.mom.filled_qty = mom_.filled_qty();
    s.mom.signs = mom_.signs_window();

    s.risk_accounts.clear();
    risk_.for_each_state([&](u32 account, const RiskState& rs) {
        SnapRiskAccount ra;
        ra.account = account;
        ra.position = rs.position;
        ra.accepted = rs.accepted;
        ra.rejected = rs.rejected;
        for (size_t i = 0; i < ra.by_reason.size() && i < 8; ++i)
            ra.by_reason[i] = rs.rejects_by_reason[i];
        ra.window = rs.window;
        s.risk_accounts.push_back(ra);
    });

    s.gateway_known_ids.assign(gateway_known_ids_.begin(), gateway_known_ids_.end());
    std::sort(s.gateway_known_ids.begin(), s.gateway_known_ids.end());
    s.channel_rng = {md_ch_.state(), order_ch_.state(), exec_ch_.state()};
    s.channel_burst = {md_ch_.burst_state(), order_ch_.burst_state(), exec_ch_.burst_state()};
    s.corrupt_rng = corrupt_rng_.state;

    s.md_buffer = md_ring_dump();
    s.pending_md.assign(pending_md_.begin(), pending_md_.end());
    s.inflight.reserve(inflight_.size());
    for (const auto& m : inflight_) {
        SnapMsg sm;
        sm.is_cancel = m.is_cancel;
        const Order& o = m.ord;
        sm.ord = SnapOrder{o.id, o.book_seq, o.account, static_cast<u8>(o.side),
                           static_cast<u8>(o.type), static_cast<u8>(o.tif), o.price, o.qty,
                           o.leaves, o.arrival, o.expiry};
        sm.frame = m.frame;
        s.inflight.push_back(sm);
    }
    s.notifies.reserve(notifies_.size());
    for (const auto& n : notifies_) {
        SnapNotify sn;
        sn.kind = n.kind;
        const Fill& f = n.fill;
        sn.fill = SnapFill{f.taker_order_id, f.maker_order_id, f.maker_account, f.taker_account,
                           f.price, f.qty, f.t, static_cast<u8>(f.maker_liq),
                           static_cast<u8>(f.taker_side), static_cast<u8>(f.maker_side)};
        sn.frame = n.frame;
        s.notifies.push_back(sn);
    }
    s.markouts.reserve(markouts_.size());
    for (const auto& m : markouts_) {
        SnapMarkout sm;
        sm.fill_t = m.fill_t;
        sm.fill_px = m.fill_px;
        sm.side = static_cast<u8>(m.side);
        sm.account = m.account;
        s.markouts.push_back(sm);
    }
    s.order_sent_t.reserve(order_sent_t_.size());
    for (const auto& [id, t] : order_sent_t_) s.order_sent_t.emplace_back(id, t);
    std::sort(s.order_sent_t.begin(), s.order_sent_t.end());

    s.stats.trades = stats_.trades;
    s.stats.volume = stats_.volume;
    s.stats.bot_fills = stats_.bot_fills;
    s.stats.strategy_fills = stats_.strategy_fills;
    s.stats.momentum_fills = stats_.momentum_fills;
    s.stats.strategy_orders_sent = stats_.strategy_orders_sent;
    s.stats.strategy_cancels_sent = stats_.strategy_cancels_sent;
    s.stats.order_frames_dropped = stats_.order_frames_dropped;
    s.stats.risk_accepted = stats_.risk_accepted;
    s.stats.risk_rejected = stats_.risk_rejected;
    s.stats.md_published = stats_.md_published;
    s.stats.md_delivered = stats_.md_delivered;
    s.stats.md_dropped = stats_.md_dropped;
    s.stats.md_duplicated = stats_.md_duplicated;
    s.stats.md_reordered = stats_.md_reordered;
    s.stats.md_gap_events = stats_.md_gap_events;
    s.stats.md_recovered = stats_.md_recovered;
    s.stats.md_snapshot_recoveries = stats_.md_snapshot_recoveries;
    s.stats.wire_frames_encoded = stats_.wire_frames_encoded;
    s.stats.wire_frames_decoded = stats_.wire_frames_decoded;
    s.stats.wire_frames_corrupted = stats_.wire_frames_corrupted;
    return s;
}

Status World::restore_snapshot(const WorldSnapshot& s, const WorldConfig& continuation_cfg,
                               bool strict_config) {
    if (s.format_version != kWorldSnapshotVersion) return fail(Errc::Unsupported, s.format_version);
    if (strict_config && s.config_hash != continuation_cfg.hash())
        return fail(Errc::SnapshotInvalid, 1);
    if (s.master_seed != master_seed_) return fail(Errc::SnapshotInvalid, 2);

    now_ = s.now;
    events_ = s.events;
    tiebreak_ = s.tiebreak;
    md_seq_ = s.md_seq;
    expected_md_seq_ = s.expected_md_seq;
    gap_request_pending_ = s.gap_pending != 0;
    halted_ = s.halted != 0;

    while (!queue_.empty()) queue_.pop();
    for (const auto& e : s.queue)
        queue_.push(WorldEvent{e.t, static_cast<u8>(e.rank), static_cast<u64>(e.tiebreak),
                               static_cast<EvType>(e.type), static_cast<u64>(e.a),
                               static_cast<u64>(e.b), static_cast<u64>(e.c), e.p});

    // book: rebuild via rest_order (canonical ladder), restore the metadata counter
    book_ = OptBook{OptBook::Options{cfg_.book_min_price, cfg_.book_max_price, 1u << 14}};
    book_.set_next_book_seq(s.next_book_seq);
    std::vector<Fill> scratch;
    // book: rebuild via rest_order (canonical ladder), restore the metadata
    // counter. A failed rest (corrupt snapshot ladder) must abort the
    // restore loudly, the state-hash check would also catch it, but a
    // silent skip produces a confusing hash mismatch instead of the cause.
    for (const auto& lvl : s.book.bids)
        for (const auto& o : lvl.orders)
            if (Status st = book_.rest_order(Order{o.id, 0, Side::Buy, OrderType::Limit, Tif::GTC,
                                                   lvl.price, o.qty, o.leaves, 0, 0, 0});
                !st)
                return st;
    for (const auto& lvl : s.book.asks)
        for (const auto& o : lvl.orders)
            if (Status st = book_.rest_order(Order{o.id, 0, Side::Sell, OrderType::Limit, Tif::GTC,
                                                   lvl.price, o.qty, o.leaves, 0, 0, 0});
                !st)
                return st;
    // ownership: restore each resting order's account (notification routing)
    for (const auto& [id, acct] : s.order_accounts) book_.set_order_account(id, acct);

    sim_.restore(s.sim);

    mm_.restore_full(s.mm.inventory, s.mm.cash, s.mm.fair, s.mm.have_md != 0, s.mm.next_order_id,
                     s.mm.inference_fills, s.mm.real_fills, s.mm.real_filled_qty,
                     s.mm.ambiguous_qty, s.mm.quote_updates, s.mm.confirm_seq_counter, {},
                     s.mm.view);
    // own orders include confirmation state; re-add via the extended snapshots
    for (const auto& o : s.mm.own) {
        MarketMakerStrategy::OwnSnapshot os{o.id, static_cast<Side>(o.side), o.price, o.leaves,
                                            o.ahead, o.confirmed != 0, o.confirm_seq};
        mm_.add_restored_own(os);
    }

    mom_.restore_full(s.mom.inventory, s.mom.cash, s.mom.last_px, s.mom.next_order_id,
                      s.mom.filled_qty, s.mom.signs);

    // risk: restore every account's isolated state (v6). Clear first so a
    // restore into an already-used engine cannot leak prior account state.
    {
        risk_.clear_states();
        for (const auto& ra : s.risk_accounts) {
            RiskState rs;
            rs.position = ra.position;
            rs.accepted = ra.accepted;
            rs.rejected = ra.rejected;
            for (size_t i = 0; i < 8; ++i) rs.rejects_by_reason[i] = ra.by_reason[i];
            rs.window = ra.window;
            risk_.restore_state(ra.account, rs);
        }
    }

    gateway_known_ids_.clear();
    gateway_known_ids_.insert(s.gateway_known_ids.begin(), s.gateway_known_ids.end());
    md_ch_.set_state(s.channel_rng[0]);
    order_ch_.set_state(s.channel_rng[1]);
    exec_ch_.set_state(s.channel_rng[2]);
    md_ch_.set_burst_state(s.channel_burst[0]);
    order_ch_.set_burst_state(s.channel_burst[1]);
    exec_ch_.set_burst_state(s.channel_burst[2]);
    corrupt_rng_.state = s.corrupt_rng;

    md_ring_ = {};
    md_head_ = 0;
    md_count_ = 0;
    for (const auto& [seq, ev] : s.md_buffer) md_ring_push(seq, ev);
    pending_md_.clear();
    for (const auto& [seq, ev] : s.pending_md) pending_md_.emplace(seq, ev);

    inflight_.clear();
    for (const auto& m : s.inflight) {
        Msg mm2;
        mm2.is_cancel = m.is_cancel != 0;
        const SnapOrder& so = m.ord;
        mm2.ord = Order{so.id, so.account, static_cast<Side>(so.side),
                        static_cast<OrderType>(so.otype), static_cast<Tif>(so.tif), so.price,
                        so.qty, so.leaves, so.arrival, so.book_seq, so.expiry};
        mm2.frame = m.frame;
        inflight_.push_back(std::move(mm2));
    }
    notifies_.clear();
    for (const auto& n : s.notifies) {
        Notify nn;
        nn.kind = n.kind;
        const SnapFill& sf = n.fill;
        nn.fill = Fill{sf.taker_id, sf.maker_id, sf.maker_acct, sf.taker_acct, sf.price, sf.qty,
                       sf.t, static_cast<Liquidity>(sf.maker_liq), static_cast<Side>(sf.taker_side),
                       static_cast<Side>(sf.maker_side)};
        nn.frame = n.frame;
        notifies_.push_back(std::move(nn));
    }
    markouts_.clear();
    markouts_.reserve(s.markouts.size());
    for (const auto& m : s.markouts) {
        MarkoutProbe mp;
        mp.fill_t = m.fill_t;
        mp.fill_px = m.fill_px;
        mp.side = static_cast<Side>(m.side);
        mp.account = m.account;
        markouts_.push_back(mp);
    }
    order_sent_t_.clear();
    for (const auto& [id, t] : s.order_sent_t) order_sent_t_[id] = t;
    stages_.clear();  // in-flight stage traces die with their orders

    stats_.trades = s.stats.trades;
    stats_.volume = s.stats.volume;
    stats_.bot_fills = s.stats.bot_fills;
    stats_.strategy_fills = s.stats.strategy_fills;
    stats_.momentum_fills = s.stats.momentum_fills;
    stats_.strategy_orders_sent = s.stats.strategy_orders_sent;
    stats_.strategy_cancels_sent = s.stats.strategy_cancels_sent;
    stats_.order_frames_dropped = s.stats.order_frames_dropped;
    stats_.risk_accepted = s.stats.risk_accepted;
    stats_.risk_rejected = s.stats.risk_rejected;
    stats_.md_published = s.stats.md_published;
    stats_.md_delivered = s.stats.md_delivered;
    stats_.md_dropped = s.stats.md_dropped;
    stats_.md_duplicated = s.stats.md_duplicated;
    stats_.md_reordered = s.stats.md_reordered;
    stats_.md_gap_events = s.stats.md_gap_events;
    stats_.md_recovered = s.stats.md_recovered;
    stats_.md_snapshot_recoveries = s.stats.md_snapshot_recoveries;
    stats_.wire_frames_encoded = s.stats.wire_frames_encoded;
    stats_.wire_frames_decoded = s.stats.wire_frames_decoded;
    stats_.wire_frames_corrupted = s.stats.wire_frames_corrupted;

    started_ = true;

    // self-validation: a complete snapshot must reproduce the captured hash
    if (const u64 h = state_hash(); h != s.state_hash) return fail(Errc::SnapshotInvalid, h);
    return good();
}

}  // namespace arbiter