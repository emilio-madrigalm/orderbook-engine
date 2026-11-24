#include "order_book.hpp"

#include <algorithm>
#include <iterator>

namespace ob {

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
            Order&         resting = queue.front();  // oldest order at this price
            const Quantity fill    = std::min(incoming.qty, resting.qty);

            if (incoming.side == Side::Buy) {
                trades.push_back(Trade{incoming.id, resting.id, level->first, fill});
            } else {
                trades.push_back(Trade{resting.id, incoming.id, level->first, fill});
            }

            incoming.qty -= fill;
            resting.qty -= fill;
            if (resting.qty == 0) {
                index_.erase(resting.id);
                queue.pop_front();
            }
        }
        if (queue.empty()) opposite.erase(level);
    }
}

void OrderBook::rest(const Order& order) {
    const PoolAllocator<Order> alloc(&arena_);
    Level& queue = order.side == Side::Buy ? bids_.try_emplace(order.price, alloc).first->second
                                           : asks_.try_emplace(order.price, alloc).first->second;
    queue.push_back(order);
    index_[order.id] = Locator{order.side, order.price, std::prev(queue.end())};
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

    auto erase_from = [&](auto& book) {
        auto level = book.find(loc.price);
        level->second.erase(loc.it);
        if (level->second.empty()) book.erase(level);
    };
    if (loc.side == Side::Buy) {
        erase_from(bids_);
    } else {
        erase_from(asks_);
    }
    return Status::Ok;
}

Status OrderBook::modify(OrderId id, Quantity new_qty) {
    if (new_qty <= 0) return Status::InvalidQuantity;

    const auto found = index_.find(id);
    if (found == index_.end()) return Status::UnknownId;

    Order& existing = *found->second.it;
    if (new_qty <= existing.qty) {
        existing.qty = new_qty;  // reducing keeps queue position
        return Status::Ok;
    }

    // Increasing qty loses time priority: cancel and re-rest at the back of the level.
    // The order was resting, so the book was not crossed at its price: no trades possible.
    Order replacement = existing;
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
        for (const auto& [price, queue] : book) {
            if (out.size() >= n) break;
            Quantity total = 0;
            for (const Order& o : queue) total += o.qty;
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
