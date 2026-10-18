// Market-data ingestion: CSV parse, validation, normalization, hashing.
#include "arbiter/data.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace arbiter {
namespace data {

const char* to_cstr(MarketEventType t) noexcept {
    switch (t) {
        case MarketEventType::Trade: return "trade";
        case MarketEventType::QuoteL1: return "quote_l1";
        case MarketEventType::DepthL2: return "depth_l2";
        case MarketEventType::Add: return "add";
        case MarketEventType::Cancel: return "cancel";
        case MarketEventType::Execution: return "execution";
    }
    return "?";
}

const char* to_cstr(DataLevel l) noexcept {
    switch (l) {
        case DataLevel::L1: return "L1";
        case DataLevel::L2: return "L2";
        case DataLevel::L3: return "L3";
    }
    return "?";
}

std::string DataQualityReport::summary() const {
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "events=%zu trades=%zu quotes=%zu depth=%zu adds=%zu cancels=%zu execs=%zu "
                  "issues{nonmono_ts=%zu dup_seq=%zu seq_gaps=%zu bad_price=%zu zero_qty=%zu "
                  "unknown_type=%zu bad_side=%zu bad_order_id=%zu bad_seq=%zu bad_recv_ts=%zu "
                  "empty=%zu} range=[%lld,%lld] verdict=%s",
                  events, trades, quotes, depth_updates, adds, cancels, executions,
                  non_monotonic_ts, duplicate_seq, seq_gaps, zero_or_negative_price, zero_qty,
                  unknown_type_rows, bad_side_rows, bad_order_id_rows, bad_seq_rows,
                  bad_recv_ts_rows, empty_or_missing_rows,
                  (long long)first_ts_ns, (long long)last_ts_ns, ok() ? "OK" : "ISSUES");
    return std::string(buf);
}

u64 dataset_hash(const std::vector<MarketEvent>& events,
                 const std::vector<NormalizationAction>& log) {
    Hasher h;
    h.tag("ARBITERX/DATASET/1");
    h.str(kDataSchemaVersion);
    h.u64v(events.size());
    for (const auto& e : events) {
        h.i64v(e.exchange_ts_ns);
        h.i64v(e.receive_ts_ns);
        h.str(e.venue);
        h.str(e.instrument);
        h.u8v(static_cast<u8>(e.type));
        h.u8v(static_cast<u8>(e.level));
        h.u64v(e.order_id);
        h.u8v(static_cast<u8>(e.side));
        h.i64v(e.price);
        h.u64v(e.qty);
        h.u64v(e.seq);
        h.str(e.session);
    }
    h.u64v(log.size());
    for (const auto& a : log) {
        h.u64v(a.input_row);
        h.str(a.action);
        h.str(a.detail);
    }
    return h.value();
}

