#pragma once

#include <cstdint>

namespace ob {

using OrderId   = std::uint64_t;
using Price     = std::int64_t;   // integer ticks, never floating point
using Quantity  = std::int64_t;
using Timestamp = std::uint64_t;  // nanoseconds, or a sequence number

enum class Side : std::uint8_t { Buy, Sell };

struct Order {
    OrderId   id{};
    Side      side{Side::Buy};
    Price     price{};      // ignored for market orders
    Quantity  qty{};
    Timestamp timestamp{};
};

struct Trade {
    OrderId  buy_id{};
    OrderId  sell_id{};
    Price    price{};
    Quantity qty{};
};

}  // namespace ob
