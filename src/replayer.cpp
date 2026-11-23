#include "replayer.hpp"

#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "event.hpp"
#include "order_book.hpp"

namespace ob {

namespace {

void write_snapshot_header(std::ostream& out, std::size_t depth) {
    out << "seq,ts_ns";
    for (std::size_t i = 1; i <= depth; ++i) out << ",bid_px" << i << ",bid_qty" << i;
    for (std::size_t i = 1; i <= depth; ++i) out << ",ask_px" << i << ",ask_qty" << i;
    out << '\n';
}

void write_side(std::ostream& out, const std::vector<LevelInfo>& levels, std::size_t depth) {
    for (std::size_t i = 0; i < depth; ++i) {
        if (i < levels.size()) {
            out << ',' << levels[i].price << ',' << levels[i].qty;
        } else {
            out << ",0,0";
        }
    }
}

void write_snapshot(std::ostream& out, const OrderBook& book, std::uint64_t seq, Timestamp ts,
                    std::size_t depth) {
    out << seq << ',' << ts;
    write_side(out, book.depth(Side::Buy, depth), depth);
    write_side(out, book.depth(Side::Sell, depth), depth);
    out << '\n';
}

}  // namespace

ReplayStats replay(std::istream& in, std::ostream* trades_out, std::ostream* snapshots_out,
                   const ReplayOptions& options) {
    ReplayStats stats;
    OrderBook   book;

    if (trades_out) *trades_out << "ts_ns,buy_id,sell_id,price,qty,aggressor\n";
    const bool snapshots = snapshots_out && options.snapshot_every > 0;
    if (snapshots) write_snapshot_header(*snapshots_out, options.depth);

    std::string   line;
    std::uint64_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line == kCsvHeader) continue;

        const auto parsed = parse_csv_line(line);
        if (!parsed) {
            if (stats.bad_lines++ == 0) stats.first_bad_line = line_no;
            continue;
        }
        const Event& e = *parsed;
        ++stats.events;

        AddResult result;
        Status    status    = Status::Ok;
        char      aggressor = '-';
        switch (e.type) {
            case EventType::Add:
                ++stats.adds;
                result    = book.add_limit(Order{e.id, to_book_side(e.side), e.price, e.qty, e.ts});
                status    = result.status;
                aggressor = static_cast<char>(e.side);
                break;
            case EventType::Market:
                ++stats.markets;
                result    = book.add_market(Order{e.id, to_book_side(e.side), 0, e.qty, e.ts});
                status    = result.status;
                aggressor = static_cast<char>(e.side);
                break;
            case EventType::Cancel:
                ++stats.cancels;
                status = book.cancel(e.id);
                break;
            case EventType::Modify:
                ++stats.modifies;
                status = book.modify(e.id, e.qty);
                break;
        }
        if (status != Status::Ok) ++stats.rejected;

        for (const Trade& t : result.trades) {
            ++stats.trades;
            stats.traded_qty += t.qty;
            if (trades_out) {
                *trades_out << e.ts << ',' << t.buy_id << ',' << t.sell_id << ',' << t.price << ','
                            << t.qty << ',' << aggressor << '\n';
            }
        }

        if (snapshots && stats.events % options.snapshot_every == 0)
            write_snapshot(*snapshots_out, book, stats.events, e.ts, options.depth);
    }
    return stats;
}

}  // namespace ob
