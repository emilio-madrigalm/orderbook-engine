#pragma once

// Replays a CSV event stream (schema in event.hpp) through an OrderBook and
// writes what happened as CSV for the Python layer.
//
// trades CSV:     ts_ns,buy_id,sell_id,price,qty,aggressor      (aggressor = B or S)
// snapshots CSV:  seq,ts_ns,bid_px1,bid_qty1,...,ask_px1,ask_qty1,...
//                 one row after every `snapshot_every`-th event, `depth` levels
//                 per side, best first; an empty level is written as 0,0.

#include <cstddef>
#include <cstdint>
#include <iosfwd>

#include "order.hpp"

namespace ob {

struct ReplayOptions {
    std::size_t snapshot_every{1000};  // 0 disables snapshots
    std::size_t depth{5};              // levels per side in each snapshot
};

struct ReplayStats {
    std::uint64_t events{};  // well-formed events fed to the book
    std::uint64_t adds{};
    std::uint64_t markets{};
    std::uint64_t cancels{};
    std::uint64_t modifies{};
    std::uint64_t trades{};
    Quantity      traded_qty{};
    std::uint64_t rejected{};        // events the book answered with a non-Ok status
    std::uint64_t bad_lines{};       // malformed CSV lines (skipped)
    std::uint64_t first_bad_line{};  // 1-based line number of the first one, 0 if none
};

// Output streams may be null to skip that output.
ReplayStats replay(std::istream& in, std::ostream* trades_out, std::ostream* snapshots_out,
                   const ReplayOptions& options);

}  // namespace ob
