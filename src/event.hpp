#pragma once

// Event stream schema shared by the generator, the replayer, the benchmark and
// (later) the FPGA parser. One event per CSV line, fixed columns:
//
//   ts_ns,type,id,side,price,qty
//
//   type  A = limit add      id = new order id, side B/S, price ticks, qty
//         M = market order   id = new order id, side B/S, price = 0,   qty
//         C = cancel         id = target id,    side '-',  price = 0,   qty = 0
//         X = modify         id = target id,    side '-',  price = 0,   qty = new qty
//
// Prices are int64 ticks. Timestamps are nanoseconds from the start of the stream.

#include <optional>
#include <string>
#include <string_view>

#include "order.hpp"

namespace ob {

inline constexpr std::string_view kCsvHeader = "ts_ns,type,id,side,price,qty";

enum class EventType : char { Add = 'A', Market = 'M', Cancel = 'C', Modify = 'X' };

// Separate from ob::Side so the matching engine's enum never has a "none" value.
enum class EventSide : char { Buy = 'B', Sell = 'S', None = '-' };

struct Event {
    Timestamp ts{};
    EventType type{EventType::Add};
    OrderId   id{};
    EventSide side{EventSide::None};
    Price     price{};
    Quantity  qty{};

    bool operator==(const Event&) const = default;
};

// Only valid for EventSide::Buy / Sell.
inline Side to_book_side(EventSide s) { return s == EventSide::Buy ? Side::Buy : Side::Sell; }

std::string to_csv_line(const Event& e);

// Parses one CSV data line (no trailing newline). Returns nullopt on any
// malformed input, including the header line.
std::optional<Event> parse_csv_line(std::string_view line);

}  // namespace ob
