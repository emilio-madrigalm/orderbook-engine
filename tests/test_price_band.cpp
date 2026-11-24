// Tick-band behaviour of the flat-array book: band validation, rejection of
// out-of-band prices, and best-level tracking across 64-bit bitmap word boundaries.

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "order_book.hpp"

using ob::LevelInfo;
using ob::Order;
using ob::OrderBook;
using ob::Side;
using ob::Status;

namespace {

Order lim(ob::OrderId id, Side side, ob::Price price, ob::Quantity qty) {
    return Order{id, side, price, qty, 0};
}

}  // namespace

TEST(PriceBand, DefaultBandCoversGeneratorRange) {
    OrderBook book;
    EXPECT_EQ(book.add_limit(lim(1, Side::Buy, 1, 5)).status, Status::Ok);
    EXPECT_EQ(book.add_limit(lim(2, Side::Sell, 101000, 5)).status, Status::Ok);
    EXPECT_EQ(book.best_bid(), 1);
    EXPECT_EQ(book.best_ask(), 101000);
}

TEST(PriceBand, BoundaryPricesAreAccepted) {
    OrderBook book(10, 20);
    EXPECT_EQ(book.add_limit(lim(1, Side::Buy, 10, 1)).status, Status::Ok);
    EXPECT_EQ(book.add_limit(lim(2, Side::Sell, 20, 1)).status, Status::Ok);
    EXPECT_EQ(book.order_count(), 2u);
}

