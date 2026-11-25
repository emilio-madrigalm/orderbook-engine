// ITCH 5.0 converter: hand-built binary messages, byte offsets from the spec.

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "itch.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

void put(Bytes& b, std::uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_str(Bytes& b, std::string s, std::size_t n) {
    s.resize(n, ' ');
    b.insert(b.end(), s.begin(), s.end());
}
Bytes header(char type, std::uint16_t locate, std::uint64_t ts) {
    Bytes b{static_cast<std::uint8_t>(type)};
    put(b, locate, 2);
    put(b, 0, 2);  // tracking
    put(b, ts, 6);
    return b;
}

Bytes stock_dir(std::uint16_t locate, const std::string& sym) {
    Bytes b = header('R', locate, 0);
    put_str(b, sym, 8);
    b.resize(39, 0);
    return b;
}
Bytes add(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, char side, std::uint32_t qty,
          std::uint32_t price, bool mpid = false) {
    Bytes b = header(mpid ? 'F' : 'A', loc, ts);
    put(b, ref, 8);
    b.push_back(static_cast<std::uint8_t>(side));
    put(b, qty, 4);
    put_str(b, "AAPL", 8);
    put(b, price, 4);
    if (mpid) put_str(b, "MPID", 4);
    return b;
}
Bytes exec(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t qty, std::uint64_t match) {
    Bytes b = header('E', loc, ts);
    put(b, ref, 8);
    put(b, qty, 4);
    put(b, match, 8);
    return b;
}
Bytes exec_price(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t qty, std::uint32_t px) {
    Bytes b = header('C', loc, ts);
    put(b, ref, 8);
    put(b, qty, 4);
    put(b, 1, 8);
    b.push_back('Y');
    put(b, px, 4);
    return b;
}
Bytes cancel(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t qty) {
    Bytes b = header('X', loc, ts);
    put(b, ref, 8);
    put(b, qty, 4);
    return b;
}
Bytes del(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref) {
    Bytes b = header('D', loc, ts);
    put(b, ref, 8);
    return b;
}
Bytes replace(std::uint16_t loc, std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref,
              std::uint32_t qty, std::uint32_t px) {
    Bytes b = header('U', loc, ts);
    put(b, old_ref, 8);
    put(b, new_ref, 8);
    put(b, qty, 4);
    put(b, px, 4);
    return b;
}
Bytes hidden_trade(std::uint16_t loc) {
    Bytes b = header('P', loc, 9);
    b.resize(44, 0);
    return b;
}

std::string frame(const std::vector<Bytes>& msgs) {
    std::string s;
    for (const auto& m : msgs) {
        s.push_back(static_cast<char>(m.size() >> 8));
        s.push_back(static_cast<char>(m.size() & 0xff));
        s.append(m.begin(), m.end());
    }
    return s;
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream       in(s);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

struct Harness {
    std::ostringstream events, fills, depth;
    ob::itch::Converter conv;
    explicit Harness(std::size_t snapshot_every = 1, std::size_t d = 2)
        : conv(ob::itch::Options{"AAPL", d, snapshot_every, 131071}, events, fills, &depth) {}
    bool feed(const std::vector<Bytes>& msgs) {
        std::istringstream in(frame(msgs));
        std::string        err;
        return ob::itch::read_stream(in, conv, &err);
    }
};

constexpr std::uint16_t kAapl  = 7;
constexpr std::uint16_t kOther = 8;
const std::string       kM0    = std::to_string(ob::itch::kMarketIdBase);
const std::string       kM1    = std::to_string(ob::itch::kMarketIdBase + 1);

TEST(Itch, MessageLengthsMatchSpec) {
    EXPECT_EQ(ob::itch::message_length('A'), add(1, 0, 1, 'B', 1, 100).size());
    EXPECT_EQ(ob::itch::message_length('F'), add(1, 0, 1, 'B', 1, 100, true).size());
    EXPECT_EQ(ob::itch::message_length('E'), exec(1, 0, 1, 1, 1).size());
    EXPECT_EQ(ob::itch::message_length('C'), exec_price(1, 0, 1, 1, 100).size());
    EXPECT_EQ(ob::itch::message_length('X'), cancel(1, 0, 1, 1).size());
    EXPECT_EQ(ob::itch::message_length('D'), del(1, 0, 1).size());
    EXPECT_EQ(ob::itch::message_length('U'), replace(1, 0, 1, 2, 1, 100).size());
    EXPECT_EQ(ob::itch::message_length('?'), 0u);
}

TEST(Itch, ConvertsEachMessageType) {
    Harness r;
    ASSERT_TRUE(r.feed({
        stock_dir(kOther, "MSFT"),
        stock_dir(kAapl, "AAPL"),
        add(kAapl, 100, 11, 'S', 50, 3001500),         // ask 50 @ 30015
        add(kAapl, 101, 12, 'S', 30, 3001500, true),   // F: ask 30 @ 30015 behind it
        add(kAapl, 102, 13, 'B', 40, 3001000),         // bid 40 @ 30010
        add(kOther, 103, 99, 'B', 40, 3001000),        // other symbol: ignored
        exec(kAapl, 104, 11, 20, 555),                 // 20 of id 11 -> buy market 20
        cancel(kAapl, 105, 12, 10),                    // 12 -> 20 left
        replace(kAapl, 106, 13, 14, 25, 3000900),      // 13 -> 14: bid 25 @ 30009
        exec_price(kAapl, 107, 11, 30, 3001400),       // rest of 11, at another price
        del(kAapl, 108, 12),
        hidden_trade(kAapl),
    }));
    const std::vector<std::string> want = {
        "ts_ns,type,id,side,price,qty",
        "100,A,11,S,30015,50",
        "101,A,12,S,30015,30",
        "102,A,13,B,30010,40",
        "104,M," + kM0 + ",B,0,20",
        "105,X,12,-,0,20",
        "106,C,13,-,0,0",
        "106,A,14,B,30009,25",
        "107,C,11,-,0,0",
        "108,C,12,-,0,0",
    };
    EXPECT_EQ(lines(r.events.str()), want);
    EXPECT_EQ(lines(r.fills.str()),
              (std::vector<std::string>{"market_id,resting_id,price,qty,match", kM0 + ",11,30015,20,555"}));

    const auto& s = r.conv.stats();
    EXPECT_EQ(r.conv.locate(), kAapl);
    EXPECT_EQ(s.messages, 12u);
    EXPECT_EQ(s.symbol_messages, 9u);
    EXPECT_EQ(s.adds, 3u);
    EXPECT_EQ(s.executions, 1u);
    EXPECT_EQ(s.executions_price, 1u);
    EXPECT_EQ(s.cancels, 1u);
    EXPECT_EQ(s.deletes, 1u);
    EXPECT_EQ(s.replaces, 1u);
    EXPECT_EQ(s.hidden_trades, 1u);
    EXPECT_EQ(s.events, 9u);
    EXPECT_EQ(s.unknown_refs + s.skipped_price + s.overfills, 0u);
}

TEST(Itch, DepthFileTracksTheReferenceBook) {
    Harness r(1, 2);
    ASSERT_TRUE(r.feed({
        stock_dir(kAapl, "AAPL"),
        add(kAapl, 1, 1, 'B', 10, 1000000),  // bid 10 @ 10000
        add(kAapl, 2, 2, 'B', 5, 999900),    // bid 5 @ 9999
        add(kAapl, 3, 3, 'S', 7, 1000100),   // ask 7 @ 10001
        exec(kAapl, 4, 1, 10, 1),            // bid 10000 gone
    }));
    const std::vector<std::string> want = {
        "seq,ts_ns,bid_px1,bid_qty1,bid_px2,bid_qty2,ask_px1,ask_qty1,ask_px2,ask_qty2",
        "1,1,10000,10,0,0,0,0,0,0",
        "2,2,10000,10,9999,5,0,0,0,0",
        "3,3,10000,10,9999,5,10001,7,0,0",
        "4,4,9999,5,0,0,10001,7,0,0",
    };
    EXPECT_EQ(lines(r.depth.str()), want);
}

TEST(Itch, SnapshotEveryN) {
    Harness r(2, 1);
    ASSERT_TRUE(r.feed({stock_dir(kAapl, "AAPL"), add(kAapl, 1, 1, 'B', 1, 100), add(kAapl, 2, 2, 'B', 1, 100),
                        add(kAapl, 3, 3, 'B', 1, 100)}));
    EXPECT_EQ(lines(r.depth.str()),
              (std::vector<std::string>{"seq,ts_ns,bid_px1,bid_qty1,ask_px1,ask_qty1", "2,2,1,2,0,0"}));
}

TEST(Itch, SubCentAndOutOfBandAddsAreSkippedWithTheirFollowups) {
    Harness r;
    ASSERT_TRUE(r.feed({
        stock_dir(kAapl, "AAPL"),
        add(kAapl, 1, 1, 'B', 10, 1000050),     // $100.005: sub-cent
        add(kAapl, 2, 2, 'S', 10, 1500000000),  // $150000: above the band
        exec(kAapl, 3, 1, 5, 1),
        replace(kAapl, 4, 2, 3, 10, 1000000),   // new ref 3 inherits the skip
        del(kAapl, 5, 3),
        del(kAapl, 6, 42),                      // never added
    }));
    const auto& s = r.conv.stats();
    EXPECT_EQ(s.skipped_price, 2u);
    EXPECT_EQ(s.skipped_followups, 3u);
    EXPECT_EQ(s.unknown_refs, 1u);
    EXPECT_EQ(s.events, 0u);
    EXPECT_EQ(lines(r.fills.str()).size(), 1u);
}

TEST(Itch, TwoExecutionsGetDistinctMarketIds) {
    Harness r;
    ASSERT_TRUE(r.feed({stock_dir(kAapl, "AAPL"), add(kAapl, 1, 1, 'B', 10, 1000000), exec(kAapl, 2, 1, 4, 7),
                        exec(kAapl, 3, 1, 6, 8)}));
    EXPECT_EQ(lines(r.fills.str()),
              (std::vector<std::string>{"market_id,resting_id,price,qty,match", kM0 + ",1,10000,4,7",
                                        kM1 + ",1,10000,6,8"}));
    EXPECT_EQ(lines(r.events.str()).back(), "3,M," + kM1 + ",S,0,6");
}

TEST(Itch, OverfillIsCountedAndClamped) {
    Harness r;
    ASSERT_TRUE(r.feed({stock_dir(kAapl, "AAPL"), add(kAapl, 1, 1, 'B', 10, 1000000), cancel(kAapl, 2, 1, 15)}));
    EXPECT_EQ(r.conv.stats().overfills, 1u);
    EXPECT_EQ(lines(r.events.str()).back(), "2,C,1,-,0,0");
}

TEST(Itch, MessagesBeforeTheSymbolIsListedAreIgnored) {
    Harness r;
    ASSERT_TRUE(r.feed({add(kAapl, 1, 1, 'B', 10, 1000000), stock_dir(kAapl, "AAPL")}));
    EXPECT_EQ(r.conv.stats().events, 0u);
    EXPECT_TRUE(r.conv.symbol_found());
}

TEST(Itch, SymbolMissing) {
    Harness r;
    ASSERT_TRUE(r.feed({stock_dir(kOther, "MSFT")}));
    EXPECT_FALSE(r.conv.symbol_found());
}

TEST(Itch, WrongLengthIsRejected) {
    Harness   r;
    Bytes bad = add(kAapl, 1, 1, 'B', 10, 1000000);
    bad.pop_back();
    EXPECT_FALSE(r.feed({stock_dir(kAapl, "AAPL"), bad}));
}

TEST(Itch, TruncatedStreamIsRejected) {
    Harness                r;
    std::string        s = frame({stock_dir(kAapl, "AAPL"), add(kAapl, 1, 1, 'B', 10, 1000000)});
    std::istringstream in(s.substr(0, s.size() - 3));
    std::string        err;
    EXPECT_FALSE(ob::itch::read_stream(in, r.conv, &err));
    EXPECT_NE(err.find("truncated"), std::string::npos);
}

TEST(Itch, UnknownTypesPassThrough) {
    Harness   r;
    Bytes odd{'~', 1, 2, 3};
    EXPECT_TRUE(r.feed({odd, stock_dir(kAapl, "AAPL")}));
    EXPECT_EQ(r.conv.stats().messages, 2u);
}

TEST(Itch, LargeStreamCrossesChunkBoundaries) {
    std::vector<Bytes> msgs{stock_dir(kAapl, "AAPL")};
    for (std::uint64_t i = 1; i <= 60000; ++i) msgs.push_back(add(kAapl, i, i, 'B', 1, 1000000));
    for (std::uint64_t i = 1; i <= 60000; ++i) msgs.push_back(del(kAapl, 60000 + i, i));
    Harness r(0);
    ASSERT_TRUE(r.feed(msgs));
    EXPECT_EQ(r.conv.stats().events, 120000u);
    EXPECT_EQ(r.conv.stats().unknown_refs, 0u);
}

}  // namespace
