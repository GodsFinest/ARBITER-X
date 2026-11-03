#include "arbiter/world.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/rng.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace arbiter {

namespace {
LogicalNs add_time_checked(LogicalNs base, LogicalNs delta) {
    if (delta < 0 || base > std::numeric_limits<LogicalNs>::max() - delta)
        throw std::overflow_error("arbiter: logical time overflow");
    return base + delta;
}

OptBook::Options checked_book_options(const WorldConfig& cfg) {
    const std::string invalid = validate_world_config(cfg);
    if (!invalid.empty()) throw std::invalid_argument("arbiter: invalid WorldConfig: " + invalid);
    return OptBook::Options{cfg.book_min_price, cfg.book_max_price, 1u << 14};
}
}  // namespace

World::World(const WorldConfig& cfg, u64 master_seed)
    : cfg_(cfg),
      cfg_hash_(cfg.hash()),
      master_seed_(master_seed),
      book_(checked_book_options(cfg)),
      sim_(cfg.sim, master_seed),
      mm_(cfg.mm),
      mom_(cfg.momentum),
      risk_(cfg.risk),
      md_ch_(1, cfg.md_channel, master_seed),
      order_ch_(2, cfg.order_channel, master_seed),
      exec_ch_(3, cfg.exec_channel, master_seed),
      corrupt_rng_(Rng::stream(master_seed, 0x4300'5250'4E47ull)) {
    // A degenerate configuration is a PROGRAMMING/CLI error, not a runtime
    // condition: reject it loudly (validate_world_config is the documented
    // entry point CLIs call first; this backstop keeps direct construction
    // honest without aborting the process).
    const std::string invalid = validate_world_config(cfg_);
    if (!invalid.empty()) throw std::invalid_argument("arbiter: invalid WorldConfig: " + invalid);
    if (cfg_.journal_events) {
        std::string path = cfg_.journal_path.empty()
                               ? "artifacts/world_" + std::to_string(master_seed_) + ".journal"
                               : cfg_.journal_path;
        auto w = JournalWriter::create(path, master_seed_, cfg_hash_);
        if (!w)
            throw std::runtime_error("arbiter: cannot create journal " + path + " (" +
                                     std::string(to_cstr(w.status.code)) + ")");
        journal_ = std::make_unique<JournalWriter>(std::move(w.value));
        journal_path_ = path;
    }
}

World::~World() = default;

void World::schedule_seed_events() {
    schedule(sim_.next_action_time(), RankSim, EvSimAction);
    schedule(add_time_checked(cfg_.warmup_ns, cfg_.mm.requote_interval_ns), RankIntent,
             EvIntentDue, 0, 0, 0);
    schedule(add_time_checked(cfg_.warmup_ns, cfg_.momentum.cooldown_ns), RankIntent,
             EvIntentDue, 1, 0, 0);
}

