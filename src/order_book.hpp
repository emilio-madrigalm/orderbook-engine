#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "order.hpp"
#include "pool_allocator.hpp"

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

// Order book: std::map of price levels, each an intrusive doubly linked FIFO of
// resting orders, plus an id -> node index for O(1) cancel/modify. Nodes and
// index entries come from the book's own NodeArena (see pool_allocator.hpp), so
// steady-state add/cancel does not call the global heap.
//
// Matching is price-time priority: best price first, then arrival order within
// a price. Trades execute at the RESTING order's price.
class OrderBook {
public:
    OrderBook()
        : bids_(std::greater<>{}, PoolAllocator<LevelNode>(&arena_)),
          asks_(std::less<>{}, PoolAllocator<LevelNode>(&arena_)),
          index_(0, std::hash<OrderId>{}, std::equal_to<OrderId>{},
                 PoolAllocator<IndexNode>(&arena_)) {}
    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;

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
    struct OrderNode {
        Order      order;
        OrderNode* prev{};
        OrderNode* next{};
    };

    struct Level {
        OrderNode* head{};
        OrderNode* tail{};
        bool       empty() const { return head == nullptr; }
    };

    struct Locator {
        Side       side;
        Price      price;
        Level*     level;  // stable: std::map nodes do not move
        OrderNode* node;
    };

    using LevelNode = std::pair<const Price, Level>;
    using IndexNode = std::pair<const OrderId, Locator>;
    using BidMap    = std::map<Price, Level, std::greater<>, PoolAllocator<LevelNode>>;
    using AskMap    = std::map<Price, Level, std::less<>, PoolAllocator<LevelNode>>;
    using IndexMap  = std::unordered_map<OrderId, Locator, std::hash<OrderId>,
                                        std::equal_to<OrderId>, PoolAllocator<IndexNode>>;

    template <class Book>
    void match(Book& opposite, Order& incoming, bool is_limit, std::vector<Trade>& trades);

    void rest(const Order& order);

    OrderNode* new_node(const Order& order);
    void       free_node(OrderNode* node) noexcept;
    static void link_back(Level& level, OrderNode* node) noexcept;
    static void unlink(Level& level, OrderNode* node) noexcept;

    // Declared first: the containers below allocate from it and must not outlive it.
    NodeArena arena_;
    BidMap    bids_;
    AskMap    asks_;
    IndexMap  index_;
};

}  // namespace ob
