// ob_bench: throughput and per-event latency of OrderBook on a generated stream.
//
//   ob_bench [--events N] [--seed S] [--warmup N] [--repeat R] [--pin CORE]
//
// Method:
//   1. Generate the stream up front (not timed). Same seed, same stream.
//   2. Warm up on a throwaway book over the first --warmup events.
//   3. Throughput: replay the whole stream R times with no per-event timing,
//      report the median run (min/max shown).
//   4. Latency: one more replay with a clock read before and after every event.
//      Subtract the measured clock-read overhead, then report p50/p99/p99.9/max.
//
// Only Release builds run (NDEBUG). Exit code: 0 ok, 1 pinning failed,
// 2 bad usage or non-Release build.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "event.hpp"
#include "generator.hpp"
#include "order_book.hpp"

#ifdef __linux__
#include <sched.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::size_t   num_events{1'000'000};
    std::uint64_t seed{1};
    std::size_t   warmup{100'000};
    std::size_t   repeat{3};
    int           pin_core{-1};  // -1: not pinned
};

// Running totals. Also acts as the sink that stops the optimizer removing work.
struct Totals {
    std::uint64_t trades{};
    std::int64_t  traded_qty{};
    std::uint64_t rejected{};
};

inline void apply(ob::OrderBook& book, const ob::Event& e, Totals& totals) {
    ob::AddResult result;
    ob::Status    status = ob::Status::Ok;
    switch (e.type) {
        case ob::EventType::Add:
            result = book.add_limit(ob::Order{e.id, ob::to_book_side(e.side), e.price, e.qty, e.ts});
            status = result.status;
            break;
        case ob::EventType::Market:
            result = book.add_market(ob::Order{e.id, ob::to_book_side(e.side), 0, e.qty, e.ts});
            status = result.status;
            break;
        case ob::EventType::Cancel:
            status = book.cancel(e.id);
            break;
        case ob::EventType::Modify:
            status = book.modify(e.id, e.qty);
            break;
    }
    if (status != ob::Status::Ok) ++totals.rejected;
    totals.trades += result.trades.size();
    for (const ob::Trade& t : result.trades) totals.traded_qty += t.qty;
}

std::uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Median gap between back-to-back clock reads: the cost of one Clock::now() call.
std::uint64_t clock_read_overhead_ns() {
    constexpr std::size_t kSamples = 1'000'000;
    std::vector<std::uint64_t> gaps;
    gaps.reserve(kSamples);
    auto prev = Clock::now();
    for (std::size_t i = 0; i < kSamples; ++i) {
        const auto now = Clock::now();
        gaps.push_back(ns_between(prev, now));
        prev = now;
    }
    std::nth_element(gaps.begin(), gaps.begin() + kSamples / 2, gaps.end());
    return gaps[kSamples / 2];
}

// Replays the whole stream on a fresh book. Returns seconds elapsed.
double run_throughput(const std::vector<ob::Event>& events, Totals& totals) {
    ob::OrderBook book;
    const auto    t0 = Clock::now();
    for (const ob::Event& e : events) apply(book, e, totals);
    const auto t1 = Clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

// Replays the stream with each event timed. Returns per-event latency in ns.
std::vector<std::uint64_t> run_latency(const std::vector<ob::Event>& events,
                                       std::uint64_t overhead_ns, Totals& totals) {
    ob::OrderBook              book;
    std::vector<std::uint64_t> samples;
    samples.reserve(events.size());
    for (const ob::Event& e : events) {
        const auto t0 = Clock::now();
        apply(book, e, totals);
        const auto t1 = Clock::now();
        const std::uint64_t raw = ns_between(t0, t1);
        samples.push_back(raw > overhead_ns ? raw - overhead_ns : 0);
    }
    return samples;
}

// Nearest-rank percentile on an already sorted, non-empty vector.
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double p) {
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

bool pin_to_core(int core) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void)core;
    return false;  // pinning is implemented on Linux only
#endif
}