void World::jappend(const Event& e) {
    // journal_records counts only events actually written to a journal,
    // a journal-off run must report 0, not "events that would have been
    // journalled"
    if (journal_) {
        if (const Status st = journal_->append(e); !st)
            throw std::runtime_error("arbiter: journal append failed (" +
                                     std::string(to_cstr(st.code)) + ")");
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
    // exchange-authoritative economics (match-time ledger; see ledger_fill)
    for (const auto& [acct, e] : ledger_) {
        if (acct == cfg_.mm.account) {
            stats_.mm_auth_inventory = e.position;
            stats_.mm_auth_cash = e.cash;
            stats_.mm_auth_fees = e.fees;
            stats_.mm_auth_filled_qty = e.filled_qty;
        } else if (acct == cfg_.momentum.account) {
            stats_.mom_auth_inventory = e.position;
            stats_.mom_auth_cash = e.cash;
            stats_.mom_auth_fees = e.fees;
            stats_.mom_auth_filled_qty = e.filled_qty;
        }
    }
    stats_.risk_accepted = risk_.state().accepted;
    stats_.risk_rejected = risk_.state().rejected;
    if (journal_) {
        if (const Status st = journal_->sync(); !st)
            throw std::runtime_error("arbiter: journal sync failed (" +
                                     std::string(to_cstr(st.code)) + ")");
    }
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
            schedule(add_time_checked(now_, period), RankIntent, EvIntentDue, ev.a, 0, 0);
            break;
        }
        case static_cast<u16>(EvGatewayArrival): {
            Msg& m = inflight_[ev.b];
            // GATEWAY DECODE: the order is reconstructed from the wire frame
            // only: CRC failure (in-transit corruption) rejects the frame,
            // journals a Fault, and the world continues (path
            // integrity; the message is gone, like a real gateway would see).
            bool is_cancel = false, ok = false;
            u8 channel = 0;
            u64 seq = 0;
            const Order decoded = decode_order_frame(m.frame, is_cancel, ok, channel, seq);
            if (!ok || channel != 2) {
                // CRC/structure failure: the frame is unintelligible, a real
                // gateway could not attribute it either.
                ++stats_.wire_frames_corrupted;
                Event flt;
                flt.t = now_;
                flt.type = EvType::Fault;
                flt.a = 2;  // fault kind: order frame rejected at gateway
                flt.b = m.frame.size();
                jappend(flt);
                m.delivered = true;
                // Only a corrupted SEND is terminal for its order; a
                // corrupted CANCEL must not terminate the (still live)
                // target order's trace state.
                if (!m.is_cancel) drop_stage_trace(m.ord.id);
                break;
            }
            // TRANSPORT DEDUPE by decoded channel+sequence: a duplicated
            // delivery of an already-accepted frame is dropped here. It is
            // idempotent, NOT a business-level duplicate-order reject (the
            // duplicate-order reject path only fires for a NEW order reusing
            // a known id).
            if (!order_rx_seqs_.insert(seq).second) {
                ++stats_.order_frames_duplicated;
                m.delivered = true;
                break;
            }
            m.ord = decoded;  // gateway operates on the decoded order
            m.is_cancel = is_cancel;
            m.delivered = true;
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
                // reserve the full working-order exposure NOW (gateway
                // acceptance); the exchange releases/consumes it as the order
                // resolves, never on report delivery.
                risk_.reserve(m.ord.id, m.ord.account,
                              is_buy(m.ord.side) ? static_cast<i64>(m.ord.qty)
                                                 : -static_cast<i64>(m.ord.qty));
                {
                    auto st = stages_.find(m.ord.id);
                    if (st != stages_.end()) st->second.gateway = now_;
                }
                schedule(add_time_checked(now_, cfg_.exchange_processing_ns), RankGateway,
                         EvMatchDue, 0, ev.b, 0);
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
                send_notify(std::move(n), now_);
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
            if (!decoded_ok || df.channel != 3 || (df.flags & kFlagExecReport) == 0) {
                ++stats_.wire_frames_corrupted;
                Event flt;
                flt.t = now_;
                flt.type = EvType::Fault;
                flt.a = 3;  // fault kind: exec frame rejected at strategy feed
                flt.b = n.frame.size();
                jappend(flt);
                n.delivered = true;
                break;
            }
            // TRANSPORT DEDUPE by decoded channel+sequence: a duplicated
            // delivery of an already-accepted report is dropped here, it must
            // not double fills, fees, PnL, markouts or risk updates.
            if (!exec_rx_seqs_.insert(df.md_seq).second) {
                ++stats_.exec_frames_duplicated;
                n.delivered = true;
                break;
            }
            n.delivered = true;
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
                resolve_open_fill_reports(f);
                deliver_fill(f, now_);
            } else if (x.kind == 1) {
                if (x.taker_acct == cfg_.mm.account) mm_.on_reject(x.taker_id);
                drop_stage_trace(x.taker_id);
            } else if (x.kind == 2) {
                if (x.taker_acct == cfg_.mm.account) mm_.on_reject(x.taker_id);
                // A terminal (remainder) report must not delete trace state a
                // still-in-flight fill report needs: clean up only when no
                // fill report for this order remains open. Exchange-side
                // bookkeeping (reservation release, terminal state) never
                // depends on this delivery at all.
                maybe_cleanup_terminal_trace(x.taker_id);
            } else if (x.kind == 3) {
                if (x.taker_acct == cfg_.mm.account) mm_.on_cancel_confirmed(x.taker_id);
                maybe_cleanup_terminal_trace(x.taker_id);
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
    if (!s) {
        // Gateway acceptance is not exchange admission. A book-level reject
        // must be terminal and observable; it must not be journaled as an add.
        // Exchange-side cleanup happens HERE (risk reservation release, trace
        // state, terminal status), never on report delivery.
        risk_.release_reservation(o.id);
        drop_stage_trace(o.id);
        Event flt;
        flt.t = t;
        flt.type = EvType::Fault;
        flt.a = 4;  // fault kind: book admission rejected a risk-accepted order
        flt.b = static_cast<u64>(s.code);
        flt.c = o.id;
        flt.account = o.account;
        jappend(flt);
        if (gateway_known_ids_.count(o.id)) {
            Notify n;
            n.kind = 1;  // terminal exchange reject (reason rides the maker-id slot)
            n.fill.taker_order_id = o.id;
            n.fill.taker_account = o.account;
            n.fill.maker_order_id = static_cast<u64>(s.code);
            send_notify(std::move(n), t);
        }
        return;
    }
    jappend(ev_book_add(0, t, o));

    Qty taker_filled = 0;
    for (const Fill& f : fills) taker_filled += f.qty;
    const bool rests = o.type == OrderType::Limit && (o.tif == Tif::GTC || o.tif == Tif::GTD);
    // FOK: fills either happen fully or the entry was rejected
    const Qty taker_canceled =
        rests ? 0 : o.qty - taker_filled;

    // Exchange-authoritative updates happen at MATCH time, for every fill
    // leg, independently of exec-report delivery:
    //   * the authoritative ledger (position/cash/fees/filled qty) behind the
    //     reported research economics;
    //   * gateway risk position (lost/corrupted reports must never make the
    //     authoritative risk view stale);
    //   * the working-order exposure reservation (consumed by the fill).
    // Strategy inventory/cash remain notification-driven (the strategy's
    // local behavioral view may go stale under an intentionally faulty feed).
    const Side maker_side = is_buy(o.side) ? Side::Sell : Side::Buy;
    for (const Fill& f : fills) {
        ledger_fill(f.taker_account, o.side, f.price, f.qty);
        ledger_fill(f.maker_account, maker_side, f.price, f.qty);
        if (gateway_known_ids_.count(f.taker_order_id))
            risk_.note_fill(is_buy(o.side) ? static_cast<i64>(f.qty)
                                           : -static_cast<i64>(f.qty),
                            f.taker_account);
        if (gateway_known_ids_.count(f.maker_order_id))
            risk_.note_fill(is_buy(o.side) ? -static_cast<i64>(f.qty)
                                           : static_cast<i64>(f.qty),
                            f.maker_account);
        // both legs' reservations are consumed by this fill (no-ops for bot
        // legs, which never hold a reservation)
        risk_.reduce_reservation(f.taker_order_id, f.qty);
        risk_.reduce_reservation(f.maker_order_id, f.qty);
    }

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
            copy.maker_side = maker_side;
            Notify n;
            n.kind = 0;
            n.fill = copy;
            if (for_mm) ++stats_.strategy_fills;
            if (for_mom) ++stats_.momentum_fills;
            // A clean (will-deliver) report keeps the fill's trace state
            // alive; a dropped/corrupted one is resolved at publish time and
            // must never block terminal cleanup.
            if (send_notify(std::move(n), t)) {
                if (gateway_known_ids_.count(f.taker_order_id))
                    ++open_fill_reports_[f.taker_order_id];
                if (gateway_known_ids_.count(f.maker_order_id))
                    ++open_fill_reports_[f.maker_order_id];
            }
        } else {
            ++stats_.bot_fills;
        }
    }

    std::vector<Event> mds;
    for (const Fill& f : fills) {
        mds.push_back(ev_md_trade(0, t, f.price, f.qty));
        if (!book_.find(f.maker_order_id)) {  // maker fully consumed
            mds.push_back(ev_md_cancel(0, t, f.maker_order_id));
            // The maker reached an exchange-terminal state in THIS match:
            // release whatever reservation remains and run terminal trace
            // cleanup now (a lost fill report must not leak it).
            risk_.release_reservation(f.maker_order_id);
            maybe_cleanup_terminal_trace(f.maker_order_id);
        }
    }
    if (rests) {
        const Order* ro = book_.find(o.id);
        if (ro) mds.push_back(ev_md_new(0, t, o.id, o.side, o.price, ro->leaves));
        if (o.tif == Tif::GTD && o.expiry > t) schedule(o.expiry, RankGtd, EvGtdExpiry, o.id, 0, 0);
    }
    publish_md(mds, t);

    if (taker_canceled > 0) {
        // IOC/FOK remainder: the exchange releases the rest of the order's
        // risk reservation NOW (exchange time), and every gateway strategy
        // gets a terminal report (momentum takers included: without one,
        // their per-order trace state would survive until end of run).
        risk_.release_reservation(o.id);
        Notify n;
        n.kind = 2;  // IOC remainder: pending record removed
        n.fill.taker_order_id = o.id;
        n.fill.taker_account = o.account;
        n.fill.qty = taker_canceled;
        send_notify(std::move(n), t);
    }
    if (!rests) {
        // The order is exchange-terminal (fully filled, remainder canceled,
        // or FOK). When no fill report is still in flight, trace state dies
        // now; otherwise the pending fill reports clean up on delivery.
        maybe_cleanup_terminal_trace(o.id);
    }
}

