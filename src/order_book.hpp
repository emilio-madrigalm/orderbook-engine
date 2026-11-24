#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

#include "order.hpp"
#include "pool_allocator.hpp"

namespace ob {

enum class Status {
    Ok,
    InvalidQuantity,  // qty <= 0
    InvalidPrice,     // limit price <= 0
    PriceOutOfBand,   // limit price outside the book's tick band
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

// Sentinel tick index meaning "no level".
inline constexpr std::size_t kNoTick = std::numeric_limits<std::size_t>::max();

// Order book: one flat array of price levels per side, indexed by price tick
// (price - min_price), plus a bitmap per side marking non-empty levels and a
// cached best level per side. Each level is an intrusive doubly linked FIFO of
// resting orders, and an id -> locator index gives O(1) cancel/modify. Nodes and
// index entries come from the book's NodeArena (see pool_allocator.hpp), so
// steady-state add/cancel does not call the global heap.
//
// The tick band [min_price, max_price] is fixed at construction: the level arrays
// cannot grow without moving levels. Limit orders priced outside it are rejected
// with Status::PriceOutOfBand. Market orders carry no price and are unaffected.
//
// Matching is price-time priority: best price first, then arrival order within
// a price. Trades execute at the RESTING order's price.
//
// The book keeps only what matching needs per resting order (id and quantity).
// Price and side are implied by the level and index entry that hold the node, and
// Order::timestamp is not stored: the book never reads it.
class OrderBook {
public:
    static constexpr Price kDefaultMinPrice = 1;
    static constexpr Price kDefaultMaxPrice = 131071;  // 2^17 - 1 ticks

    // Throws std::invalid_argument unless 1 <= min_price <= max_price.
    explicit OrderBook(Price min_price = kDefaultMinPrice, Price max_price = kDefaultMaxPrice);
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
    Price       min_price() const { return min_price_; }
    Price       max_price() const { return max_price_; }

private:
    // 32 bytes on 64-bit targets: two nodes per cache line, no padding.
    struct OrderNode {
        OrderId    id{};
        Quantity   qty{};
        OrderNode* prev{};
        OrderNode* next{};
    };
    static_assert(sizeof(OrderNode) <= 4 * sizeof(void*), "OrderNode should stay compact");

    struct Level {
        OrderNode* head{};
        OrderNode* tail{};
        bool       empty() const { return head == nullptr; }
    };

    // Open-addressing (linear probing, backward-shift deletion) id -> locator table.
    // Power-of-two capacity, grows at 1/2 load. Entries are compact and contiguous,
    // so a lookup is usually one cache line instead of a bucket-list pointer chase.
    class IdIndex {
    public:
        struct Entry {
            OrderId  id{};
            OrderNode* node{};   // nullptr marks an empty slot
            std::uint32_t tick{};
            Side     side{};
        };

        IdIndex() = default;
        IdIndex(const IdIndex&)            = delete;
        IdIndex& operator=(const IdIndex&) = delete;

        std::size_t size() const noexcept { return size_; }
        bool        contains(OrderId id) const noexcept { return find(id) != nullptr; }

        const Entry* find(OrderId id) const noexcept;
        Entry*       find(OrderId id) noexcept;
        void         insert(OrderId id, Side side, std::size_t tick, OrderNode* node);
        void         erase(OrderId id) noexcept;

    private:
        static std::size_t hash(OrderId id) noexcept {
            return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> 16);
        }
        void grow();

        std::vector<Entry> slots_;
        std::size_t        mask_{0};
        std::size_t        size_{0};
    };

    std::size_t tick_of(Price price) const {
        return static_cast<std::size_t>(price - min_price_);
    }
    Price price_of(std::size_t tick) const { return min_price_ + static_cast<Price>(tick); }
    bool  in_band(Price price) const { return price >= min_price_ && price <= max_price_; }

    void match(Side incoming_side, Order& incoming, bool is_limit, std::vector<Trade>& trades);
    void rest(const Order& order);

    // Called when a level becomes empty: clears its bit and fixes the cached best.
    void drop_level(Side side, std::size_t tick) noexcept;

    OrderNode* new_node(const Order& order);
    void       free_node(OrderNode* node) noexcept;
    static void link_back(Level& level, OrderNode* node) noexcept;
    static void unlink(Level& level, OrderNode* node) noexcept;

    // Declared first: the containers below allocate from it and must not outlive it.
    NodeArena arena_;
    IdIndex   index_;

    Price min_price_;
    Price max_price_;

    std::vector<Level>         bid_levels_;
    std::vector<Level>         ask_levels_;
    std::vector<std::uint64_t> bid_bits_;  // bit t set <=> bid level at tick t is non-empty
    std::vector<std::uint64_t> ask_bits_;
    std::size_t                best_bid_{kNoTick};  // highest non-empty bid tick
    std::size_t                best_ask_{kNoTick};  // lowest non-empty ask tick
};

}  // namespace ob
