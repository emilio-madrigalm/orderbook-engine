#include "event.hpp"

#include <charconv>
#include <cstdint>

namespace ob {

std::string to_csv_line(const Event& e) {
    std::string out;
    out.reserve(48);
    out += std::to_string(e.ts);
    out += ',';
    out += static_cast<char>(e.type);
    out += ',';
    out += std::to_string(e.id);
    out += ',';
    out += static_cast<char>(e.side);
    out += ',';
    out += std::to_string(e.price);
    out += ',';
    out += std::to_string(e.qty);
    return out;
}

namespace {

// Takes the next comma-separated field off the front of `rest`.
// Returns false if there is no field (only possible for the first call on "").
std::string_view next_field(std::string_view& rest, bool& last) {
    const auto comma = rest.find(',');
    if (comma == std::string_view::npos) {
        last = true;
        const auto field = rest;
        rest             = {};
        return field;
    }
    last             = false;
    const auto field = rest.substr(0, comma);
    rest.remove_prefix(comma + 1);
    return field;
}

template <class T>
bool parse_number(std::string_view s, T& out) {
    if (s.empty()) return false;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

}  // namespace

std::optional<Event> parse_csv_line(std::string_view line) {
    std::string_view fields[6];
    std::string_view rest = line;
    for (int i = 0; i < 6; ++i) {
        bool last = false;
        fields[i] = next_field(rest, last);
        // Field 5 must be the last; fields 0-4 must not be.
        if ((i < 5) == last) return std::nullopt;
    }

    Event e;
    if (!parse_number(fields[0], e.ts)) return std::nullopt;

    if (fields[1].size() != 1) return std::nullopt;
    switch (fields[1][0]) {
        case 'A': e.type = EventType::Add; break;
        case 'M': e.type = EventType::Market; break;
        case 'C': e.type = EventType::Cancel; break;
        case 'X': e.type = EventType::Modify; break;
        default: return std::nullopt;
    }

    if (!parse_number(fields[2], e.id)) return std::nullopt;

    if (fields[3].size() != 1) return std::nullopt;
    switch (fields[3][0]) {
        case 'B': e.side = EventSide::Buy; break;
        case 'S': e.side = EventSide::Sell; break;
        case '-': e.side = EventSide::None; break;
        default: return std::nullopt;
    }

    if (!parse_number(fields[4], e.price)) return std::nullopt;
    if (!parse_number(fields[5], e.qty)) return std::nullopt;
    return e;
}

}  // namespace ob