void World::cancel_in_book(u64 order_id, LogicalNs t) {
    const Order* o = book_.find(order_id);
    if (!o) return;  // already gone (filled / unknown): nothing to publish
    const u32 owner = o->account;
    if (Status s = book_.cancel(order_id); s) {
        jappend(ev_book_cancel(0, t, order_id));
        // Exchange-time reservation release: a canceled order's working
        // exposure is freed whether or not the cancel confirmation report
        // reaches the strategy.
        risk_.release_reservation(order_id);
        std::vector<Event> mds;
        mds.push_back(ev_md_cancel(0, t, order_id));
        publish_md(mds, t);
        if (owner == cfg_.mm.account) {
            Notify n;
            n.kind = 3;
            n.fill.taker_order_id = order_id;
            n.fill.taker_account = owner;
            send_notify(std::move(n), t);
        }
        // Terminal at the exchange: clean up trace state unless fill reports
        // for this order are still in flight (they clean up on delivery).
        maybe_cleanup_terminal_trace(order_id);
    }
}

void World::publish_md(std::vector<Event>& mds, LogicalNs t) {
    for (const Event& e : mds) {
        const u64 seq = md_seq_++;
        md_ring_push(seq, e);
        jappend(e);
        ++stats_.md_published;
        const DeliveryPlan d = md_ch_.next_delivery();
        // One frame per message, created (and counted) BEFORE the loss
        // decision: wire_frames_encoded counts frames created before network
        // loss on every channel.
        std::string frame = wire_encode(1, seq, kFlagNone, e);
        ++stats_.wire_frames_encoded;
        if (d.drop) {
            ++stats_.md_dropped;
            continue;
        }
        if (d.corrupt) corrupt_frame(frame, corrupt_rng_);
        if (d.reorder_extra_ns) ++stats_.md_reordered;
        // The original delivery is its own packet attempt...
        md_packets_.emplace(md_packet_seq_++, MdPacket{seq, frame});
        schedule(add_time_checked(add_time_checked(t, d.delay_ns), d.reorder_extra_ns), RankMd,
                 EvMdDeliver, 0,
                 md_packet_seq_ - 1, 0);
        // ...and a duplicated delivery is a DISTINCT packet attempt: same
        // bytes and sequence, but its own scheduled event and delivery slot,
        // so it can never alias (or be delivered by) the original's event.
        if (d.duplicate) {
            md_packets_.emplace(md_packet_seq_++, MdPacket{seq, frame});
            schedule(add_time_checked(add_time_checked(t, d.dup_extra_ns), d.reorder_extra_ns),
                     RankMd, EvMdDeliver, 0,
                     md_packet_seq_ - 1, 0);
        }
    }
}

