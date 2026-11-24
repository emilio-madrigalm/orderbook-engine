#include "order_book.hpp"

#include <algorithm>
#include <bit>
#include <new>
#include <stdexcept>

namespace ob {

namespace {

std::size_t band_size(Price min_price, Price max_price) {
    if (min_price < 1 || max_price < min_price)
        throw std::invalid_argument("OrderBook: need 1 <= min_price <= max_price");
    return static_cast<std::size_t>(max_price - min_price) + 1;
}

std::size_t words_for(std::size_t ticks) { return (ticks + 63) / 64; }

void set_bit(std::vector<std::uint64_t>& bits, std::size_t t) noexcept {
    bits[t >> 6] |= std::uint64_t{1} << (t & 63);
}

void clear_bit(std::vector<std::uint64_t>& bits, std::size_t t) noexcept {
    bits[t >> 6] &= ~(std::uint64_t{1} << (t & 63));
}

// Highest set bit at or below `from` (requires from < 64 * bits.size()), or kNoTick.
std::size_t highest_set(const std::vector<std::uint64_t>& bits, std::size_t from) noexcept {
    std::size_t   w    = from >> 6;
    std::uint64_t word = bits[w] & (~std::uint64_t{0} >> (63 - (from & 63)));
    while (true) {
        if (word != 0) return (w << 6) + 63 - static_cast<std::size_t>(std::countl_zero(word));
        if (w == 0) return kNoTick;
        word = bits[--w];
    }
}

// Lowest set bit at or above `from` (requires from < 64 * bits.size()), or kNoTick.
std::size_t lowest_set(const std::vector<std::uint64_t>& bits, std::size_t from) noexcept {
    std::size_t   w    = from >> 6;
    std::uint64_t word = bits[w] & (~std::uint64_t{0} << (from & 63));
    while (true) {
        if (word != 0) return (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
        if (++w == bits.size()) return kNoTick;
        word = bits[w];
    }
}

}  // namespace

OrderBook::OrderBook(Price min_price, Price max_price)
    : index_(0, std::hash<OrderId>{}, std::equal_to<OrderId>{},
             PoolAllocator<IndexNode>(&arena_)),
      min_price_(min_price),
      max_price_(max_price),
      bid_levels_(band_size(min_price, max_price)),
      ask_levels_(band_size(min_price, max_price)),
      bid_bits_(words_for(bid_levels_.size()), 0),
      ask_bits_(words_for(ask_levels_.size()), 0) {}

OrderBook::OrderNode* OrderBook::new_node(const Order& order) {
    void* mem = arena_.allocate(sizeof(OrderNode));
    return ::new (mem) OrderNode{order, nullptr, nullptr};
}

void OrderBook::free_node(OrderNode* node) noexcept {
    arena_.deallocate(node, sizeof(OrderNode));
}

void OrderBook::link_back(Level& level, OrderNode* node) noexcept {
    node->prev = level.tail;
    node->next = nullptr;
    if (level.tail) {
        level.tail->next = node;
    } else {
        level.head = node;
    }
    level.tail = node;
}

void OrderBook::unlink(Level& level, OrderNode* node) noexcept {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        level.head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        level.tail = node->prev;
    }
}

void OrderBook::drop_level(Side side, std::size_t tick) noexcept {
    if (side == Side::Buy) {
        clear_bit(bid_bits_, tick);
        if (best_bid_ == tick) best_bid_ = tick > 0 ? highest_set(bid_bits_, tick - 1) : kNoTick;
    } else {
        clear_bit(ask_bits_, tick);
        if (best_ask_ == tick) {
            best_ask_ = tick + 1 < ask_levels_.size() ? lowest_set(ask_bits_, tick + 1) : kNoTick;
        }
    }
}

void OrderBook::match(Side incoming_side, Order& incoming, bool is_limit,
                      std::vector<Trade>& trades) {
    const bool         buying = incoming_side == Side::Buy;
    std::vector<Level>& levels = buying ? ask_levels_ : bid_levels_;
    std::size_t&       best   = buying ? best_ask_ : best_bid_;  // best opposite tick
    const Side         opposite = buying ? Side::Sell : Side::Buy;

    while (incoming.qty > 0 && best != kNoTick) {
        const std::size_t tick        = best;
        const Price       level_price = price_of(tick);

        if (is_limit) {
            const bool crosses = buying ? level_price <= incoming.price
                                        : level_price >= incoming.price;
            if (!crosses) break;
        }

        Level& queue = levels[tick];
        while (incoming.qty > 0 && !queue.empty()) {
            OrderNode*     resting = queue.head;  // oldest order at this price
            const Quantity fill    = std::min(incoming.qty, resting->order.qty);

            if (buying) {
                trades.push_back(Trade{incoming.id, resting->order.id, level_price, fill});
            } else {
                trades.push_back(Trade{resting->order.id, incoming.id, level_price, fill});
            }

            incoming.qty -= fill;
            resting->order.qty -= fill;
            if (resting->order.qty == 0) {
                index_.erase(resting->order.id);
                unlink(queue, resting);
                free_node(resting);
            }
        }
        if (queue.empty()) drop_level(opposite, tick);
    }
}

void OrderBook::rest(const Order& order) {
    const std::size_t tick    = tick_of(order.price);
    const bool        is_bid  = order.side == Side::Buy;
    Level&            level   = is_bid ? bid_levels_[tick] : ask_levels_[tick];
    std::size_t&      best    = is_bid ? best_bid_ : best_ask_;

    set_bit(is_bid ? bid_bits_ : ask_bits_, tick);
    if (best == kNoTick || (is_bid ? tick > best : tick < best)) best = tick;

    OrderNode* node = new_node(order);
    link_back(level, node);
    index_[order.id] = Locator{order.side, tick, &level, node};
}

AddResult OrderBook::add_limit(const Order& order) {
    AddResult result;
    if (order.qty <= 0) {
        result.status = Status::InvalidQuantity;
        return result;
    }
    if (order.price <= 0) {
        result.status = Status::InvalidPrice;
        return result;
    }
    if (!in_band(order.price)) {
        result.status = Status::PriceOutOfBand;
        return result;
    }
    if (index_.contains(order.id)) {
        result.status = Status::DuplicateId;
        return result;
    }

    Order incoming = order;
    match(incoming.side, incoming, true, result.trades);
    if (incoming.qty > 0) rest(incoming);
    return result;
}

AddResult OrderBook::add_market(const Order& order) {
    AddResult result;
    if (order.qty <= 0) {
        result.status = Status::InvalidQuantity;
        return result;
    }

    Order incoming = order;
    match(incoming.side, incoming, false, result.trades);
    // Any unfilled remainder is dropped: market orders never rest.
    return result;
}

Status OrderBook::cancel(OrderId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) return Status::UnknownId;

