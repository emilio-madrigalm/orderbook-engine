#pragma once

// Deliberately naive reference order book used only as a test oracle.
//
// Every resting order lives in ONE vector in arrival order. Matching finds the
// best opposite order by linear scan: best price, then earliest arrival.
// It shares no data structures or logic with ob::OrderBook, so agreement
// between the two on random streams is strong evidence both are right.
// Speed is irrelevant here; obviousness is the goal.

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "order_book.hpp"

namespace ob::ref {

class ReferenceBook {
public:
    AddResult add_limit(const Order& order) {
        AddResult result;
        if (order.qty <= 0) return {Status::InvalidQuantity, {}};
        if (order.price <= 0) return {Status::InvalidPrice, {}};
        if (find(order.id) != orders_.end()) return {Status::DuplicateId, {}};

        Order incoming = order;
        match(incoming, /*is_limit=*/true, result.trades);
        if (incoming.qty > 0) orders_.push_back(incoming);
        return result;
    }

    AddResult add_market(const Order& order) {
        AddResult result;
        if (order.qty <= 0) return {Status::InvalidQuantity, {}};

        Order incoming = order;
        match(incoming, /*is_limit=*/false, result.trades);
        return result;
    }

    Status cancel(OrderId id) {
        auto it = find(id);
        if (it == orders_.end()) return Status::UnknownId;
        orders_.erase(it);
        return Status::Ok;
    }

    Status modify(OrderId id, Quantity new_qty) {
        if (new_qty <= 0) return Status::InvalidQuantity;
        auto it = find(id);
        if (it == orders_.end()) return Status::UnknownId;

        if (new_qty <= it->qty) {
            it->qty = new_qty;  // keeps its place in arrival order
        } else {
            Order replacement = *it;
            replacement.qty   = new_qty;
            orders_.erase(it);
            orders_.push_back(replacement);  // newest arrival: back of its level
        }
        return Status::Ok;
    }

    std::optional<Price> best_bid() const {
        std::optional<Price> best;
        for (const Order& o : orders_)
            if (o.side == Side::Buy && (!best || o.price > *best)) best = o.price;
        return best;
    }

    std::optional<Price> best_ask() const {
        std::optional<Price> best;
        for (const Order& o : orders_)
            if (o.side == Side::Sell && (!best || o.price < *best)) best = o.price;
        return best;
    }

    std::optional<Price> spread() const {
        const auto bid = best_bid();
        const auto ask = best_ask();
        if (!bid || !ask) return std::nullopt;
        return *ask - *bid;
    }

    std::vector<LevelInfo> depth(Side side, std::size_t n) const {
        std::map<Price, Quantity> totals;
        for (const Order& o : orders_)
            if (o.side == side) totals[o.price] += o.qty;

        std::vector<LevelInfo> out;
        if (side == Side::Buy) {
            for (auto it = totals.rbegin(); it != totals.rend() && out.size() < n; ++it)
                out.push_back(LevelInfo{it->first, it->second});
        } else {
            for (auto it = totals.begin(); it != totals.end() && out.size() < n; ++it)
                out.push_back(LevelInfo{it->first, it->second});
        }
        return out;
    }

    std::size_t order_count() const { return orders_.size(); }

private:
    std::vector<Order>::iterator find(OrderId id) {
        return std::find_if(orders_.begin(), orders_.end(),
                            [id](const Order& o) { return o.id == id; });
    }

    // Best-priced, earliest-arrived resting order the incoming order can trade
    // with, or orders_.end() if none.
    std::vector<Order>::iterator best_match(const Order& incoming, bool is_limit) {
        auto best = orders_.end();
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (it->side == incoming.side) continue;
            if (is_limit) {
                const bool crosses = incoming.side == Side::Buy ? it->price <= incoming.price
                                                                : it->price >= incoming.price;
                if (!crosses) continue;
            }
            // Strictly better price wins; equal price keeps the earlier arrival.
            const bool better = best == orders_.end() ||
                                (incoming.side == Side::Buy ? it->price < best->price
                                                            : it->price > best->price);
            if (better) best = it;
        }
        return best;
    }

    void match(Order& incoming, bool is_limit, std::vector<Trade>& trades) {
        while (incoming.qty > 0) {
            auto resting = best_match(incoming, is_limit);
            if (resting == orders_.end()) break;

            const Quantity fill = std::min(incoming.qty, resting->qty);
            if (incoming.side == Side::Buy) {
                trades.push_back(Trade{incoming.id, resting->id, resting->price, fill});
            } else {
                trades.push_back(Trade{resting->id, incoming.id, resting->price, fill});
            }
            incoming.qty -= fill;
            resting->qty -= fill;
            if (resting->qty == 0) orders_.erase(resting);
        }
    }

    std::vector<Order> orders_;  // arrival order, oldest first
};

}  // namespace ob::ref
