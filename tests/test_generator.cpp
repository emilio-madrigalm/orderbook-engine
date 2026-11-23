#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "event.hpp"
#include "generator.hpp"
#include "order_book.hpp"

namespace {

using ob::Event;
using ob::EventType;
using ob::GenConfig;
using ob::EventSide;

// ---- CSV schema ------------------------------------------------------------

TEST(EventCsv, HeaderIsFixed) {
    EXPECT_EQ(std::string(ob::kCsvHeader), "ts_ns,type,id,side,price,qty");
}

TEST(EventCsv, FormatsEachEventType) {
    EXPECT_EQ(ob::to_csv_line(Event{1500, EventType::Add, 7, EventSide::Buy, 10005, 40}),
              "1500,A,7,B,10005,40");
    EXPECT_EQ(ob::to_csv_line(Event{1600, EventType::Add, 8, EventSide::Sell, 10010, 5}),
              "1600,A,8,S,10010,5");
    EXPECT_EQ(ob::to_csv_line(Event{1700, EventType::Market, 9, EventSide::Buy, 0, 25}),
              "1700,M,9,B,0,25");
    EXPECT_EQ(ob::to_csv_line(Event{1800, EventType::Cancel, 7, EventSide::None, 0, 0}),
              "1800,C,7,-,0,0");
    EXPECT_EQ(ob::to_csv_line(Event{1900, EventType::Modify, 8, EventSide::None, 0, 12}),
              "1900,X,8,-,0,12");
}

TEST(EventCsv, RoundTrips) {
    const std::vector<Event> events = {
        {1500, EventType::Add, 7, EventSide::Buy, 10005, 40},
        {1600, EventType::Add, 8, EventSide::Sell, 10010, 5},
        {1700, EventType::Market, 9, EventSide::Buy, 0, 25},
        {1800, EventType::Cancel, 7, EventSide::None, 0, 0},
        {1900, EventType::Modify, 8, EventSide::None, 0, 12},
    };
    for (const Event& e : events) {
        const auto parsed = ob::parse_csv_line(ob::to_csv_line(e));
        ASSERT_TRUE(parsed.has_value()) << ob::to_csv_line(e);
        EXPECT_EQ(*parsed, e);
    }
}

TEST(EventCsv, RejectsMalformedLines) {
    EXPECT_FALSE(ob::parse_csv_line(""));
    EXPECT_FALSE(ob::parse_csv_line("1,A,7,B,100"));         // too few fields
    EXPECT_FALSE(ob::parse_csv_line("1,A,7,B,100,5,9"));     // too many fields
    EXPECT_FALSE(ob::parse_csv_line("1,Z,7,B,100,5"));       // bad type
    EXPECT_FALSE(ob::parse_csv_line("1,A,7,Q,100,5"));       // bad side
    EXPECT_FALSE(ob::parse_csv_line("1,A,7,B,abc,5"));       // bad number
    EXPECT_FALSE(ob::parse_csv_line("1,A,7,B,100,5x"));      // trailing junk
    EXPECT_FALSE(ob::parse_csv_line("-1,A,7,B,100,5"));      // negative timestamp
    EXPECT_FALSE(ob::parse_csv_line("1,AA,7,B,100,5"));      // multi-char type
    EXPECT_FALSE(ob::parse_csv_line("ts_ns,type,id,side,price,qty"));  // header is not an event
}

// ---- Generator -------------------------------------------------------------

GenConfig config(std::uint64_t seed, std::size_t n) {
    GenConfig c;
    c.seed       = seed;
    c.num_events = n;
    return c;
}

TEST(Generator, ProducesRequestedCount) {
    EXPECT_EQ(ob::generate(config(1, 0)).size(), 0u);
    EXPECT_EQ(ob::generate(config(1, 1)).size(), 1u);
    EXPECT_EQ(ob::generate(config(1, 5000)).size(), 5000u);
}

TEST(Generator, SameSeedSameStream) {
    EXPECT_EQ(ob::generate(config(42, 20000)), ob::generate(config(42, 20000)));
}

TEST(Generator, DifferentSeedDifferentStream) {
    EXPECT_NE(ob::generate(config(1, 2000)), ob::generate(config(2, 2000)));
}

TEST(Generator, PrefixIsStableAcrossLengths) {
    // Asking for more events must not change the earlier ones.
    const auto short_run = ob::generate(config(7, 1000));
    const auto long_run  = ob::generate(config(7, 3000));
    ASSERT_GE(long_run.size(), short_run.size());
    for (std::size_t i = 0; i < short_run.size(); ++i) EXPECT_EQ(short_run[i], long_run[i]) << i;
}

TEST(Generator, TimestampsNeverDecrease) {
    const auto events = ob::generate(config(3, 50000));
    for (std::size_t i = 1; i < events.size(); ++i)
        ASSERT_GE(events[i].ts, events[i - 1].ts) << "event " << i;
    EXPECT_GT(events.back().ts, events.front().ts);
}

TEST(Generator, MixIsRoughlySixtyThirtyTen) {
    const auto  events = ob::generate(config(5, 100000));
    std::size_t limits = 0, cancels = 0, markets = 0, other = 0;
    for (const Event& e : events) {
        switch (e.type) {
            case EventType::Add: ++limits; break;
            case EventType::Cancel: ++cancels; break;
            case EventType::Market: ++markets; break;
            default: ++other; break;
        }
    }
    const double n = static_cast<double>(events.size());
    EXPECT_NEAR(limits / n, 0.60, 0.02);
    EXPECT_NEAR(cancels / n, 0.30, 0.02);
    EXPECT_NEAR(markets / n, 0.10, 0.02);
    EXPECT_EQ(other, 0u);  // generator never emits modifies
}

TEST(Generator, FieldsAreWellFormed) {
    const GenConfig c      = config(9, 50000);
    const auto      events = ob::generate(c);
    for (const Event& e : events) {
        switch (e.type) {
            case EventType::Add:
                ASSERT_NE(e.side, EventSide::None);
                ASSERT_GT(e.price, 0);
                ASSERT_GE(e.qty, 1);
                ASSERT_LE(e.qty, c.max_qty);
                break;
            case EventType::Market:
                ASSERT_NE(e.side, EventSide::None);
                ASSERT_EQ(e.price, 0);
                ASSERT_GE(e.qty, 1);
                ASSERT_LE(e.qty, c.max_qty);
                break;
            case EventType::Cancel:
                ASSERT_EQ(e.side, EventSide::None);
                ASSERT_EQ(e.price, 0);
                ASSERT_EQ(e.qty, 0);
                break;
            case EventType::Modify: FAIL() << "unexpected modify";
        }
    }
}

TEST(Generator, NewOrderIdsAreUniqueAndIncreasing) {
    const auto events = ob::generate(config(11, 50000));
    ob::OrderId last = 0;
    for (const Event& e : events) {
        if (e.type == EventType::Cancel) continue;
        ASSERT_GT(e.id, last);
        last = e.id;
    }
}

TEST(Generator, CancelsTargetIssuedLimitIdsAtMostOnce) {
    const auto           events = ob::generate(config(13, 50000));
    std::set<ob::OrderId> issued, cancelled;
    for (const Event& e : events) {
        if (e.type == EventType::Add) issued.insert(e.id);
        if (e.type == EventType::Cancel) {
            ASSERT_TRUE(issued.count(e.id)) << "cancel of never-issued id " << e.id;
            ASSERT_TRUE(cancelled.insert(e.id).second) << "double cancel of id " << e.id;
        }
    }
}

TEST(Generator, PricesStayNearTheRandomWalkingMid) {
    const GenConfig c      = config(17, 100000);
    const auto      events = ob::generate(c);
    ob::Price lo = INT64_MAX, hi = 0;
    for (const Event& e : events) {
        if (e.type != EventType::Add) continue;
        lo = std::min(lo, e.price);
        hi = std::max(hi, e.price);
    }
    EXPECT_GT(lo, 0);
    EXPECT_GT(hi, lo);  // the mid actually moves
}

TEST(Generator, ReplaysThroughOrderBookWithoutRejectsOrStaleCancels) {
    const auto    events = ob::generate(config(19, 100000));
    ob::OrderBook book;
    for (const Event& e : events) {
        const ob::Order o{e.id, ob::to_book_side(e.side), e.price, e.qty, e.ts};
        switch (e.type) {
            case EventType::Add: ASSERT_EQ(book.add_limit(o).status, ob::Status::Ok); break;
            case EventType::Market: ASSERT_EQ(book.add_market(o).status, ob::Status::Ok); break;
            case EventType::Cancel:
                // The generator tracks fills, so it only cancels orders still resting.
                ASSERT_EQ(book.cancel(e.id), ob::Status::Ok) << "stale cancel of id " << e.id;
                break;
            case EventType::Modify: FAIL() << "unexpected modify";
        }
        if (book.best_bid() && book.best_ask()) {
            ASSERT_LT(*book.best_bid(), *book.best_ask());
        }
    }
    EXPECT_GT(book.order_count(), 0u);
}

}  // namespace
