// ob_replay: feed a CSV event stream through the book, write trades/snapshots CSV.
//   ob_replay --in data/sample.csv --trades out/trades.csv --snapshots out/snaps.csv
//             [--snapshot-every N] [--depth D]
// Exit code: 0 ok, 1 I/O error or malformed input lines, 2 bad usage.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include "replayer.hpp"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: ob_replay --in FILE [--trades FILE] [--snapshots FILE]\n"
                 "                 [--snapshot-every N] [--depth D]\n");
}

}  // namespace

int main(int argc, char** argv) {
    const char*    in_path     = nullptr;
    const char*    trades_path = nullptr;
    const char*    snaps_path  = nullptr;
    ob::ReplayOptions opts;

    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (!std::strcmp(argv[i], "--in") && has_value) {
            in_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--trades") && has_value) {
            trades_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--snapshots") && has_value) {
            snaps_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--snapshot-every") && has_value) {
            opts.snapshot_every = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--depth") && has_value) {
            opts.depth = std::strtoull(argv[++i], nullptr, 10);
        } else {
            usage();
            return 2;
        }
    }
    if (!in_path) {
        usage();
        return 2;
    }

    std::ifstream in(in_path);
    if (!in) {
        std::fprintf(stderr, "ob_replay: cannot open %s\n", in_path);
        return 1;
    }
    std::ofstream trades, snaps;
    if (trades_path) {
        trades.open(trades_path);
        if (!trades) {
            std::fprintf(stderr, "ob_replay: cannot write %s\n", trades_path);
            return 1;
        }
    }
    if (snaps_path) {
        snaps.open(snaps_path);
        if (!snaps) {
            std::fprintf(stderr, "ob_replay: cannot write %s\n", snaps_path);
            return 1;
        }
    }

    const ob::ReplayStats s =
        ob::replay(in, trades_path ? &trades : nullptr, snaps_path ? &snaps : nullptr, opts);

    std::printf(
        "events=%llu adds=%llu markets=%llu cancels=%llu modifies=%llu trades=%llu "
        "traded_qty=%lld rejected=%llu bad_lines=%llu\n",
        static_cast<unsigned long long>(s.events), static_cast<unsigned long long>(s.adds),
        static_cast<unsigned long long>(s.markets), static_cast<unsigned long long>(s.cancels),
        static_cast<unsigned long long>(s.modifies), static_cast<unsigned long long>(s.trades),
        static_cast<long long>(s.traded_qty), static_cast<unsigned long long>(s.rejected),
        static_cast<unsigned long long>(s.bad_lines));

    if (s.bad_lines > 0) {
        std::fprintf(stderr, "ob_replay: %llu malformed line(s), first at line %llu\n",
                     static_cast<unsigned long long>(s.bad_lines),
                     static_cast<unsigned long long>(s.first_bad_line));
        return 1;
    }
    return 0;
}
