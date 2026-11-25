#include <gtest/gtest.h>

#include "wire.hpp"

namespace {

using ob::Event;
using ob::EventSide;
using ob::EventType;
using ob::WireError;

Event ev(EventType t, EventSide s, ob::Price px, ob::Quantity q) {
    Event e;
    e.ts    = 123456789012345ULL;
    e.type  = t;
    e.id    = 0x0102030405060708ULL;
    e.side  = s;
    e.price = px;
    e.qty   = q;
    return e;
}

WireError err_of(const Event& e) { return ob::decode(*ob::encode(e)).err; }

TEST(Wire, LayoutIsLittleEndianAtFixedOffsets) {
    const auto m = *ob::encode(ev(EventType::Add, EventSide::Buy, 0x1FFFF, 0x0A0B0C0D));
    EXPECT_EQ(m[0], 'A');
    EXPECT_EQ(m[1], 'B');
    EXPECT_EQ(m[2], 0);
    EXPECT_EQ(m[3], 0);
    EXPECT_EQ(m[4], 0x0D);  // qty LSB first
    EXPECT_EQ(m[7], 0x0A);
    EXPECT_EQ(m[16], 0x08);  // id LSB first
    EXPECT_EQ(m[23], 0x01);
    EXPECT_EQ(m[24], 0xFF);  // price
    EXPECT_EQ(m[26], 0x01);
    for (std::size_t i = 28; i < 32; ++i) EXPECT_EQ(m[i], 0);
}

TEST(Wire, ValidEventsRoundTrip) {
    for (const Event& e : {ev(EventType::Add, EventSide::Sell, 1, 1),
                           ev(EventType::Add, EventSide::Buy, ob::kWireMaxPrice, 4000000000LL),
                           ev(EventType::Market, EventSide::Buy, 0, 7),
                           ev(EventType::Cancel, EventSide::None, 0, 0),
                           ev(EventType::Modify, EventSide::None, 0, 3)}) {
        const auto d = ob::decode(*ob::encode(e));
        EXPECT_EQ(d.err, WireError::None) << ob::to_csv_line(e);
        EXPECT_EQ(d.ts, e.ts);
        EXPECT_EQ(d.id, e.id);
        EXPECT_EQ(d.type, static_cast<std::uint8_t>(e.type));
        EXPECT_EQ(d.side, static_cast<std::uint8_t>(e.side));
        EXPECT_EQ(d.price, static_cast<std::uint32_t>(e.price));
        EXPECT_EQ(d.qty, static_cast<std::uint32_t>(e.qty));
    }
}

TEST(Wire, EachRuleHasItsOwnCode) {
    auto bad_type = *ob::encode(ev(EventType::Add, EventSide::Buy, 5, 1));
    bad_type[0]   = 'Z';
    EXPECT_EQ(ob::decode(bad_type).err, WireError::BadType);

    EXPECT_EQ(err_of(ev(EventType::Add, EventSide::None, 5, 1)), WireError::BadSide);
    EXPECT_EQ(err_of(ev(EventType::Cancel, EventSide::Buy, 0, 0)), WireError::BadSide);

    EXPECT_EQ(err_of(ev(EventType::Add, EventSide::Buy, 0, 1)), WireError::BadPrice);
    EXPECT_EQ(err_of(ev(EventType::Add, EventSide::Buy, ob::kWireMaxPrice + 1, 1)), WireError::BadPrice);
    EXPECT_EQ(err_of(ev(EventType::Market, EventSide::Sell, 9, 1)), WireError::BadPrice);

    EXPECT_EQ(err_of(ev(EventType::Add, EventSide::Buy, 5, 0)), WireError::BadQty);
    EXPECT_EQ(err_of(ev(EventType::Cancel, EventSide::None, 0, 1)), WireError::BadQty);
    EXPECT_EQ(err_of(ev(EventType::Modify, EventSide::None, 0, 0)), WireError::BadQty);

    for (std::size_t i : {2u, 3u, 28u, 31u}) {
        auto m = *ob::encode(ev(EventType::Market, EventSide::Buy, 0, 1));
        m[i]   = 1;
        EXPECT_EQ(ob::decode(m).err, WireError::Reserved) << "byte " << i;
    }
}

TEST(Wire, FirstFailedRuleWins) {
    // Wrong side, wrong price and zero qty at once: side is reported.
    EXPECT_EQ(err_of(ev(EventType::Add, EventSide::None, 0, 0)), WireError::BadSide);
    // Wrong price and zero qty: price is reported.
    EXPECT_EQ(err_of(ev(EventType::Add, EventSide::Buy, 0, 0)), WireError::BadPrice);
}

TEST(Wire, EncodeRefusesFieldsThatDoNotFit) {
    EXPECT_FALSE(ob::encode(ev(EventType::Add, EventSide::Buy, 5, -1)));
    EXPECT_FALSE(ob::encode(ev(EventType::Add, EventSide::Buy, 5, 1LL << 32)));
    EXPECT_FALSE(ob::encode(ev(EventType::Add, EventSide::Buy, -5, 1)));
}

TEST(Wire, HexIsByte31FirstAndRoundTrips) {
    const auto m = *ob::encode(ev(EventType::Add, EventSide::Buy, 0x1FFFF, 1));
    const std::string h = ob::to_hex(m);
    ASSERT_EQ(h.size(), 64u);
    EXPECT_EQ(h.substr(62), "41");  // byte 0 = 'A' comes last
    EXPECT_EQ(h.substr(8, 8), "0001ffff");  // bytes 27..24 = price
    EXPECT_EQ(ob::from_hex(h), m);
    EXPECT_FALSE(ob::from_hex(h.substr(1)));
    EXPECT_FALSE(ob::from_hex(std::string(64, 'g')));
}

TEST(Wire, EdgeCasesCoverEveryCodeAndBound) {
    const auto cases = ob::edge_cases();
    int per_code[6] = {};
    bool max_ok = false, max_plus_one_bad = false;
    for (const auto& m : cases) {
        const auto d = ob::decode(m);
        ++per_code[static_cast<int>(d.err)];
        if (d.type == 'A' && d.price == ob::kWireMaxPrice && d.err == WireError::None) max_ok = true;
        if (d.type == 'A' && d.price == ob::kWireMaxPrice + 1 && d.err == WireError::BadPrice) max_plus_one_bad = true;
    }
    for (int c = 0; c < 6; ++c) EXPECT_GE(per_code[c], 3) << "code " << c;
    EXPECT_TRUE(max_ok);
    EXPECT_TRUE(max_plus_one_bad);
    // Expected codes, in list order (pins the list so edits are deliberate).
    std::string codes;
    for (const auto& m : cases) codes += static_cast<char>('0' + static_cast<int>(ob::decode(m).err));
    EXPECT_EQ(codes, "00000000" "111" "22222" "333333" "4444" "555555" "23341");
}

TEST(Wire, DecodedLineFormat) {
    const auto d = ob::decode(*ob::encode(ev(EventType::Cancel, EventSide::None, 0, 0)));
    EXPECT_EQ(ob::decoded_line(d), "0,123456789012345,67,72623859790382856,45,0,0");
}

}  // namespace
