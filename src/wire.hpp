#pragma once

// Fixed 32-byte binary message for the event stream (the format the FPGA parser
// in fpga/rtl/msg_parser.sv reads, one message per 256-bit bus word).
//
//   offset size field     rule
//        0    1 type      'A' 'M' 'C' 'X'
//        1    1 side      'B'/'S' for A and M, '-' for C and X
//        2    2 reserved  must be 0
//        4    4 qty       > 0 for A, M, X; 0 for C
//        8    8 ts_ns
//       16    8 id
//       24    4 price     ticks, 1..131071 for A; 0 for M, C, X
//       28    4 reserved  must be 0
//
// All integers little-endian. Byte i of the message is bits [8i+7:8i] of the bus
// word. decode() is the reference model for the hardware: it reports the FIRST
// failed rule in the order type, side, price, qty, reserved, and the hardware must
// report the same code and the same fields for every message.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "event.hpp"

namespace ob {

inline constexpr std::size_t kWireSize     = 32;
inline constexpr Price       kWireMaxPrice = 131071;  // OrderBook's default band

using WireMsg = std::array<std::uint8_t, kWireSize>;

enum class WireError : std::uint8_t {
    None     = 0,
    BadType  = 1,
    BadSide  = 2,
    BadPrice = 3,
    BadQty   = 4,
    Reserved = 5,
};

// nullopt if a field does not fit its wire width (qty or price outside u32,
// negative values). It does not check the rules: invalid events can be encoded,
// which is how the error paths are tested.
std::optional<WireMsg> encode(const Event& e);

struct Decoded {
    WireError     err{WireError::None};
    std::uint64_t ts{};
    std::uint8_t  type{};  // raw byte
    std::uint64_t id{};
    std::uint8_t  side{};  // raw byte
    std::uint32_t price{};
    std::uint32_t qty{};
    bool operator==(const Decoded&) const = default;
};

Decoded decode(const WireMsg& m);

// Hand-written boundary messages: every error code, every type, price 0, 1,
// max and max + 1, each reserved byte, and messages that break several rules at
// once. ob_wire encode --edge-cases puts them in front of the generated stream,
// because random traffic and random corruption almost never hit exact bounds.
std::vector<WireMsg> edge_cases();

// "err,ts,type,id,side,price,qty" with type and side as decimal byte values:
// the line format the testbench writes too, so the two can be diffed.
std::string decoded_line(const Decoded& d);

// 64 hex digits, byte 31 first: one line per message, $readmemh order.
std::string to_hex(const WireMsg& m);
std::optional<WireMsg> from_hex(std::string_view s);

}  // namespace ob