    const Locator loc = found->second;
    index_.erase(found);

    unlink(*loc.level, loc.node);
    if (loc.level->empty()) drop_level(loc.side, loc.tick);
    free_node(loc.node);
    return Status::Ok;
}

Status OrderBook::modify(OrderId id, Quantity new_qty) {
    if (new_qty <= 0) return Status::InvalidQuantity;

    const auto found = index_.find(id);
    if (found == index_.end()) return Status::UnknownId;

    OrderNode* existing = found->second.node;
    if (new_qty <= existing->order.qty) {
        existing->order.qty = new_qty;  // reducing keeps queue position
        return Status::Ok;
    }

    // Increasing qty loses time priority: cancel and re-rest at the back of the level.
    // The order was resting, so the book was not crossed at its price: no trades possible.
    Order replacement = existing->order;
    replacement.qty   = new_qty;
    cancel(id);
    rest(replacement);
    return Status::Ok;
}

std::optional<Price> OrderBook::best_bid() const {
    if (best_bid_ == kNoTick) return std::nullopt;
    return price_of(best_bid_);
}

std::optional<Price> OrderBook::best_ask() const {
    if (best_ask_ == kNoTick) return std::nullopt;
    return price_of(best_ask_);
}

std::optional<Price> OrderBook::spread() const {
    if (best_bid_ == kNoTick || best_ask_ == kNoTick) return std::nullopt;
    return price_of(best_ask_) - price_of(best_bid_);
}

std::vector<LevelInfo> OrderBook::depth(Side side, std::size_t n) const {
    std::vector<LevelInfo> out;
    auto total_at = [](const Level& level) {
        Quantity total = 0;
        for (const OrderNode* o = level.head; o != nullptr; o = o->next) total += o->order.qty;
        return total;
    };

    if (side == Side::Buy) {
        for (std::size_t t = best_bid_; t != kNoTick && out.size() < n;
             t = t > 0 ? highest_set(bid_bits_, t - 1) : kNoTick) {
            out.push_back(LevelInfo{price_of(t), total_at(bid_levels_[t])});
        }
    } else {
        for (std::size_t t = best_ask_; t != kNoTick && out.size() < n;
             t = t + 1 < ask_levels_.size() ? lowest_set(ask_bits_, t + 1) : kNoTick) {
            out.push_back(LevelInfo{price_of(t), total_at(ask_levels_[t])});
        }
    }
    return out;
}

}  // namespace ob