namespace {

bool parse_type(const std::string& s, MarketEventType& out) {
    if (s == "trade") out = MarketEventType::Trade;
    else if (s == "quote_l1" || s == "quote") out = MarketEventType::QuoteL1;
    else if (s == "depth_l2" || s == "depth") out = MarketEventType::DepthL2;
    else if (s == "add") out = MarketEventType::Add;
    else if (s == "cancel") out = MarketEventType::Cancel;
    else if (s == "execution" || s == "exec") out = MarketEventType::Execution;
    else return false;
    return true;
}

bool parse_side(const std::string& s, Side& out) {
    if (s == "buy" || s == "bid" || s == "BUY" || s == "BID") out = Side::Buy;
    else if (s == "sell" || s == "ask" || s == "SELL" || s == "ASK") out = Side::Sell;
    else return false;
    return true;
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> cols;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            cols.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    cols.push_back(cur);
    return cols;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
                     s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

long long to_ll(const std::string& s, bool& ok) {
    if (s.empty()) {
        ok = false;
        return 0;
    }
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    ok = (end && *end == '\0');
    return v;
}

}  // namespace

Result<std::vector<MarketEvent>> parse_csv(const std::string& path, DataQualityReport& report) {
    report = DataQualityReport{};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return ErrT<std::vector<MarketEvent>>(Errc::NotFound, 0);
    char line[4096];
    // header
    if (!std::fgets(line, sizeof line, f)) {
        std::fclose(f);
        return ErrT<std::vector<MarketEvent>>(Errc::InvalidArgument, 0);
    }
    std::string header(line);
    auto hcols = split_csv(header);
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < hcols.size(); ++i) idx[trim(hcols[i])] = i;
    const char* required[] = {"exchange_ts_ns", "venue",    "instrument", "type",
                              "order_id",       "side",     "price",      "qty",
                              "seq"};
    for (const char* r : required) {
        if (!idx.count(r)) {
            std::fclose(f);
            return ErrT<std::vector<MarketEvent>>(Errc::InvalidArgument, 1);
        }
    }
    const bool has_recv = idx.count("receive_ts_ns") > 0;
    const bool has_session = idx.count("session") > 0;
    const bool has_level = idx.count("level") > 0;
    std::vector<MarketEvent> out;
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        if (s.empty() || s == "\n") {
            ++report.empty_or_missing_rows;
            continue;
        }
        auto cols = split_csv(s);
        auto get = [&](const std::string& k) -> std::string {
            auto it = idx.find(k);
            if (it == idx.end() || it->second >= cols.size()) return "";
            return trim(cols[it->second]);
        };
        bool ok = true;
        bool ok2 = true;
        i64 ts = static_cast<i64>(to_ll(get("exchange_ts_ns"), ok));
        MarketEvent e;
        if (!ok) {
            ++report.empty_or_missing_rows;
            continue;
        }
        e.exchange_ts_ns = ts;
        e.venue = get("venue");
        e.instrument = get("instrument");
        if (!parse_type(get("type"), e.type)) {
            ++report.unknown_type_rows;
            continue;
        }
        // order_id: unparsable values are discarded rows; 0 is LEGAL (L1
        // quotes/trades carry no order identity)
        e.order_id = static_cast<u64>(to_ll(get("order_id"), ok2));
        if (!ok2) {
            ++report.bad_order_id_rows;
            continue;
        }
        if (!parse_side(get("side"), e.side)) {
            ++report.bad_side_rows;
            continue;
        }
        e.price = static_cast<Price>(to_ll(get("price"), ok));
        if (!ok || e.price <= 0) {
            ++report.zero_or_negative_price;
            continue;
        }
        e.qty = static_cast<Qty>(to_ll(get("qty"), ok));
        if (!ok || e.qty == 0) {
            ++report.zero_qty;
            continue;
        }
        e.seq = static_cast<u64>(to_ll(get("seq"), ok2));
        if (!ok2) {
            ++report.bad_seq_rows;
            continue;
        }
        if (has_recv) {
            const std::string r = get("receive_ts_ns");
            if (r.empty()) {
                e.receive_ts_ns = ts;
            } else {
                e.receive_ts_ns = static_cast<i64>(to_ll(r, ok2));
                if (!ok2) {
                    ++report.bad_recv_ts_rows;
                    continue;
                }
            }
        } else {
            e.receive_ts_ns = ts;
        }
        e.session = has_session ? get("session") : "";
        if (has_level) {
            const std::string l = get("level");
            if (l == "L3") e.level = DataLevel::L3;
            else if (l == "L2") e.level = DataLevel::L2;
            else e.level = DataLevel::L1;
        } else {
            // honest defaults: only explicit order-id streams are L3
            if (e.type == MarketEventType::Add || e.type == MarketEventType::Cancel ||
                e.type == MarketEventType::Execution)
                e.level = e.order_id ? DataLevel::L3 : DataLevel::L2;
            else if (e.type == MarketEventType::DepthL2)
                e.level = DataLevel::L2;
            else
                e.level = DataLevel::L1;
        }
        out.push_back(std::move(e));
    }
    std::fclose(f);
    validate(out, report);
    return OkT<std::vector<MarketEvent>>(std::move(out));
}

