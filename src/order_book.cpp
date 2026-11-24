#include "order_book.hpp"

#include <algorithm>
#include <new>

namespace ob {

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

template <class Book>
void OrderBook::match(Book& opposite, Order& incoming, bool is_limit, std::vector<Trade>& trades) {
    while (incoming.qty > 0 && !opposite.empty()) {
        auto level = opposite.begin();  // best price on the opposite side

        if (is_limit) {
            const bool crosses = incoming.side == Side::Buy ? level->first <= incoming.price
                                                            : level->first >= incoming.price;
            if (!crosses) break;
        }

        Level& queue = level->second;
        while (incoming.qty > 0 && !queue.empty()) {
            OrderNode*     resting = queue.head;  // oldest order at this price
            const Quantity fill    = std::min(incoming.qty, resting->order.qty);

            if (incoming.side == Side::Buy) {
                trades.push_back(Trade{incoming.id, resting->order.id, level->first, fill});
            } else {
                trades.push_back(Trade{resting->order.id, incoming.id, level->first, fill});
            }

            incoming.qty -= fill;
            resting->order.qty -= fill;
            if (resting->order.qty == 0) {
                index_.erase(resting->order.id);
                unlink(queue, resting);
                free_node(resting);
            }
        }
        if (queue.empty()) opposite.erase(level);
    }
}

void OrderBook::rest(const Order& order) {
    Level& level = order.side == Side::Buy ? bids_.try_emplace(order.price).first->second
                                           : asks_.try_emplace(order.price).first->second;
    OrderNode* node = new_node(order);
    link_back(level, node);
    index_[order.id] = Locator{order.side, order.price, &level, node};
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
    if (index_.contains(order.id)) {
        result.status = Status::DuplicateId;
        return result;
    }

    Order incoming = order;
    if (incoming.side == Side::Buy) {
        match(asks_, incoming, true, result.trades);
    } else {
        match(bids_, incoming, true, result.trades);
    }
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
    if (incoming.side == Side::Buy) {
        match(asks_, incoming, false, result.trades);
    } else {
        match(bids_, incoming, false, result.trades);
    }
    // Any unfilled remainder is dropped: market orders never rest.
    return result;
}

Status OrderBook::cancel(OrderId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) return Status::UnknownId;

    const Locator loc = found->second;
    index_.erase(found);

    unlink(*loc.level, loc.node);
    if (loc.level->empty()) {
        if (loc.side == Side::Buy) {
            bids_.erase(loc.price);
        } else {
            asks_.erase(loc.price);
        }
    }
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
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    return asks_.begin()->first - bids_.begin()->first;
}

std::vector<LevelInfo> OrderBook::depth(Side side, std::size_t n) const {
    std::vector<LevelInfo> out;
    auto collect = [&](const auto& book) {
        for (const auto& [price, level] : book) {
            if (out.size() >= n) break;
            Quantity total = 0;
            for (const OrderNode* o = level.head; o != nullptr; o = o->next) total += o->order.qty;
            out.push_back(LevelInfo{price, total});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return out;
}

}  // namespace ob
