#include <gtest/gtest.h>

#include "order.hpp"
#include "order_book.hpp"

using namespace ob;

namespace {

Order mk(OrderId id, Side side, Price price, Quantity qty) {
    return Order{id, side, price, qty, 0};
}

void expect_trade(const Trade& t, OrderId buy, OrderId sell, Price price, Quantity qty) {
    EXPECT_EQ(t.buy_id, buy);
    EXPECT_EQ(t.sell_id, sell);
    EXPECT_EQ(t.price, price);
    EXPECT_EQ(t.qty, qty);
}

}  // namespace

// ---- Types (from the scaffold commit) ----

TEST(OrderTypes, OrderHoldsFields) {
    Order o{42, Side::Sell, 10050, 7, 1};
    EXPECT_EQ(o.id, 42u);
    EXPECT_EQ(o.side, Side::Sell);
    EXPECT_EQ(o.price, 10050);
    EXPECT_EQ(o.qty, 7);
    EXPECT_EQ(o.timestamp, 1u);
}

TEST(OrderTypes, TradeHoldsFields) {
    Trade t{1, 2, 10050, 3};
    EXPECT_EQ(t.buy_id, 1u);
    EXPECT_EQ(t.sell_id, 2u);
    EXPECT_EQ(t.price, 10050);
    EXPECT_EQ(t.qty, 3);
}

// ---- Matching ----

TEST(OrderBook, EmptyBookHasNoQuotes) {
    OrderBook book;
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_FALSE(book.spread());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, NonCrossingOrdersRest) {
    OrderBook book;
    EXPECT_TRUE(book.add_limit(mk(1, Side::Buy, 99, 5)).trades.empty());
    EXPECT_TRUE(book.add_limit(mk(2, Side::Sell, 100, 5)).trades.empty());
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.best_ask(), 100);
    EXPECT_EQ(book.spread(), 1);
    EXPECT_EQ(book.order_count(), 2u);
}

TEST(OrderBook, SimpleMatch) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 10));
    auto r = book.add_limit(mk(2, Side::Buy, 100, 10));

    ASSERT_EQ(r.status, Status::Ok);
    ASSERT_EQ(r.trades.size(), 1u);
    expect_trade(r.trades[0], 2, 1, 100, 10);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_ask());
    EXPECT_FALSE(book.best_bid());
}

TEST(OrderBook, TradeExecutesAtRestingPrice) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 5));
    auto r = book.add_limit(mk(2, Side::Buy, 105, 5));  // willing to pay 105

    ASSERT_EQ(r.trades.size(), 1u);
    expect_trade(r.trades[0], 2, 1, 100, 5);  // but trades at 100
}

TEST(OrderBook, PartialFillOfIncomingRestsRemainder) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 5));
    auto r = book.add_limit(mk(2, Side::Buy, 100, 8));

    ASSERT_EQ(r.trades.size(), 1u);
    expect_trade(r.trades[0], 2, 1, 100, 5);
    EXPECT_FALSE(book.best_ask());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.depth(Side::Buy, 1), (std::vector<LevelInfo>{{100, 3}}));
}

TEST(OrderBook, PartialFillOfRestingOrderLeavesRemainder) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 10));
    auto r = book.add_limit(mk(2, Side::Buy, 100, 4));

    ASSERT_EQ(r.trades.size(), 1u);
    expect_trade(r.trades[0], 2, 1, 100, 4);
    EXPECT_EQ(book.depth(Side::Sell, 1), (std::vector<LevelInfo>{{100, 6}}));
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, MultiLevelSweepBuy) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 101, 5));
    book.add_limit(mk(2, Side::Sell, 102, 5));
    book.add_limit(mk(3, Side::Sell, 103, 5));

    auto r = book.add_limit(mk(4, Side::Buy, 102, 12));

    ASSERT_EQ(r.trades.size(), 2u);
    expect_trade(r.trades[0], 4, 1, 101, 5);  // best price first
    expect_trade(r.trades[1], 4, 2, 102, 5);
    EXPECT_EQ(book.best_bid(), 102);  // remaining 2 rests at its limit
    EXPECT_EQ(book.best_ask(), 103);  // 103 is beyond the limit, untouched
    EXPECT_EQ(book.depth(Side::Buy, 1), (std::vector<LevelInfo>{{102, 2}}));
}