TEST(PriceBand, OutOfBandPriceIsRejectedAndNothingRests) {
    OrderBook book(10, 20);
    EXPECT_EQ(book.add_limit(lim(1, Side::Buy, 9, 1)).status, Status::PriceOutOfBand);
    EXPECT_EQ(book.add_limit(lim(2, Side::Sell, 21, 1)).status, Status::PriceOutOfBand);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(PriceBand, NonPositivePriceIsStillInvalidPrice) {
    OrderBook book(10, 20);
    EXPECT_EQ(book.add_limit(lim(1, Side::Buy, 0, 1)).status, Status::InvalidPrice);
    EXPECT_EQ(book.add_limit(lim(2, Side::Buy, -4, 1)).status, Status::InvalidPrice);
}

TEST(PriceBand, OutOfBandRejectionDoesNotConsumeId) {
    OrderBook book(10, 20);
    EXPECT_EQ(book.add_limit(lim(7, Side::Buy, 99, 1)).status, Status::PriceOutOfBand);
    EXPECT_EQ(book.add_limit(lim(7, Side::Buy, 15, 1)).status, Status::Ok);
}

TEST(PriceBand, ConstructorRejectsBadBand) {
    EXPECT_THROW(OrderBook(0, 10), std::invalid_argument);
    EXPECT_THROW(OrderBook(-5, 10), std::invalid_argument);
    EXPECT_THROW(OrderBook(10, 9), std::invalid_argument);
    EXPECT_NO_THROW(OrderBook(10, 10));
}

TEST(PriceBand, BestBidTracksAcrossWordBoundaries) {
    OrderBook book(1, 1000);
    book.add_limit(lim(1, Side::Buy, 3, 1));
    book.add_limit(lim(2, Side::Buy, 70, 1));
    book.add_limit(lim(3, Side::Buy, 130, 1));
    EXPECT_EQ(book.best_bid(), 130);

    EXPECT_EQ(book.cancel(3), Status::Ok);
    EXPECT_EQ(book.best_bid(), 70);
    EXPECT_EQ(book.cancel(2), Status::Ok);
    EXPECT_EQ(book.best_bid(), 3);
    EXPECT_EQ(book.cancel(1), Status::Ok);
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(PriceBand, BestAskTracksAcrossWordBoundaries) {
    OrderBook book(1, 1000);
    book.add_limit(lim(1, Side::Sell, 900, 1));
    book.add_limit(lim(2, Side::Sell, 65, 1));
    book.add_limit(lim(3, Side::Sell, 64, 1));
    EXPECT_EQ(book.best_ask(), 64);

    EXPECT_EQ(book.cancel(3), Status::Ok);
    EXPECT_EQ(book.best_ask(), 65);
    EXPECT_EQ(book.cancel(2), Status::Ok);
    EXPECT_EQ(book.best_ask(), 900);
}

TEST(PriceBand, SpreadUsesFlatArrayBest) {
    OrderBook book(1, 1000);
    book.add_limit(lim(1, Side::Buy, 120, 1));
    book.add_limit(lim(2, Side::Sell, 200, 1));
    EXPECT_EQ(book.spread(), 80);
}

TEST(PriceBand, DepthWalksAcrossGapsInOrder) {
    OrderBook book(1, 1000);
    book.add_limit(lim(1, Side::Buy, 130, 2));
    book.add_limit(lim(2, Side::Buy, 64, 3));
    book.add_limit(lim(3, Side::Buy, 63, 4));
    book.add_limit(lim(4, Side::Buy, 2, 5));
    book.add_limit(lim(5, Side::Buy, 2, 1));

    const std::vector<LevelInfo> expected{{130, 2}, {64, 3}, {63, 4}, {2, 6}};
    EXPECT_EQ(book.depth(Side::Buy, 10), expected);
    EXPECT_EQ(book.depth(Side::Buy, 2),
              (std::vector<LevelInfo>{{130, 2}, {64, 3}}));
    EXPECT_TRUE(book.depth(Side::Sell, 5).empty());
}

TEST(PriceBand, SweepAcrossWordBoundariesFillsInPriceOrder) {
    OrderBook book(1, 1000);
    book.add_limit(lim(1, Side::Sell, 63, 1));
    book.add_limit(lim(2, Side::Sell, 64, 1));
    book.add_limit(lim(3, Side::Sell, 128, 1));
    book.add_limit(lim(4, Side::Sell, 200, 1));

    const auto result = book.add_limit(lim(5, Side::Buy, 200, 4));
    ASSERT_EQ(result.status, Status::Ok);
    ASSERT_EQ(result.trades.size(), 4u);
    EXPECT_EQ(result.trades[0].price, 63);
    EXPECT_EQ(result.trades[1].price, 64);
    EXPECT_EQ(result.trades[2].price, 128);
    EXPECT_EQ(result.trades[3].price, 200);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(PriceBand, LimitStopsAtFirstNonCrossingLevelAcrossWords) {
    OrderBook book(1, 1000);
    book.add_limit(lim(1, Side::Sell, 60, 1));
    book.add_limit(lim(2, Side::Sell, 100, 1));
    book.add_limit(lim(3, Side::Sell, 200, 1));

    const auto result = book.add_limit(lim(4, Side::Buy, 100, 5));
    ASSERT_EQ(result.trades.size(), 2u);
    EXPECT_EQ(result.trades[0].price, 60);
    EXPECT_EQ(result.trades[1].price, 100);
    EXPECT_EQ(book.best_ask(), 200);
    EXPECT_EQ(book.best_bid(), 100);  // remainder 3 rests at 100
}

TEST(PriceBand, MarketSellHitsHighestBidAcrossWords) {
    OrderBook book(1, 1000);
    book.add_limit(lim(1, Side::Buy, 10, 1));
    book.add_limit(lim(2, Side::Buy, 500, 1));
    const auto result = book.add_market(lim(3, Side::Sell, 0, 1));
    ASSERT_EQ(result.trades.size(), 1u);
    EXPECT_EQ(result.trades[0].price, 500);
    EXPECT_EQ(book.best_bid(), 10);
}

TEST(PriceBand, ReusingAnEmptiedLevelRestoresBest) {
    OrderBook book(1, 1000);
    book.add_limit(lim(1, Side::Buy, 50, 1));
    EXPECT_EQ(book.cancel(1), Status::Ok);
    EXPECT_FALSE(book.best_bid().has_value());

    book.add_limit(lim(2, Side::Buy, 40, 1));
    EXPECT_EQ(book.best_bid(), 40);
    book.add_limit(lim(3, Side::Buy, 50, 1));
    EXPECT_EQ(book.best_bid(), 50);
}