void validate(const std::vector<MarketEvent>& events, DataQualityReport& report) {
    // Two counter OWNERSHIPS, kept strictly separate:
    //   * row-level counters (bad_* / empty_or_missing_rows / unknown_type /
    //     bad_side) are owned by parse_csv, validate preserves them, so a
    //     new parse counter is automatically kept (no wipe-and-restore list
    //     to maintain in two places).
    //   * stream-level stats (type counts, monotonicity, duplicate/gapped
    //     seqs, ts range, zero price/qty among KEPT events) are recomputed
    //     here from the surviving event set.
    const size_t bad_order_id = report.bad_order_id_rows, bad_seq = report.bad_seq_rows,
                 bad_recv = report.bad_recv_ts_rows, bad_side = report.bad_side_rows,
                 unknown_type = report.unknown_type_rows, empty = report.empty_or_missing_rows;
    report = DataQualityReport{};
    report.bad_order_id_rows = bad_order_id;
    report.bad_seq_rows = bad_seq;
    report.bad_recv_ts_rows = bad_recv;
    report.bad_side_rows = bad_side;
    report.unknown_type_rows = unknown_type;
    report.empty_or_missing_rows = empty;
    // zero_price/zero_qty are recomputed below over the KEPT events (parse
    // counts them only for rows it discards)
    report.events = events.size();
    std::set<u64> seqs;
    bool have_seq = false;
    u64 prev_seq = 0;
    i64 prev_ts = 0;
    bool first = true;
    for (const auto& e : events) {
        switch (e.type) {
            case MarketEventType::Trade: ++report.trades; break;
            case MarketEventType::QuoteL1: ++report.quotes; break;
            case MarketEventType::DepthL2: ++report.depth_updates; break;
            case MarketEventType::Add: ++report.adds; break;
            case MarketEventType::Cancel: ++report.cancels; break;
            case MarketEventType::Execution: ++report.executions; break;
        }
        if (first) {
            report.first_ts_ns = e.exchange_ts_ns;
            first = false;
        }
        report.last_ts_ns = e.exchange_ts_ns;
        if (e.exchange_ts_ns < prev_ts) ++report.non_monotonic_ts;
        prev_ts = e.exchange_ts_ns;
        if (e.price <= 0) ++report.zero_or_negative_price;
        if (e.qty == 0) ++report.zero_qty;
        if (e.seq != 0) {
            if (!seqs.insert(e.seq).second) ++report.duplicate_seq;
            if (have_seq && e.seq > prev_seq + 1) ++report.seq_gaps;
            if (!have_seq || e.seq > prev_seq) prev_seq = e.seq;
            have_seq = true;
        }
    }
}

std::vector<MarketEvent> normalize(std::vector<MarketEvent> events,
                                   std::vector<NormalizationAction>& log) {
    log.clear();
    // 1. stable sort by (exchange_ts_ns, seq)
    std::stable_sort(events.begin(), events.end(), [](const MarketEvent& a, const MarketEvent& b) {
        if (a.exchange_ts_ns != b.exchange_ts_ns) return a.exchange_ts_ns < b.exchange_ts_ns;
        return a.seq < b.seq;
    });
    log.push_back({0, "stable_sort", "ordered by (exchange_ts_ns, seq)"});
    // 2. fill missing receive timestamps (receive < exchange is impossible)
    size_t filled = 0, clamped = 0;
    for (auto& e : events) {
        if (e.receive_ts_ns == 0) {
            e.receive_ts_ns = e.exchange_ts_ns;
            ++filled;
        } else if (e.receive_ts_ns < e.exchange_ts_ns) {
            e.receive_ts_ns = e.exchange_ts_ns;
            ++clamped;
        }
    }
    if (filled) log.push_back({0, "fill_missing_receive_ts", std::to_string(filled) + " rows"});
    if (clamped)
        log.push_back({0, "clamp_receive_ts", std::to_string(clamped) + " rows had receive<exchange"});
    return events;
}

Status export_csv(const std::string& path, const std::vector<MarketEvent>& events) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return Status{Errc::NotFound, 0};
    std::fprintf(f,
                 "# schema=%s columns=exchange_ts_ns,venue,instrument,type,order_id,side,"
                 "price,qty,seq,receive_ts_ns,session,level\n",
                 kDataSchemaVersion);
    std::fprintf(f,
                 "exchange_ts_ns,venue,instrument,type,order_id,side,price,qty,seq,"
                 "receive_ts_ns,session,level\n");
    for (const auto& e : events) {
        std::fprintf(f, "%lld,%s,%s,%s,%llu,%s,%lld,%llu,%llu,%lld,%s,%s\n",
                     (long long)e.exchange_ts_ns, e.venue.c_str(), e.instrument.c_str(),
                     to_cstr(e.type), (unsigned long long)e.order_id, to_cstr(e.side),
                     (long long)e.price, (unsigned long long)e.qty, (unsigned long long)e.seq,
                     (long long)e.receive_ts_ns, e.session.c_str(), to_cstr(e.level));
    }
    std::fclose(f);
    return Status{Errc::Ok, 0};
}

ChronoSplit chrono_split(size_t n, double train_frac, double valid_frac, double held_out_frac) {
    ChronoSplit s;
    if (n == 0) return s;
    s.train_end = static_cast<size_t>(n * train_frac);
    s.valid_end = s.train_end + static_cast<size_t>(n * valid_frac);
    if (s.valid_end > n) s.valid_end = n;
    const size_t held = static_cast<size_t>(n * held_out_frac);
    s.held_out_begin = held >= n ? 0 : n - held;
    if (s.held_out_begin < s.valid_end) s.held_out_begin = s.valid_end;
    return s;
}

}  // namespace data
}  // namespace arbiter