TEST(OrderBook, MultiLevelSweepSell) {
    OrderBook book;
    book.add_limit(mk(1, Side::Buy, 100, 10));
    book.add_limit(mk(2, Side::Buy, 99, 10));

    auto r = book.add_limit(mk(3, Side::Sell, 99, 15));

    ASSERT_EQ(r.trades.size(), 2u);
    expect_trade(r.trades[0], 1, 3, 100, 10);  // highest bid first
    expect_trade(r.trades[1], 2, 3, 99, 5);
    EXPECT_EQ(book.depth(Side::Buy, 5), (std::vector<LevelInfo>{{99, 5}}));
}

TEST(OrderBook, FifoWithinLevel) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 5));
    book.add_limit(mk(2, Side::Sell, 100, 5));
    book.add_limit(mk(3, Side::Sell, 100, 5));

    auto r = book.add_limit(mk(4, Side::Buy, 100, 7));

    ASSERT_EQ(r.trades.size(), 2u);
    expect_trade(r.trades[0], 4, 1, 100, 5);  // oldest first
    expect_trade(r.trades[1], 4, 2, 100, 2);
    EXPECT_EQ(book.cancel(1), Status::UnknownId);  // fully filled
    EXPECT_EQ(book.cancel(2), Status::Ok);         // partially filled, still resting
    EXPECT_EQ(book.cancel(3), Status::Ok);
}

// ---- Market orders ----

TEST(OrderBook, MarketOrderOnEmptyBook) {
    OrderBook book;
    auto r = book.add_market(mk(1, Side::Buy, 0, 5));
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_TRUE(r.trades.empty());
    EXPECT_EQ(book.order_count(), 0u);  // does not rest
}

TEST(OrderBook, MarketOrderSweepsLevelsAndDropsRemainder) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 3));
    book.add_limit(mk(2, Side::Sell, 105, 3));

    auto r = book.add_market(mk(3, Side::Buy, 0, 10));

    ASSERT_EQ(r.trades.size(), 2u);
    expect_trade(r.trades[0], 3, 1, 100, 3);
    expect_trade(r.trades[1], 3, 2, 105, 3);
    EXPECT_EQ(book.order_count(), 0u);  // remainder of 4 is discarded, not rested
    EXPECT_FALSE(book.best_bid());
}

TEST(OrderBook, MarketSellHitsBids) {
    OrderBook book;
    book.add_limit(mk(1, Side::Buy, 100, 5));
    auto r = book.add_market(mk(2, Side::Sell, 0, 3));
    ASSERT_EQ(r.trades.size(), 1u);
    expect_trade(r.trades[0], 1, 2, 100, 3);
    EXPECT_EQ(book.depth(Side::Buy, 1), (std::vector<LevelInfo>{{100, 2}}));
}

// ---- Cancel ----

TEST(OrderBook, CancelRemovesOrderAndUpdatesBest) {
    OrderBook book;
    book.add_limit(mk(1, Side::Buy, 100, 5));
    book.add_limit(mk(2, Side::Buy, 99, 5));

    EXPECT_EQ(book.cancel(1), Status::Ok);
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, CancelUnknownId) {
    OrderBook book;
    EXPECT_EQ(book.cancel(123), Status::UnknownId);
}

TEST(OrderBook, CancelTwiceFailsSecondTime) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 5));
    EXPECT_EQ(book.cancel(1), Status::Ok);
    EXPECT_EQ(book.cancel(1), Status::UnknownId);
}

TEST(OrderBook, CancelFullyFilledOrderFails) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 5));
    book.add_limit(mk(2, Side::Buy, 100, 5));
    EXPECT_EQ(book.cancel(1), Status::UnknownId);
}

// ---- Validation ----