void usage() {
    std::fprintf(stderr,
                 "usage: ob_bench [--events N] [--seed S] [--warmup N] [--repeat R] [--pin CORE]\n");
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (!std::strcmp(argv[i], "--events") && has_value) {
            opt.num_events = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--seed") && has_value) {
            opt.seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--warmup") && has_value) {
            opt.warmup = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--repeat") && has_value) {
            opt.repeat = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--pin") && has_value) {
            opt.pin_core = std::atoi(argv[++i]);
        } else {
            return false;
        }
    }
    return opt.num_events > 0 && opt.repeat > 0 && opt.pin_core >= -1;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef NDEBUG
    std::fprintf(stderr,
                 "ob_bench: refusing to run a non-Release build (NDEBUG not defined).\n"
                 "          configure with -DCMAKE_BUILD_TYPE=Release\n");
    return 2;
#endif

    Options opt;
    if (!parse_args(argc, argv, opt)) {
        usage();
        return 2;
    }

    if (opt.pin_core >= 0 && !pin_to_core(opt.pin_core)) {
        std::fprintf(stderr, "ob_bench: could not pin to core %d (pinning needs Linux)\n",
                     opt.pin_core);
        return 1;
    }

    ob::GenConfig cfg;
    cfg.seed       = opt.seed;
    cfg.num_events = opt.num_events;
    const std::vector<ob::Event> events = ob::generate(cfg);

    Totals sink;  // accumulates from warmup too, printed at the end
    {
        ob::OrderBook book;
        const std::size_t n = std::min(opt.warmup, events.size());
        for (std::size_t i = 0; i < n; ++i) apply(book, events[i], sink);
    }

    const std::uint64_t overhead = clock_read_overhead_ns();

    std::vector<double> seconds;
    seconds.reserve(opt.repeat);
    for (std::size_t r = 0; r < opt.repeat; ++r) {
        Totals t;
        seconds.push_back(run_throughput(events, t));
        sink.trades += t.trades;
        sink.traded_qty += t.traded_qty;
        sink.rejected += t.rejected;
    }
    std::sort(seconds.begin(), seconds.end());
    const double median_s = seconds[seconds.size() / 2];
    const double n_events = static_cast<double>(events.size());

    Totals lat_totals;
    auto   samples = run_latency(events, overhead, lat_totals);
    sink.trades += lat_totals.trades;
    sink.traded_qty += lat_totals.traded_qty;
    sink.rejected += lat_totals.rejected;

    std::sort(samples.begin(), samples.end());
    std::uint64_t sum = 0;
    for (std::uint64_t s : samples) sum += s;

    std::printf("build:      %s, %s\n", "Release", __VERSION__);
    std::printf("stream:     %zu events, seed %llu, warmup %zu, pinned core: %s\n",
                events.size(), static_cast<unsigned long long>(opt.seed), opt.warmup,
                opt.pin_core >= 0 ? std::to_string(opt.pin_core).c_str() : "none");
    std::printf("clock:      %llu ns per clock read (median back-to-back gap, subtracted)\n",
                static_cast<unsigned long long>(overhead));
    std::printf("throughput: %.3f M events/s median of %zu runs (min %.3f, max %.3f)\n",
                n_events / median_s / 1e6, opt.repeat, n_events / seconds.back() / 1e6,
                n_events / seconds.front() / 1e6);
    std::printf("latency ns: p50 %llu  p99 %llu  p99.9 %llu  max %llu  mean %.1f\n",
                static_cast<unsigned long long>(percentile(samples, 0.50)),
                static_cast<unsigned long long>(percentile(samples, 0.99)),
                static_cast<unsigned long long>(percentile(samples, 0.999)),
                static_cast<unsigned long long>(samples.back()),
                static_cast<double>(sum) / static_cast<double>(samples.size()));
    std::printf("checksum:   trades %llu, traded qty %lld, rejected %llu\n",
                static_cast<unsigned long long>(sink.trades),
                static_cast<long long>(sink.traded_qty),
                static_cast<unsigned long long>(sink.rejected));
    return 0;
}