// ---------------- feed handler (strategy side) ----------------

void World::feed_handler_md(u64 packet_id) {
    auto pit = md_packets_.find(packet_id);
    if (pit == md_packets_.end()) return;  // already processed (cannot happen with 1:1 events)
    const MdPacket packet = std::move(pit->second);
    md_packets_.erase(pit);
    auto decoded = wire_decode(reinterpret_cast<const u8*>(packet.frame.data()), packet.frame.size());
    if (!decoded || decoded.value.channel != 1 || decoded.value.md_seq != packet.md_seq ||
        (decoded.value.flags & (kFlagSnapshot | kFlagExecReport)) != 0) {
        ++stats_.wire_frames_corrupted;
        Event flt;
        flt.t = now_;
        flt.type = EvType::Fault;
        flt.a = 1;  // market-data frame rejected at strategy feed
        flt.b = packet.frame.size();
        jappend(flt);
        if (packet.md_seq >= expected_md_seq_) maybe_schedule_gap_request();
        return;
    }
    const u64 md_seq = packet.md_seq;
    // Receiver dedupe by decoded transport sequence: an already-expected /
    // already-held message (duplicate attempt, racing retransmit, or
    // post-recovery replay) is dropped here, idempotently, and counted as a
    // SUPPRESSED duplicate delivery. `decoded` counts only attempts ACCEPTED
    // for processing — the same contract as the order and exec channels.
    if (md_seq < expected_md_seq_ || pending_md_.count(md_seq)) {
        ++stats_.md_duplicated;
        return;
    }
    ++stats_.wire_frames_decoded;
    pending_md_.emplace(md_seq, decoded.value.ev);
    if (md_seq > expected_md_seq_) {
        // arrived out of order: hold it in pending_md_ and
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
    schedule(add_time_checked(now_, 5'000), RankGap, EvGapRequestDue, 0, 0, 0);
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
        // every in-flight packet below the resynchronized sequence is dead
        // on arrival; retire it (its scheduled event will no-op on lookup)
        for (auto it = md_packets_.begin(); it != md_packets_.end();) {
            if (it->second.md_seq < expected_md_seq_)
                it = md_packets_.erase(it);
            else
                ++it;
        }
        ++stats_.md_snapshot_recoveries;
        ++stats_.md_recovered;
        deliver_expected();
        return;
    }
    for (u64 seq = expected_md_seq_; seq < md_seq_ && seq < oldest_buffered + kMdBufferCap; ++seq) {
        if (pending_md_.count(seq)) continue;  // already arrived client-side
        if (const Event* ev = md_ring_find(seq)) {
            // deterministic recovery path: a fresh valid RETRANSMIT packet
            // attempt at base delay, with no further loss/dup/corruption.
            // Its own packet id guarantees it can never alias the original
            // packet's delivery slot.
            md_packets_.emplace(md_packet_seq_++,
                                MdPacket{seq, wire_encode(1, seq, kFlagRetransmit, *ev)});
            ++stats_.wire_frames_encoded;
            schedule(add_time_checked(now_, cfg_.md_channel.base_delay_ns), RankMd,
                     EvMdDeliver, 0,
                     md_packet_seq_ - 1, 0);
            ++stats_.md_recovered;
        }
    }
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
        const i64 pnl = mm_.marked_pnl(mid);
        if (pnl > stats_.mm_marked_pnl_peak) stats_.mm_marked_pnl_peak = pnl;
        const i64 dd = saturating_sub_i64(stats_.mm_marked_pnl_peak, pnl);
        if (dd > stats_.mm_max_drawdown) stats_.mm_max_drawdown = dd;
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
        schedule(add_time_checked(t, 100'000), RankMarkout, EvMarkout, 0,
                 markouts_.size() - 1, 0);
        schedule(add_time_checked(t, 10'000'000), RankMarkout, EvMarkout, 1,
                 markouts_.size() - 1, 0);
    };
    if (f.taker_account == cfg_.mm.account) {
        mm_.on_fill(f.taker_order_id, f.taker_side, f.price, f.qty);
        note_latency(f.taker_order_id);
        note_drawdown();
        schedule_markouts(cfg_.mm.account, f.taker_side, f.price);
    } else if (f.maker_account == cfg_.mm.account) {
        mm_.on_fill(f.maker_order_id, f.maker_side, f.price, f.qty);
        note_latency(f.maker_order_id);
        note_drawdown();
        schedule_markouts(cfg_.mm.account, f.maker_side, f.price);
    }
    if (f.taker_account == cfg_.momentum.account) {
        mom_.on_fill(f.taker_order_id, f.taker_side, f.price, f.qty);
        note_latency(f.taker_order_id);
        schedule_markouts(cfg_.momentum.account, f.taker_side, f.price);
    } else if (f.maker_account == cfg_.momentum.account) {
        mom_.on_fill(f.maker_order_id, f.maker_side, f.price, f.qty);
        note_latency(f.maker_order_id);
        schedule_markouts(cfg_.momentum.account, f.maker_side, f.price);
    }
}

// ---------------- intent routing ----------------

void World::route_intent(const StrategyAction& act, LogicalNs t) {
    const DeliveryPlan d = order_ch_.next_delivery();
    // The frame is created (and counted) BEFORE the loss decision:
    // wire_frames_encoded counts frames created before network loss on every
    // channel. A dropped intent is counted and gone (documented v1
    // limitation: no client-side send timeout, the strategy never learns).
    Msg m;
    m.transport_seq = ++order_seq_;
    m.frame = encode_order_frame(act, cfg_.mm.account, m.transport_seq);
    // message-type granularity: cancels use their own decision latency when
    // configured (cancel_latency_ns >= 0), else the shared algo latency
    const bool is_cancel = act.kind == StrategyAction::Kind::Cancel;
    const LogicalNs decision_latency = cfg_.decision_latency_for(is_cancel);
    const LogicalNs depart_t = add_time_checked(t, decision_latency);
    if (d.drop) {
        ++stats_.order_frames_dropped;
        ++stats_.wire_frames_encoded;
        return;
    }
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
    schedule(add_time_checked(add_time_checked(add_time_checked(t, decision_latency), d.delay_ns),
                              d.reorder_extra_ns),
             RankGateway,
             EvGatewayArrival, cancel ? 1 : 0, idx, 0);
    // A duplicated delivery re-transmits the SAME frame (same transport
    // sequence); the gateway dedupes it by decoded sequence. The duplicate
    // is counted when the receiver SUPPRESSES it (order_frames_duplicated),
    // not here: this counter is receiver-side, like exec_frames_duplicated.
    if (d.duplicate)
        schedule(add_time_checked(add_time_checked(add_time_checked(t, decision_latency),
                                                   d.dup_extra_ns),
                                  d.reorder_extra_ns),
                 RankGateway,
                 EvGatewayArrival, cancel ? 1 : 0, idx, 0);
}

// ---------------- wire frame codecs (live path) ----------------

// Order-intent frame payload convention (58-byte canonical Event):
//   Send:   type=BookAdd, a=id, b=expiry, c=qty, p=price,
//           account/side/otype/tif as-is
//   Cancel: type=BookCancel, a=target id, account=owner
std::string World::encode_order_frame(const StrategyAction& act, u32 mm_account, u64 seq) {
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
    return wire_encode(2 /* order channel */, seq, kFlagNone, e);
}

Order World::decode_order_frame(const std::string& frame, bool& is_cancel, bool& ok,
                                u8& channel, u64& seq) {
    is_cancel = false;
    ok = false;
    channel = 0;
    seq = 0;
    auto d = wire_decode(reinterpret_cast<const u8*>(frame.data()), frame.size());
    if (!d || (d.value.flags & (kFlagSnapshot | kFlagExecReport)) != 0) return {};
    channel = d.value.channel;
    seq = d.value.md_seq;
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

std::string World::encode_notify_frame(const Notify& n, u64 seq) {
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
    return wire_encode_exec(3 /* exec channel */, seq, r);
}

// Exec-channel transmission: sequence assignment, loss/corruption/duplication
// plan, delivery scheduling. Returns true when a CLEAN report (one that will
// deliver and decode) was scheduled.
bool World::send_notify(Notify n, LogicalNs t) {
    n.transport_seq = ++exec_seq_;
    n.frame = encode_notify_frame(n, n.transport_seq);
    ++stats_.wire_frames_encoded;  // created before network loss
    const DeliveryPlan d = exec_ch_.next_delivery();
    if (d.drop) {
        ++stats_.exec_frames_dropped;
        return false;
    }
    if (d.corrupt) corrupt_frame(n.frame, corrupt_rng_);
    const size_t idx = notifies_.size();
    notifies_.push_back(std::move(n));
    schedule(add_time_checked(
                 add_time_checked(add_time_checked(t, cfg_.publish_latency_ns), d.delay_ns),
                 d.reorder_extra_ns),
             RankExec, EvExecNotify, 0, idx, 0);
    // A duplicated delivery re-transmits the SAME frame (same transport
    // sequence); the strategy feed dedupes it by decoded sequence. The
    // duplicate is counted when the receiver SUPPRESSES it
    // (exec_frames_duplicated), not here.
    if (d.duplicate)
        schedule(add_time_checked(add_time_checked(add_time_checked(t, cfg_.publish_latency_ns),
                                                   d.dup_extra_ns),
                                  d.reorder_extra_ns),
                 RankExec, EvExecNotify, 0, idx, 0);
    return !d.corrupt;
}

// ---------------- exchange-authoritative ledger ----------------

void World::ledger_fill(u32 account, Side ours, Price price, Qty qty) {
    if (account != cfg_.mm.account && account != cfg_.momentum.account) return;
    LedgerEntry& e = ledger_[account];
    const i64 signed_qty = signed_qty_i64(ours, qty);
    e.position = saturating_add_i64(e.position, signed_qty);
    e.cash = saturating_sub_i64(e.cash, saturating_mul_i64(signed_qty, price));
    const Qty bounded_qty = std::min<Qty>(qty, static_cast<Qty>(std::numeric_limits<i64>::max()));
    const i64 fee = saturating_mul_i64(cfg_.fee_per_lot, static_cast<i64>(bounded_qty));
    e.cash = saturating_sub_i64(e.cash, fee);
    e.fees = saturating_add_i64(e.fees, fee);
    e.filled_qty = saturating_add_u64(e.filled_qty, qty);
}

void World::resolve_open_fill_reports(const Fill& f) {
    for (const u64 id : {f.taker_order_id, f.maker_order_id}) {
        auto it = open_fill_reports_.find(id);
        if (it == open_fill_reports_.end()) continue;
        if (--it->second == 0) open_fill_reports_.erase(it);
    }
}

void World::maybe_cleanup_terminal_trace(u64 order_id) {
    auto it = open_fill_reports_.find(order_id);
    if (it != open_fill_reports_.end() && it->second > 0) return;  // fills still in flight
    if (it != open_fill_reports_.end()) open_fill_reports_.erase(it);
    drop_stage_trace(order_id);
}

void World::corrupt_frame(std::string& frame, Rng& rng) {
    if (frame.size() <= kFrameHeaderSize) return;
    const u64 pos = kFrameHeaderSize + rng.bounded(frame.size() - kFrameHeaderSize);
    frame[static_cast<size_t>(pos)] = static_cast<char>(frame[static_cast<size_t>(pos)] ^ 0x5A);
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


} // namespace arbiter
