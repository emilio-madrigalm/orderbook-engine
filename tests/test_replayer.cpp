#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "generator.hpp"
#include "order_book.hpp"
#include "replayer.hpp"

namespace {

using ob::ReplayOptions;
using ob::ReplayStats;

struct Output {
    ReplayStats stats;
    std::string trades;
    std::string snapshots;
};

Output run(const std::string& csv, const ReplayOptions& opts) {
    std::istringstream in(csv);
    std::ostringstream trades, snaps;
    Output             out;
    out.stats     = ob::replay(in, &trades, &snaps, opts);
    out.trades    = trades.str();
    out.snapshots = snaps.str();
    return out;
}

// bid 100x10 rests; ask 102x5 rests; a sell of 4 @100 crosses the bid.
const char* kSmall =
    "ts_ns,type,id,side,price,qty\n"
    "1000,A,1,B,100,10\n"
    "2000,A,2,S,102,5\n"
    "3000,A,3,S,100,4\n";

TEST(Replayer, WritesTradesWithAggressorSide) {
    ReplayOptions opts;
    opts.snapshot_every = 0;
    const Output out    = run(kSmall, opts);
    EXPECT_EQ(out.trades,
              "ts_ns,buy_id,sell_id,price,qty,aggressor\n"
              "3000,1,3,100,4,S\n");
    EXPECT_EQ(out.stats.trades, 1u);
    EXPECT_EQ(out.stats.traded_qty, 4);
}

TEST(Replayer, WritesSnapshotsEveryEvent) {
    ReplayOptions opts;
    opts.snapshot_every = 1;
    opts.depth          = 2;
    const Output out    = run(kSmall, opts);
    EXPECT_EQ(out.snapshots,
              "seq,ts_ns,bid_px1,bid_qty1,bid_px2,bid_qty2,ask_px1,ask_qty1,ask_px2,ask_qty2\n"
              "1,1000,100,10,0,0,0,0,0,0\n"
              "2,2000,100,10,0,0,102,5,0,0\n"
              "3,3000,100,6,0,0,102,5,0,0\n");
}

TEST(Replayer, SnapshotEveryNEvents) {
    ReplayOptions opts;
    opts.snapshot_every = 2;
    opts.depth          = 1;
    const Output out    = run(kSmall, opts);
    EXPECT_EQ(out.snapshots,
              "seq,ts_ns,bid_px1,bid_qty1,ask_px1,ask_qty1\n"
              "2,2000,100,10,102,5\n");
}

TEST(Replayer, CountsEventsByType) {
    const std::string csv =
        "ts_ns,type,id,side,price,qty\n"
        "1,A,1,B,100,10\n"
        "2,A,2,S,105,10\n"
        "3,M,3,B,0,3\n"
        "4,X,1,-,0,4\n"
        "5,C,1,-,0,0\n";
    ReplayOptions opts;
    opts.snapshot_every = 0;
    const Output out    = run(csv, opts);
    EXPECT_EQ(out.stats.events, 5u);
    EXPECT_EQ(out.stats.adds, 2u);
    EXPECT_EQ(out.stats.markets, 1u);
    EXPECT_EQ(out.stats.modifies, 1u);
    EXPECT_EQ(out.stats.cancels, 1u);
    EXPECT_EQ(out.stats.rejected, 0u);
    EXPECT_EQ(out.stats.bad_lines, 0u);
    EXPECT_EQ(out.stats.trades, 1u);  // market buy of 3 hits the 105 ask
}

TEST(Replayer, CountsEngineRejects) {
    const std::string csv =
        "ts_ns,type,id,side,price,qty\n"
        "1,C,99,-,0,0\n"    // unknown id
        "2,A,1,B,100,5\n"
        "3,A,1,B,101,5\n";  // duplicate id
    ReplayOptions opts;
    opts.snapshot_every = 0;
    const Output out    = run(csv, opts);
    EXPECT_EQ(out.stats.events, 3u);
    EXPECT_EQ(out.stats.rejected, 2u);
}

TEST(Replayer, HeaderIsOptionalAndBlankLinesAreSkipped) {
    const std::string csv = "1000,A,1,B,100,10\n\n2000,A,2,S,102,5\n";
    ReplayOptions     opts;
    opts.snapshot_every = 0;
    const Output out    = run(csv, opts);
    EXPECT_EQ(out.stats.events, 2u);
    EXPECT_EQ(out.stats.bad_lines, 0u);
}

TEST(Replayer, MalformedLinesAreCountedAndSkipped) {
    const std::string csv =
        "ts_ns,type,id,side,price,qty\n"
        "1000,A,1,B,100,10\n"
        "garbage\n"
        "2000,A,2,S,102,5\n";
    ReplayOptions opts;
    opts.snapshot_every = 0;
    const Output out    = run(csv, opts);
    EXPECT_EQ(out.stats.events, 2u);
    EXPECT_EQ(out.stats.bad_lines, 1u);
    EXPECT_EQ(out.stats.first_bad_line, 3u);  // 1-based, header is line 1
}

TEST(Replayer, ToleratesWindowsLineEndings) {
    const std::string csv = "ts_ns,type,id,side,price,qty\r\n1000,A,1,B,100,10\r\n";
    ReplayOptions     opts;
    opts.snapshot_every = 0;
    const Output out    = run(csv, opts);
    EXPECT_EQ(out.stats.events, 1u);
    EXPECT_EQ(out.stats.bad_lines, 0u);
}

TEST(Replayer, NullOutputsAreAllowed) {
    std::istringstream in(kSmall);
    const ReplayStats  stats = ob::replay(in, nullptr, nullptr, ReplayOptions{});
    EXPECT_EQ(stats.events, 3u);
    EXPECT_EQ(stats.trades, 1u);
}

// End to end: generate -> CSV -> replay must match feeding the book directly.
TEST(Replayer, GeneratedStreamMatchesDirectReplay) {
    ob::GenConfig cfg;
    cfg.seed       = 21;
    cfg.num_events = 20000;

    std::ostringstream csv;
    csv << ob::kCsvHeader << '\n';
    ob::OrderBook direct;
    std::size_t   direct_trades = 0;
    ob::Quantity  direct_qty    = 0;
    ob::Generator gen(cfg);
    for (std::size_t i = 0; i < cfg.num_events; ++i) {
        const ob::Event e = gen.next();
        csv << ob::to_csv_line(e) << '\n';

        ob::AddResult r;
        if (e.type == ob::EventType::Add)
            r = direct.add_limit(ob::Order{e.id, ob::to_book_side(e.side), e.price, e.qty, e.ts});
        if (e.type == ob::EventType::Market)
            r = direct.add_market(ob::Order{e.id, ob::to_book_side(e.side), 0, e.qty, e.ts});
        if (e.type == ob::EventType::Cancel) direct.cancel(e.id);
        direct_trades += r.trades.size();
        for (const auto& t : r.trades) direct_qty += t.qty;
    }

    ReplayOptions opts;
    opts.snapshot_every = 0;
    const Output out    = run(csv.str(), opts);
    EXPECT_EQ(out.stats.events, cfg.num_events);
    EXPECT_EQ(out.stats.bad_lines, 0u);
    EXPECT_EQ(out.stats.rejected, 0u);
    EXPECT_EQ(out.stats.trades, direct_trades);
    EXPECT_EQ(out.stats.traded_qty, direct_qty);
}

}  // namespace
