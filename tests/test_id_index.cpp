// Id index behaviour under sparse, colliding and deleted ids: the flat open-addressing
// table must find, reject and forget exactly the ids the book holds.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "order_book.hpp"

using ob::Order;
using ob::OrderBook;
using ob::Side;
using ob::Status;

TEST(IdIndex, SparseLargeIdsAreFoundAndForgotten) {
    OrderBook book;
    std::mt19937_64 rng{7};
    std::set<std::uint64_t> ids;
    std::vector<std::uint64_t> order;
    // Priced on distinct levels so nothing crosses; ids are sparse 64-bit values.
    while (ids.size() < 5000) {
        const std::uint64_t id = rng() | 1;  // never 0
        if (!ids.insert(id).second) continue;
        order.push_back(id);
        const ob::Price px = 1 + static_cast<ob::Price>(order.size() % 100000);
        ASSERT_EQ(book.add_limit(Order{id, Side::Buy, px, 1, 0}).status, Status::Ok);
    }
    EXPECT_EQ(book.order_count(), 5000u);
    for (const auto id : order) {
        EXPECT_EQ(book.add_limit(Order{id, Side::Buy, 1, 1, 0}).status, Status::DuplicateId);
    }

    // Cancel a random half in random order; the rest must stay reachable.
    std::shuffle(order.begin(), order.end(), rng);
    const std::size_t half = order.size() / 2;
    for (std::size_t i = 0; i < half; ++i) {
        ASSERT_EQ(book.cancel(order[i]), Status::Ok) << "cancel " << i;
    }
    for (std::size_t i = 0; i < half; ++i) {
        EXPECT_EQ(book.cancel(order[i]), Status::UnknownId);
    }
    for (std::size_t i = half; i < order.size(); ++i) {
        EXPECT_EQ(book.modify(order[i], 2), Status::Ok) << "modify " << i;
    }
    EXPECT_EQ(book.order_count(), order.size() - half);
}

TEST(IdIndex, ReinsertingAnErasedIdIsAllowed) {
    OrderBook book;
    for (std::uint64_t round = 0; round < 3; ++round) {
        for (std::uint64_t id = 1000; id < 1500; ++id) {
            ASSERT_EQ(book.add_limit(Order{id * 7919, Side::Sell, 10, 1, 0}).status, Status::Ok);
        }
        for (std::uint64_t id = 1000; id < 1500; ++id) {
            ASSERT_EQ(book.cancel(id * 7919), Status::Ok);
        }
        EXPECT_EQ(book.order_count(), 0u);
    }
}
