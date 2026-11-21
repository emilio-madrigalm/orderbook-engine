#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "order.hpp"

namespace ob {

enum class Status {
    Ok,
    InvalidQuantity,  // qty <= 0
    InvalidPrice,     // limit price <= 0
    DuplicateId,      // a resting order already uses this id
    UnknownId,        // cancel/modify of an id that is not resting
};

// Result of adding an order: a status plus any trades it generated.
struct AddResult {
    Status             status{Status::Ok};
    std::vector<Trade> trades;
};

// One aggregated price level (total resting quantity at a price).
struct LevelInfo {
    Price    price{};
    Quantity qty{};
    bool operator==(const LevelInfo&) const = default;
};

// Baseline limit order book: std::map of price levels, each a FIFO std::list,
// plus an id -> iterator index for O(1) lookup on cancel/modify.
//
// Matching is price-time priority: best price first, then arrival order within
// a price. Trades execute at the RESTING order's price.
class OrderBook {
public:
    // Match against the opposite side while prices cross; rest any remainder.
    AddResult add_limit(const Order& order);

    // Match until filled or the opposite side is empty. Never rests.
    // The id of a market order is not stored, so it is not checked for duplicates.
    AddResult add_market(const Order& order);

    Status cancel(OrderId id);

    // Cancel/replace semantics. Reducing (or keeping) qty keeps queue priority.
    // Increasing qty moves the order to the back of its price level.
    Status modify(OrderId id, Quantity new_qty);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<Price> spread() const;  // best_ask - best_bid, if both sides exist

    // Top n aggregated levels on one side, best price first.
    std::vector<LevelInfo> depth(Side side, std::size_t n) const;

    std::size_t order_count() const { return index_.size(); }

private:
    using Level = std::list<Order>;

    struct Locator {
        Side            side;
        Price           price;
        Level::iterator it;
    };

    template <class Book>
    void match(Book& opposite, Order& incoming, bool is_limit, std::vector<Trade>& trades);

    void rest(const Order& order);

    std::map<Price, Level, std::greater<>> bids_;
    std::map<Price, Level>                 asks_;
    std::unordered_map<OrderId, Locator>   index_;
};

}  // namespace ob