TEST(OrderBook, RejectsNonPositiveQuantity) {
    OrderBook book;
    EXPECT_EQ(book.add_limit(mk(1, Side::Buy, 100, 0)).status, Status::InvalidQuantity);
    EXPECT_EQ(book.add_limit(mk(2, Side::Buy, 100, -5)).status, Status::InvalidQuantity);
    EXPECT_EQ(book.add_market(mk(3, Side::Buy, 0, 0)).status, Status::InvalidQuantity);
    EXPECT_EQ(book.add_market(mk(4, Side::Sell, 0, -1)).status, Status::InvalidQuantity);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, RejectsNonPositiveLimitPrice) {
    OrderBook book;
    EXPECT_EQ(book.add_limit(mk(1, Side::Buy, 0, 5)).status, Status::InvalidPrice);
    EXPECT_EQ(book.add_limit(mk(2, Side::Sell, -10, 5)).status, Status::InvalidPrice);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, DuplicateIdRejectedAndBookUnchanged) {
    OrderBook book;
    book.add_limit(mk(1, Side::Buy, 100, 5));
    auto r = book.add_limit(mk(1, Side::Sell, 90, 5));  // would have crossed

    EXPECT_EQ(r.status, Status::DuplicateId);
    EXPECT_TRUE(r.trades.empty());
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.depth(Side::Buy, 1), (std::vector<LevelInfo>{{100, 5}}));
}

TEST(OrderBook, IdCanBeReusedAfterFullFill) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 5));
    book.add_limit(mk(2, Side::Buy, 100, 5));  // order 1 fully filled
    EXPECT_EQ(book.add_limit(mk(1, Side::Buy, 90, 5)).status, Status::Ok);
}

// ---- Modify ----

TEST(OrderBook, ModifyDownKeepsPriority) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 10));
    book.add_limit(mk(2, Side::Sell, 100, 10));

    EXPECT_EQ(book.modify(1, 5), Status::Ok);
    auto r = book.add_limit(mk(3, Side::Buy, 100, 5));

    ASSERT_EQ(r.trades.size(), 1u);
    expect_trade(r.trades[0], 3, 1, 100, 5);  // order 1 is still first in line
}

TEST(OrderBook, ModifyUpLosesPriority) {
    OrderBook book;
    book.add_limit(mk(1, Side::Sell, 100, 10));
    book.add_limit(mk(2, Side::Sell, 100, 10));

    EXPECT_EQ(book.modify(1, 15), Status::Ok);
    auto r = book.add_limit(mk(3, Side::Buy, 100, 5));

    ASSERT_EQ(r.trades.size(), 1u);
    expect_trade(r.trades[0], 3, 2, 100, 5);  // order 2 is now first in line
    EXPECT_EQ(book.depth(Side::Sell, 1), (std::vector<LevelInfo>{{100, 20}}));  // 15 + 5
}

TEST(OrderBook, ModifyInvalidArguments) {
    OrderBook book;
    book.add_limit(mk(1, Side::Buy, 100, 5));
    EXPECT_EQ(book.modify(1, 0), Status::InvalidQuantity);
    EXPECT_EQ(book.modify(1, -3), Status::InvalidQuantity);
    EXPECT_EQ(book.modify(99, 5), Status::UnknownId);
    EXPECT_EQ(book.depth(Side::Buy, 1), (std::vector<LevelInfo>{{100, 5}}));
}

// ---- Depth ----

TEST(OrderBook, DepthAggregatesLevelsBestFirst) {
    OrderBook book;
    book.add_limit(mk(1, Side::Buy, 100, 5));
    book.add_limit(mk(2, Side::Buy, 100, 3));
    book.add_limit(mk(3, Side::Buy, 99, 2));
    book.add_limit(mk(4, Side::Buy, 98, 1));
    book.add_limit(mk(5, Side::Sell, 101, 4));
    book.add_limit(mk(6, Side::Sell, 102, 6));

    EXPECT_EQ(book.depth(Side::Buy, 2), (std::vector<LevelInfo>{{100, 8}, {99, 2}}));
    EXPECT_EQ(book.depth(Side::Buy, 10).size(), 3u);
    EXPECT_EQ(book.depth(Side::Sell, 10), (std::vector<LevelInfo>{{101, 4}, {102, 6}}));
    EXPECT_TRUE(book.depth(Side::Buy, 0).empty());
}
