#pragma once

// Synthetic order-flow generator: Poisson arrivals, random-walk mid price,
// ~60% limit / 30% cancel / 10% market. Fully determined by GenConfig, so the
// same seed always yields the same stream.
//
// To only ever cancel orders that are still resting, the generator feeds its
// own events through an internal OrderBook and tracks fills. The stream
// therefore depends on the matching rules (price-time priority) but not on how
// the book is implemented, so it is unchanged by optimization rounds.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "event.hpp"
#include "order_book.hpp"

namespace ob {

struct GenConfig {
    std::uint64_t seed{1};
    std::size_t   num_events{1'000'000};

    double rate_per_sec{1'000'000.0};  // mean arrival rate (Poisson => exponential gaps)

    Price start_mid{10'000};   // ticks
    Price min_mid{1'000};      // random walk is clamped to [min_mid, max_mid]
    Price max_mid{100'000};
    double mid_move_prob{0.05};  // chance the mid steps +/-1 tick on any event

    double p_limit{0.60};
    double p_cancel{0.30};      // remainder (1 - p_limit - p_cancel) is market orders
    double p_aggressive{0.05};  // share of limit orders priced to cross the spread
    double offset_p{0.30};      // geometric(p) distance from mid, in ticks (mean ~ 1/p)

    Quantity max_qty{100};      // quantities are uniform in [1, max_qty]
};

class Generator {
public:
    explicit Generator(const GenConfig& config);

    // Next event in the stream. Callers decide how many to take.
    Event next();

    std::size_t resting_orders() const { return book_.order_count(); }

private:
    static constexpr std::uint32_t kNotLive = UINT32_MAX;

    // Small self-contained generator (splitmix64) so output does not depend on
    // the standard library's distribution implementations.
    struct SplitMix64 {
        std::uint64_t state;
        std::uint64_t next() {
            std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
            z               = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z               = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }
    };

    std::uint64_t raw() { return rng_.next(); }
    double        uniform01();
    std::uint64_t below(std::uint64_t n);
    Quantity      random_qty();

    OrderId issue_id();                // allocates the next id and its tracking slots
    Event   draw();                    // choose the next event (no book update)
    Event   draw_limit(Timestamp ts);
    void    apply(const Event& e);     // feed the internal book, track fills
    void    settle(const std::vector<Trade>& trades);
    void    track(OrderId id, Quantity qty);
    void    untrack(OrderId id);

    GenConfig  cfg_;
    SplitMix64 rng_;
    double     clock_ns_{0.0};
    Price      mid_;

    OrderBook book_;
    OrderId   next_id_{1};

    // Indexed by order id (slot 0 unused). A limit order is "live" while it rests.
    std::vector<Quantity>      remaining_;
    std::vector<std::uint32_t> live_pos_;  // index into live_, or kNotLive
    std::vector<OrderId>       live_;
};

std::vector<Event> generate(const GenConfig& config);

}  // namespace ob
