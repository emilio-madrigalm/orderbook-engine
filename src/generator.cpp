#include "generator.hpp"

#include <algorithm>
#include <cmath>

namespace ob {

Generator::Generator(const GenConfig& config)
    : cfg_(config), rng_{config.seed}, mid_(config.start_mid) {
    remaining_.push_back(0);  // slot 0 unused: ids start at 1
    live_pos_.push_back(kNotLive);
}

double Generator::uniform01() {
    // 53 random bits -> [0, 1)
    return static_cast<double>(raw() >> 11) * (1.0 / 9007199254740992.0);
}

std::uint64_t Generator::below(std::uint64_t n) { return raw() % n; }

Quantity Generator::random_qty() {
    return 1 + static_cast<Quantity>(below(static_cast<std::uint64_t>(cfg_.max_qty)));
}

OrderId Generator::issue_id() {
    remaining_.push_back(0);
    live_pos_.push_back(kNotLive);
    return next_id_++;
}

void Generator::track(OrderId id, Quantity qty) {
    remaining_[id] = qty;
    live_pos_[id]  = static_cast<std::uint32_t>(live_.size());
    live_.push_back(id);
}

void Generator::untrack(OrderId id) {
    const std::uint32_t pos = live_pos_[id];
    const OrderId       moved = live_.back();
    live_[pos]                = moved;  // swap-remove: O(1)
    live_pos_[moved]          = pos;
    live_.pop_back();
    live_pos_[id] = kNotLive;
}

Event Generator::draw_limit(Timestamp ts) {
    const bool buy = below(2) == 0;

    // Distance from the mid in ticks: geometric, at least 1 (passive).
    Price offset = 1;
    while (uniform01() >= cfg_.offset_p && offset < 1000) ++offset;

    Price price;
    if (uniform01() < cfg_.p_aggressive) {
        // Priced through the mid, so it will usually trade on arrival.
        price = buy ? mid_ + offset : mid_ - offset;
    } else {
        price = buy ? mid_ - offset : mid_ + offset;
    }
    price = std::max<Price>(price, 1);

    const Quantity qty = random_qty();
    const OrderId  id  = issue_id();
    return Event{ts, EventType::Add, id, buy ? EventSide::Buy : EventSide::Sell, price, qty};
}

Event Generator::draw() {
    // Poisson arrivals: exponential gap with mean 1e9 / rate nanoseconds.
    const double gap = -std::log(1.0 - uniform01()) * (1e9 / cfg_.rate_per_sec);
    clock_ns_ += gap;
    const auto ts = static_cast<Timestamp>(clock_ns_);

    // Random-walk mid, clamped.
    if (uniform01() < cfg_.mid_move_prob) {
        mid_ += (below(2) == 0) ? 1 : -1;
        mid_ = std::clamp(mid_, cfg_.min_mid, cfg_.max_mid);
    }

    const double roll = uniform01();
    if (roll < cfg_.p_limit) return draw_limit(ts);

    if (roll < cfg_.p_limit + cfg_.p_cancel) {
        if (live_.empty()) return draw_limit(ts);  // nothing resting to cancel yet
        const OrderId id = live_[static_cast<std::size_t>(below(live_.size()))];
        return Event{ts, EventType::Cancel, id, EventSide::None, 0, 0};
    }

    const bool     buy = below(2) == 0;
    const Quantity qty = random_qty();
    return Event{ts, EventType::Market, issue_id(), buy ? EventSide::Buy : EventSide::Sell, 0, qty};
}

void Generator::settle(const std::vector<Trade>& trades) {
    for (const Trade& t : trades) {
        for (const OrderId id : {t.buy_id, t.sell_id}) {
            if (live_pos_[id] == kNotLive) continue;  // e.g. the incoming market order
            remaining_[id] -= t.qty;
            if (remaining_[id] == 0) untrack(id);
        }
    }
}

void Generator::apply(const Event& e) {
    const Order o{e.id, to_book_side(e.side), e.price, e.qty, e.ts};
    switch (e.type) {
        case EventType::Add:
            track(e.id, e.qty);  // settle() removes it again if it fills on arrival
            settle(book_.add_limit(o).trades);
            break;
        case EventType::Market: settle(book_.add_market(o).trades); break;
        case EventType::Cancel:
            book_.cancel(e.id);
            untrack(e.id);
            break;
        case EventType::Modify: break;  // never generated
    }
}

Event Generator::next() {
    const Event e = draw();
    apply(e);
    return e;
}

std::vector<Event> generate(const GenConfig& config) {
    Generator          gen(config);
    std::vector<Event> events;
    events.reserve(config.num_events);
    for (std::size_t i = 0; i < config.num_events; ++i) events.push_back(gen.next());
    return events;
}

}  // namespace ob
