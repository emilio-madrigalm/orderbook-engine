#include "itch.hpp"

#include <algorithm>
#include <cstring>
#include <istream>
#include <ostream>
#include <vector>

namespace ob::itch {

namespace {

std::uint64_t be(const std::uint8_t* p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

// Field offsets are from the ITCH 5.0 spec; every message starts with
// type(1) locate(2) tracking(2) timestamp(6).
constexpr std::size_t kLocate = 1;
constexpr std::size_t kTime   = 5;
constexpr std::size_t kRef    = 11;

template <class Levels>
void write_levels(std::ostream& out, const Levels& levels, std::size_t depth) {
    std::size_t i = 0;
    for (auto it = levels.begin(); it != levels.end() && i < depth; ++it, ++i)
        out << ',' << it->first << ',' << it->second;
    for (; i < depth; ++i) out << ",0,0";
}

}  // namespace

std::size_t message_length(char type) {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        default:  return 0;
    }
}

Converter::Converter(Options options, std::ostream& events, std::ostream& fills, std::ostream* depth)
    : opt_(std::move(options)), events_(events), fills_(fills), depth_(depth) {
    opt_.symbol.resize(8, ' ');  // ITCH stock fields are right-padded with spaces
    events_ << kCsvHeader << '\n';
    fills_ << "market_id,resting_id,price,qty,match\n";
    if (depth_ && opt_.snapshot_every > 0) {
        *depth_ << "seq,ts_ns";
        for (std::size_t i = 1; i <= opt_.depth; ++i) *depth_ << ",bid_px" << i << ",bid_qty" << i;
        for (std::size_t i = 1; i <= opt_.depth; ++i) *depth_ << ",ask_px" << i << ",ask_qty" << i;
        *depth_ << '\n';
    }
}

void Converter::emit(Timestamp ts, EventType type, OrderId id, EventSide side, Price price, Quantity qty) {
    events_ << to_csv_line(Event{ts, type, id, side, price, qty}) << '\n';
    ++stats_.events;
    if (depth_ && opt_.snapshot_every > 0 && stats_.events % opt_.snapshot_every == 0) write_depth(ts);
}

void Converter::write_depth(Timestamp ts) {
    *depth_ << stats_.events << ',' << ts;
    write_levels(*depth_, bids_, opt_.depth);
    write_levels(*depth_, asks_, opt_.depth);
    *depth_ << '\n';
}

Quantity Converter::reduce(std::unordered_map<OrderId, Resting>::iterator it, Quantity by) {
    Resting& o = it->second;
    if (by > o.qty) {
        ++stats_.overfills;
        by = o.qty;
    }
    o.qty -= by;
    const auto drop_level = [&](auto& levels) {
        auto lv = levels.find(o.price);
        lv->second -= by;
        if (lv->second == 0) levels.erase(lv);
    };
    if (o.side == 'B') {
        drop_level(bids_);
    } else {
        drop_level(asks_);
    }
    const Quantity left = o.qty;
    if (left == 0) orders_.erase(it);
    return left;
}

std::unordered_map<OrderId, Converter::Resting>::iterator Converter::find(OrderId ref, bool& ok) {
    auto it = orders_.find(ref);
    ok      = it != orders_.end();
    if (!ok) {
        if (skipped_.count(ref)) {
            ++stats_.skipped_followups;
        } else {
            ++stats_.unknown_refs;
        }
    }
    return it;
}

bool Converter::add(Timestamp ts, OrderId ref, char side, std::uint32_t raw_price, Quantity qty) {
    const bool whole_cent = raw_price % 100 == 0;
    const Price px        = static_cast<Price>(raw_price / 100);
    if (!whole_cent || px < 1 || px > opt_.max_price || qty <= 0 || (side != 'B' && side != 'S')) {
        ++stats_.skipped_price;
        skipped_.insert(ref);
        return false;
    }
    orders_[ref] = Resting{side, px, qty};
    if (side == 'B') {
        bids_[px] += qty;
    } else {
        asks_[px] += qty;
    }
    emit(ts, EventType::Add, ref, side == 'B' ? EventSide::Buy : EventSide::Sell, px, qty);
    return true;
}

bool Converter::on_message(const std::uint8_t* m, std::size_t len) {
    if (len == 0) return false;
    ++stats_.messages;
    const char        type = static_cast<char>(m[0]);
    const std::size_t want = message_length(type);
    if (want != 0 && len != want) return false;
    if (want == 0) return true;  // a type we do not read

    if (type == 'R') {
        if (!found_ && std::memcmp(m + 11, opt_.symbol.data(), 8) == 0) {
            found_  = true;
            locate_ = static_cast<std::uint16_t>(be(m + kLocate, 2));
        }
        return true;
    }
    if (!found_ || be(m + kLocate, 2) != locate_) return true;
    ++stats_.symbol_messages;

    const Timestamp ts = be(m + kTime, 6);
    bool            ok = false;
    switch (type) {
        case 'A':
        case 'F': {
            ++stats_.adds;
            add(ts, be(m + kRef, 8), static_cast<char>(m[19]), static_cast<std::uint32_t>(be(m + 32, 4)),
                static_cast<Quantity>(be(m + 20, 4)));
            break;
        }
        case 'E': {
            ++stats_.executions;
            const OrderId ref = be(m + kRef, 8);
            auto          it  = find(ref, ok);
            if (!ok) break;
            const Quantity  qty   = static_cast<Quantity>(be(m + 19, 4));
            const Resting   o     = it->second;
            const OrderId   mid   = next_market_++;
            const EventSide aggr  = o.side == 'B' ? EventSide::Sell : EventSide::Buy;
            // Update the reference book first so the depth row after the M matches.
            reduce(it, qty);
            fills_ << mid << ',' << ref << ',' << o.price << ',' << qty << ',' << be(m + 23, 8) << '\n';
            emit(ts, EventType::Market, mid, aggr, 0, qty);
            break;
        }
        case 'C':
        case 'X': {
            ++(type == 'C' ? stats_.executions_price : stats_.cancels);
            const OrderId ref = be(m + kRef, 8);
            auto          it  = find(ref, ok);
            if (!ok) break;
            const Quantity left = reduce(it, static_cast<Quantity>(be(m + 19, 4)));
            if (left > 0) {
                emit(ts, EventType::Modify, ref, EventSide::None, 0, left);
            } else {
                emit(ts, EventType::Cancel, ref, EventSide::None, 0, 0);
            }
            break;
        }
        case 'D': {
            ++stats_.deletes;
            const OrderId ref = be(m + kRef, 8);
            auto          it  = find(ref, ok);
            if (!ok) break;
            reduce(it, it->second.qty);
            emit(ts, EventType::Cancel, ref, EventSide::None, 0, 0);
            break;
        }
        case 'U': {
            ++stats_.replaces;
            const OrderId old_ref = be(m + kRef, 8);
            const OrderId new_ref = be(m + 19, 8);
            auto          it      = find(old_ref, ok);
            if (!ok) {
                // The new order inherits the old one's fate: if that was skipped, so is this.
                if (skipped_.count(old_ref)) skipped_.insert(new_ref);
                break;
            }
            const char side = it->second.side;
            reduce(it, it->second.qty);
            emit(ts, EventType::Cancel, old_ref, EventSide::None, 0, 0);
            add(ts, new_ref, side, static_cast<std::uint32_t>(be(m + 31, 4)),
                static_cast<Quantity>(be(m + 27, 4)));
            break;
        }
        case 'P': ++stats_.hidden_trades; break;
        case 'Q': ++stats_.cross_trades; break;
        case 'B': ++stats_.broken_trades; break;
        default: break;
    }
    return true;
}

bool read_stream(std::istream& in, Converter& conv, std::string* error) {
    constexpr std::size_t   kChunk = 1 << 20;
    std::vector<std::uint8_t> buf(kChunk + 65536);
    std::size_t               have   = 0;
    std::uint64_t             offset = 0;  // stream offset of buf[0]
    for (;;) {
        in.read(reinterpret_cast<char*>(buf.data() + have), static_cast<std::streamsize>(kChunk));
        const std::size_t got = static_cast<std::size_t>(in.gcount());
        have += got;
        std::size_t pos = 0;
        while (have - pos >= 2) {
            const std::size_t len = (std::size_t{buf[pos]} << 8) | buf[pos + 1];
            if (have - pos - 2 < len) break;
            if (!conv.on_message(buf.data() + pos + 2, len)) {
                if (error) {
                    *error = "bad message at byte " + std::to_string(offset + pos) + " (type '" +
                             (len ? std::string(1, static_cast<char>(buf[pos + 2])) : std::string("?")) +
                             "', length " + std::to_string(len) + ")";
                }
                return false;
            }
            pos += 2 + len;
        }
        std::memmove(buf.data(), buf.data() + pos, have - pos);
        have -= pos;
        offset += pos;
        if (got == 0 || !in) {
            if (in.bad()) {
                if (error) *error = "read error";
                return false;
            }
            if (have != 0) {
                if (error) *error = "truncated message at byte " + std::to_string(offset);
                return false;
            }
            return true;
        }
    }
}

}  // namespace ob::itch
