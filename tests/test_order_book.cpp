#include <gtest/gtest.h>

#include "order.hpp"

TEST(OrderTypes, OrderHoldsFields) {
    ob::Order o{42, ob::Side::Sell, 10050, 7, 1};
    EXPECT_EQ(o.id, 42u);
    EXPECT_EQ(o.side, ob::Side::Sell);
    EXPECT_EQ(o.price, 10050);
    EXPECT_EQ(o.qty, 7);
    EXPECT_EQ(o.timestamp, 1u);
}

TEST(OrderTypes, TradeHoldsFields) {
    ob::Trade t{1, 2, 10050, 3};
    EXPECT_EQ(t.buy_id, 1u);
    EXPECT_EQ(t.sell_id, 2u);
    EXPECT_EQ(t.price, 10050);
    EXPECT_EQ(t.qty, 3);
}
