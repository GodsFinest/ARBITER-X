#pragma once
// Real market-data ingestion layer (deterministic research input).
//
// SCOPE: parse + validate + normalize public historical market data into the
// canonical MarketEvent stream. The C++ core stays dependency-free (CSV in,
// columnar Parquet-like export is plain CSV with a versioned schema for the
// Python layer). Licensing: only load data you are entitled to use; every
// artifact records dataset_hash + source + session so results are traceable.
//
// RULES (see docs/DATA.md):
//   * exchange timestamp, receive timestamp, sequence, source, symbol and
//     session are preserved end-to-end;
//   * NOTHING is silently repaired, every normalization action is appended
//     to NormalizationLog and hashed into the dataset identity;
//   * L2 data is never promoted to L3: without order ids the level is L2.
//     (DataLevel is explicit on every event.)
//   * as-of correctness: consumers must only use events with
//     exchange_ts_ns <= as_of. There is no lookahead in this layer.

#include "arbiter/errors.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace arbiter {
namespace data {

// Canonical data schema version (bump on any column/semantic change).
inline constexpr const char* kDataSchemaVersion = "mktdata_v1";

enum class MarketEventType : u8 { Trade = 0, QuoteL1 = 1, DepthL2 = 2, Add = 3, Cancel = 4, Execution = 5 };
enum class DataLevel : u8 { L1 = 0, L2 = 1, L3 = 2 };

const char* to_cstr(MarketEventType t) noexcept;
const char* to_cstr(DataLevel l) noexcept;

// One normalized market event. Integer timestamps (ns) and ticks/lots only,
// no floats in this layer.
struct MarketEvent {
    i64 exchange_ts_ns = 0;   // exchange/event timestamp (authoritative order)
    i64 receive_ts_ns = 0;    // local/receive timestamp if available, else == exchange_ts_ns
    std::string venue;        // e.g. "SYNTH", "BINANCE", "COINBASE"
    std::string instrument;   // e.g. "BTC-USDT"
    MarketEventType type = MarketEventType::Trade;
    DataLevel level = DataLevel::L1;
    u64 order_id = 0;         // L3 only; 0 = unknown/not-applicable
    Side side = Side::Buy;    // aggressor side for trades, quote side for adds/cancels
    Price price = 0;          // ticks (>0 for price-bearing events)
    Qty qty = 0;              // lots
    u64 seq = 0;              // source sequence number (0 = unavailable)
    std::string session;      // date/session tag, e.g. "2026-01-05"
};

struct NormalizationAction {
    size_t input_row = 0;     // 1-based source line (0 = synthetic/derived)
    std::string action;       // e.g. "fill_missing_receive_ts", "drop_zero_qty"
    std::string detail;
};

struct DataQualityReport {
    size_t events = 0;
    size_t trades = 0, quotes = 0, depth_updates = 0, adds = 0, cancels = 0, executions = 0;
    // issue counters (every discarded/flagged row is counted)
    size_t non_monotonic_ts = 0;
    size_t duplicate_seq = 0;
    size_t seq_gaps = 0;
    size_t zero_or_negative_price = 0;
    size_t zero_qty = 0;
    size_t unknown_type_rows = 0;
    size_t bad_side_rows = 0;
    size_t bad_order_id_rows = 0;   // unparsable/zero order_id (row discarded)
    size_t bad_seq_rows = 0;        // unparsable seq (row discarded)
    size_t bad_recv_ts_rows = 0;    // unparsable receive_ts_ns (row discarded)
    size_t empty_or_missing_rows = 0;
    i64 first_ts_ns = 0, last_ts_ns = 0;
    bool ok() const {
        return non_monotonic_ts == 0 && duplicate_seq == 0 && zero_or_negative_price == 0 &&
               zero_qty == 0 && unknown_type_rows == 0 && bad_side_rows == 0 &&
               bad_order_id_rows == 0 && bad_seq_rows == 0 && bad_recv_ts_rows == 0;
    }
    std::string summary() const;
};

// Deterministic identity of a dataset: FNV-1a over (schema version, ordered
// event fields, normalization log). Equal bytes in -> equal hash.
u64 dataset_hash(const std::vector<MarketEvent>& events,
                 const std::vector<NormalizationAction>& log);

// CSV format (header required):
//   exchange_ts_ns,venue,instrument,type,order_id,side,price,qty,seq[,receive_ts_ns,session,level]
// type in {trade,quote_l1,depth_l2,add,cancel,execution}; side in {buy,sell,bid,ask};
// level in {L1,L2,L3} (default L1 for quote_l1/trade, L2 for depth_l2, L3 for add/cancel/execution).
// Malformed rows are SKIPPED and counted in the report (no exceptions, no silent repair).
Result<std::vector<MarketEvent>> parse_csv(const std::string& path, DataQualityReport& report);

// Validate an event stream: monotonicity, duplicate seq, gaps, price/qty
// domain, side/type consistency. Pure (no mutation); fills `report` in place.
void validate(const std::vector<MarketEvent>& events, DataQualityReport& report);

// Normalize: sort by (exchange_ts_ns, seq) stably, fill receive_ts_ns when
// missing, drop zero-qty rows. Every action is logged; the log participates
// in dataset_hash. Returns the normalized stream.
std::vector<MarketEvent> normalize(std::vector<MarketEvent> events,
                                   std::vector<NormalizationAction>& log);

// Export the stable research schema for the Python layer (CSV with schema
// header `# schema=mktdata_v1 ...`). Deterministic row order = input order.
Status export_csv(const std::string& path, const std::vector<MarketEvent>& events);

// Split chronologically into [train][validation][test] by FRACTION of events
// (fractions must sum to <= 1; remainder is the HELD-OUT tail). Indices, not
// copies. Never shuffles (see docs/RESEARCH.md).
struct ChronoSplit {
    size_t train_end = 0;   // [0, train_end)
    size_t valid_end = 0;   // [train_end, valid_end)
    // [valid_end, n) = test; the final `held_out` fraction of test is sealed.
    size_t held_out_begin = 0;  // [held_out_begin, n) must stay untouched until freeze
};
ChronoSplit chrono_split(size_t n, double train_frac = 0.6, double valid_frac = 0.2,
                         double held_out_frac = 0.1);

}  // namespace data
}  // namespace arbiter