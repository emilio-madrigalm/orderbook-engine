#pragma once

// Nasdaq TotalView-ITCH 5.0 -> our event stream (event.hpp), for one symbol.
//
// ITCH is the exchange's own binary feed: every displayed order's add, execution,
// cancel, delete and replace, each with the order's reference number. A full-day
// file starts before the market opens, so the book starts empty and every order's
// add is in the stream.
//
// Framing: each message is a 2-byte big-endian length, then that many bytes; the
// first byte is the message type. All integers are big-endian. Prices are $ x 10000.
//
// Conversion (only messages whose stock locate matches the symbol's 'R' entry):
//   A, F  add order          -> A (limit add, same id, side, price in cents)
//   E     order executed     -> M on the opposite side for the executed qty, and an
//                               expected fill (market id, resting ref, price, qty).
//                               The C++ book picks which order fills: that is what
//                               the check measures.
//   C     executed at a different price (crosses) -> X/C reduction of that order;
//                               our book has no auction, so this is applied, not matched
//   X     partial cancel     -> X (modify to the remaining qty) or C if none left
//   D     delete             -> C
//   U     replace            -> C of the old ref, A of the new ref (loses priority,
//                               as on Nasdaq)
//   P, Q, B  hidden, cross and broken trades: no visible order changes; counted only
//
// Adds whose price is not a whole cent or is outside [1, max_price] are skipped, and
// so is every later message for that order.
//
// Alongside the events, the converter keeps its own book built straight from the
// messages (each execution reduces the order Nasdaq named) and writes it in the
// same format as ob_replay's snapshots. If the C++ book matches the way Nasdaq did,
// the two snapshot files are identical.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "event.hpp"
#include "order.hpp"

namespace ob::itch {

// Ids for the market orders that stand in for executions. ITCH refs are far below.
inline constexpr OrderId kMarketIdBase = OrderId{1} << 62;

// Message length (type byte included) for the types we read or count; 0 = unknown.
std::size_t message_length(char type);

struct Stats {
    std::uint64_t messages{};          // every message in the stream
    std::uint64_t symbol_messages{};   // messages for the chosen symbol
    std::uint64_t adds{};
    std::uint64_t executions{};        // E
    std::uint64_t executions_price{};  // C
    std::uint64_t cancels{};           // X
    std::uint64_t deletes{};           // D
    std::uint64_t replaces{};          // U
    std::uint64_t hidden_trades{};     // P
    std::uint64_t cross_trades{};      // Q
    std::uint64_t broken_trades{};     // B
    std::uint64_t skipped_price{};     // adds skipped: sub-cent or outside the band
    std::uint64_t skipped_followups{}; // later messages for a skipped order
    std::uint64_t unknown_refs{};      // execute/cancel/delete/replace of a ref never added
    std::uint64_t overfills{};         // executed or cancelled more than was resting
    std::uint64_t events{};            // events written
};

struct Options {
    std::string symbol;               // e.g. "AAPL"
    std::size_t depth{5};             // levels per side in the depth file
    std::size_t snapshot_every{1};    // 0 = no depth file rows
    Price       max_price{131071};    // OrderBook's default band
};

class Converter {
public:
    // fills receives "market_id,resting_id,price,qty,match" rows (header written here).
    // depth may be null to skip the reference depth file.
    Converter(Options options, std::ostream& events, std::ostream& fills, std::ostream* depth);

    // One message without its length prefix. Returns false if the length is wrong
    // for its type (the stream is then not ITCH 5.0, or is corrupt).
    bool on_message(const std::uint8_t* msg, std::size_t len);

    const Stats& stats() const { return stats_; }
    bool         symbol_found() const { return found_; }
    std::uint16_t locate() const { return locate_; }

private:
    struct Resting {
        char     side;  // 'B' or 'S'
        Price    price;
        Quantity qty;
    };

    void emit(Timestamp ts, EventType type, OrderId id, EventSide side, Price price, Quantity qty);
    void write_depth(Timestamp ts);
    // Removes up to `by` shares from the order and its level; returns what is left.
    Quantity reduce(std::unordered_map<OrderId, Resting>::iterator it, Quantity by);
    // Finds a live order, counting a miss as skipped or unknown. Null if none.
    std::unordered_map<OrderId, Resting>::iterator find(OrderId ref, bool& ok);
    bool add(Timestamp ts, OrderId ref, char side, std::uint32_t raw_price, Quantity qty);

    Options       opt_;
    std::ostream& events_;
    std::ostream& fills_;
    std::ostream* depth_;
    Stats         stats_;
    bool          found_{false};
    std::uint16_t locate_{0};
    OrderId       next_market_{kMarketIdBase};

    std::unordered_map<OrderId, Resting>          orders_;
    std::unordered_set<OrderId>                   skipped_;
    std::map<Price, Quantity, std::greater<Price>> bids_;
    std::map<Price, Quantity>                     asks_;
};

// Reads length-prefixed messages until EOF. Returns false (with *error set) on a
// truncated stream or a message Converter::on_message rejects.
bool read_stream(std::istream& in, Converter& conv, std::string* error);

}  // namespace ob::itch
