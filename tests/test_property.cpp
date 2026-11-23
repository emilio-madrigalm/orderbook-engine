// Property test: OrderBook must agree with the naive ReferenceBook on long
// random operation streams. Every operation's result (status + trades) and the
// full observable book state are compared after each step.

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "order_book.hpp"
#include "reference_book.hpp"

namespace {

using ob::Order;
using ob::OrderId;
using ob::Price;
using ob::Quantity;
using ob::Side;
using ob::Status;
using ob::Trade;

constexpr int         kMaxQty       = 20;
constexpr std::size_t kCompareDepth = 100000;  // far above any possible level count

std::string describe(const std::vector<Trade>& trades) {
    std::ostringstream os;
    for (const Trade& t : trades)
        os << "[buy=" << t.buy_id << " sell=" << t.sell_id << " px=" << t.price
           << " qty=" << t.qty << "] ";
    return os.str();
}

bool same_trades(const std::vector<Trade>& a, const std::vector<Trade>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].buy_id != b[i].buy_id || a[i].sell_id != b[i].sell_id ||
            a[i].price != b[i].price || a[i].qty != b[i].qty)
            return false;
    }
    return true;
}

class Fuzzer {
public:
    Fuzzer(std::uint64_t seed, int min_price, int max_price)
        : rng_(seed), min_price_(min_price), max_price_(max_price) {}

    // Runs `ops` random operations. Returns an empty string on success, or a
    // description of the first divergence.
    std::string run(int ops) {
        for (int i = 0; i < ops; ++i) {
            std::string err = step(i);
            if (err.empty()) err = compare_state(i);
            if (!err.empty()) return err;
        }
        return {};
    }

private:
    int uniform(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }

    Side     random_side() { return uniform(0, 1) == 0 ? Side::Buy : Side::Sell; }
    Price    random_price() { return uniform(min_price_, max_price_); }
    Quantity random_qty() { return uniform(1, kMaxQty); }

    OrderId random_issued_id() {
        return issued_[static_cast<std::size_t>(uniform(0, static_cast<int>(issued_.size()) - 1))];
    }

    // Mostly a fresh id; sometimes one already issued (duplicate or stale).
    OrderId id_for_new_order() {
        if (!issued_.empty() && uniform(0, 19) == 0) return random_issued_id();
        issued_.push_back(next_id_);
        return next_id_++;
    }

    // Mostly an issued id (may already be filled or cancelled); sometimes one never issued.
    OrderId id_for_existing_order() {
        if (issued_.empty() || uniform(0, 19) == 0) return next_id_ + 1000;
        return random_issued_id();
    }

    std::string step(int i) {
        const int          roll = uniform(0, 99);
        std::ostringstream what;

        if (roll < 55) {  // limit order, occasionally invalid
            Order     o{id_for_new_order(), random_side(), random_price(), random_qty(), 0};
            const int bad = uniform(0, 49);
            if (bad == 0) o.qty = uniform(-3, 0);
            if (bad == 1) o.price = uniform(-3, 0);
            what << "limit id=" << o.id << " side=" << (o.side == Side::Buy ? "B" : "S")
                 << " px=" << o.price << " qty=" << o.qty;
            return compare_add(i, what.str(), book_.add_limit(o), ref_.add_limit(o));
        }
        if (roll < 70) {  // market order, occasionally invalid qty
            Order o{next_id_++, random_side(), 0, random_qty() * uniform(1, 4), 0};
            if (uniform(0, 49) == 0) o.qty = uniform(-3, 0);
            what << "market id=" << o.id << " side=" << (o.side == Side::Buy ? "B" : "S")
                 << " qty=" << o.qty;
            return compare_add(i, what.str(), book_.add_market(o), ref_.add_market(o));
        }
        if (roll < 90) {  // cancel
            const OrderId id = id_for_existing_order();
            what << "cancel id=" << id;
            return compare_status(i, what.str(), book_.cancel(id), ref_.cancel(id));
        }
        // modify, occasionally invalid qty
        const OrderId id      = id_for_existing_order();
        Quantity      new_qty = random_qty();
        if (uniform(0, 29) == 0) new_qty = uniform(-3, 0);
        what << "modify id=" << id << " qty=" << new_qty;
        return compare_status(i, what.str(), book_.modify(id, new_qty), ref_.modify(id, new_qty));
    }

    static std::string fail(int i, const std::string& op, const std::string& detail) {
        std::ostringstream os;
        os << "op #" << i << " (" << op << "): " << detail;
        return os.str();
    }

    static std::string compare_add(int i, const std::string& op, const ob::AddResult& a,
                                   const ob::AddResult& b) {
        if (a.status != b.status) return fail(i, op, "status differs");
        if (!same_trades(a.trades, b.trades))
            return fail(i, op,
                        "trades differ: book=" + describe(a.trades) + " ref=" + describe(b.trades));
        return {};
    }

    static std::string compare_status(int i, const std::string& op, Status a, Status b) {
        if (a != b) return fail(i, op, "status differs");
        return {};
    }

    std::string compare_state(int i) const {
        if (book_.best_bid() != ref_.best_bid()) return fail(i, "state", "best_bid differs");
        if (book_.best_ask() != ref_.best_ask()) return fail(i, "state", "best_ask differs");
        if (book_.spread() != ref_.spread()) return fail(i, "state", "spread differs");
        if (book_.order_count() != ref_.order_count())
            return fail(i, "state", "order_count differs");
        if (book_.depth(Side::Buy, kCompareDepth) != ref_.depth(Side::Buy, kCompareDepth))
            return fail(i, "state", "bid depth differs");
        if (book_.depth(Side::Sell, kCompareDepth) != ref_.depth(Side::Sell, kCompareDepth))
            return fail(i, "state", "ask depth differs");
        // The book must never be left crossed.
        if (book_.best_bid() && book_.best_ask() && *book_.best_bid() >= *book_.best_ask())
            return fail(i, "state", "book is crossed");
        return {};
    }

    std::mt19937_64        rng_;
    int                    min_price_;
    int                    max_price_;
    ob::OrderBook          book_;
    ob::ref::ReferenceBook ref_;
    std::vector<OrderId>   issued_;
    OrderId                next_id_{1};
};

}  // namespace

// Wide-ish band: many price levels, regular crossing and sweeps.
TEST(Property, MatchesReferenceOnRandomStreams) {
    for (std::uint64_t seed = 1; seed <= 25; ++seed) {
        Fuzzer      fuzzer(seed, 95, 105);
        const auto  err = fuzzer.run(20000);
        ASSERT_TRUE(err.empty()) << "seed " << seed << ": " << err;
    }
}

// Very tight band: few levels, long FIFO queues, constant partial fills and
// priority-sensitive modifies.
TEST(Property, MatchesReferenceOnDeepQueues) {
    for (std::uint64_t seed = 100; seed < 110; ++seed) {
        Fuzzer      fuzzer(seed, 99, 101);
        const auto  err = fuzzer.run(20000);
        ASSERT_TRUE(err.empty()) << "seed " << seed << ": " << err;
    }
}
