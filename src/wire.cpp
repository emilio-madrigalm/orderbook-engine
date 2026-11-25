#include "wire.hpp"

#include <limits>

namespace ob {

namespace {

template <class T>
void put_le(WireMsg& m, std::size_t off, T v) {
    for (std::size_t i = 0; i < sizeof(T); ++i) m[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

template <class T>
T get_le(const WireMsg& m, std::size_t off) {
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(m[off + i]) << (8 * i));
    return v;
}

constexpr bool fits_u32(std::int64_t v) {
    return v >= 0 && v <= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::optional<WireMsg> encode(const Event& e) {
    if (!fits_u32(e.qty) || !fits_u32(e.price)) return std::nullopt;
    WireMsg m{};
    m[0] = static_cast<std::uint8_t>(e.type);
    m[1] = static_cast<std::uint8_t>(e.side);
    put_le<std::uint32_t>(m, 4, static_cast<std::uint32_t>(e.qty));
    put_le<std::uint64_t>(m, 8, e.ts);
    put_le<std::uint64_t>(m, 16, e.id);
    put_le<std::uint32_t>(m, 24, static_cast<std::uint32_t>(e.price));
    return m;
}

Decoded decode(const WireMsg& m) {
    Decoded d;
    d.type  = m[0];
    d.side  = m[1];
    d.qty   = get_le<std::uint32_t>(m, 4);
    d.ts    = get_le<std::uint64_t>(m, 8);
    d.id    = get_le<std::uint64_t>(m, 16);
    d.price = get_le<std::uint32_t>(m, 24);

    const bool is_add    = d.type == 'A';
    const bool is_market = d.type == 'M';
    const bool is_cancel = d.type == 'C';
    const bool is_modify = d.type == 'X';
    const bool sided     = is_add || is_market;

    const bool type_ok  = is_add || is_market || is_cancel || is_modify;
    const bool side_ok  = sided ? (d.side == 'B' || d.side == 'S') : d.side == '-';
    const bool price_ok = is_add ? (d.price >= 1 && d.price <= kWireMaxPrice) : d.price == 0;
    const bool qty_ok   = is_cancel ? d.qty == 0 : d.qty > 0;
    const bool rsv_ok   = m[2] == 0 && m[3] == 0 && get_le<std::uint32_t>(m, 28) == 0;

    if (!type_ok)       d.err = WireError::BadType;
    else if (!side_ok)  d.err = WireError::BadSide;
    else if (!price_ok) d.err = WireError::BadPrice;
    else if (!qty_ok)   d.err = WireError::BadQty;
    else if (!rsv_ok)   d.err = WireError::Reserved;
    return d;
}

std::vector<WireMsg> edge_cases() {
    constexpr std::uint32_t kMax = static_cast<std::uint32_t>(kWireMaxPrice);
    constexpr std::uint32_t kAll = std::numeric_limits<std::uint32_t>::max();
    std::vector<WireMsg> out;
    std::uint64_t id = 0xE000000000000000ULL;
    // Raw bytes, so values the Event type cannot express (bad type/side) fit too.
    auto add = [&](std::uint8_t type, std::uint8_t side, std::uint32_t price, std::uint32_t qty) -> WireMsg& {
        WireMsg m{};
        m[0] = type;
        m[1] = side;
        put_le<std::uint32_t>(m, 4, qty);
        put_le<std::uint64_t>(m, 8, 1000 + out.size());
        put_le<std::uint64_t>(m, 16, id++);
        put_le<std::uint32_t>(m, 24, price);
        out.push_back(m);
        return out.back();
    };
    // valid
    add('A', 'B', 1, 1);
    add('A', 'S', kMax, 1);
    add('A', 'B', kMax - 1, kAll);
    add('M', 'B', 0, 1);
    add('M', 'S', 0, kAll);
    add('C', '-', 0, 0);
    add('X', '-', 0, 1);
    add('X', '-', 0, kAll);
    // type
    add('a', 'B', 1, 1);
    add(0x00, '-', 0, 0);
    add(0xFF, 0xFF, kAll, kAll);
    // side
    add('A', '-', 5, 1);
    add('A', 'b', 5, 1);
    add('M', '-', 0, 1);
    add('C', 'B', 0, 0);
    add('X', 'S', 0, 1);
    // price
    add('A', 'B', 0, 1);
    add('A', 'S', kMax + 1, 1);
    add('A', 'B', kAll, 1);
    add('M', 'S', 1, 1);
    add('C', '-', 1, 0);
    add('X', '-', kMax, 1);
    // qty
    add('A', 'S', 7, 0);
    add('M', 'B', 0, 0);
    add('C', '-', 0, 1);
    add('X', '-', 0, 0);
    // reserved: each reserved byte on its own
    for (std::size_t i : {2u, 3u, 28u, 29u, 30u, 31u}) add('A', 'B', 9, 1)[i] = 0x80;
    // several rules broken at once: the first in rule order must win
    add('A', '-', 0, 0);                 // side
    add('A', 'B', kMax + 1, 0);          // price
    add('C', '-', 1, 1)[2] = 1;          // price
    add('M', 'S', 0, 0)[31] = 1;         // qty
    add('Q', '-', kMax + 1, 0)[28] = 1;  // type
    return out;
}

std::string decoded_line(const Decoded& d) {
    std::string s;
    s.reserve(64);
    s += std::to_string(static_cast<unsigned>(d.err));
    s += ',';
    s += std::to_string(d.ts);
    s += ',';
    s += std::to_string(static_cast<unsigned>(d.type));
    s += ',';
    s += std::to_string(d.id);
    s += ',';
    s += std::to_string(static_cast<unsigned>(d.side));
    s += ',';
    s += std::to_string(d.price);
    s += ',';
    s += std::to_string(d.qty);
    return s;
}

std::string to_hex(const WireMsg& m) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s(2 * kWireSize, '0');
    for (std::size_t i = 0; i < kWireSize; ++i) {
        const std::uint8_t b = m[kWireSize - 1 - i];
        s[2 * i]             = kDigits[b >> 4];
        s[2 * i + 1]         = kDigits[b & 0xF];
    }
    return s;
}

std::optional<WireMsg> from_hex(std::string_view s) {
    if (s.size() != 2 * kWireSize) return std::nullopt;
    WireMsg m{};
    for (std::size_t i = 0; i < kWireSize; ++i) {
        const int hi = hex_value(s[2 * i]);
        const int lo = hex_value(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        m[kWireSize - 1 - i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return m;
}

}  // namespace ob
