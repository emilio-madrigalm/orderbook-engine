// ob_itch: pull one symbol out of a Nasdaq TotalView-ITCH 5.0 file as an event stream.
//
//   zcat 01302020.NASDAQ_ITCH50.gz | ob_itch --symbol AAPL --out-dir out/itch
//   ob_itch --symbol AAPL --in day.itch --out-dir out/itch [--depth 5] [--snapshot-every 1]
//
// Writes into --out-dir:
//   events.csv          our event schema (event.hpp), ready for ob_replay
//   expected_fills.csv  market_id,resting_id,price,qty,match: the order Nasdaq executed
//   expected_depth.csv  the book built straight from ITCH, in ob_replay's snapshot format
//   itch_meta.txt       symbol, depth, snapshot_every and the counts below, key=value
// The input is the uncompressed stream; pipe the .gz through zcat.
// Exit code: 0 ok, 1 I/O error, bad stream or symbol not found, 2 bad usage.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "itch.hpp"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: ob_itch --symbol SYM --out-dir DIR [--in FILE] [--depth D] [--snapshot-every N]\n"
                 "       (reads the uncompressed ITCH 5.0 stream from stdin when --in is absent)\n");
}

}  // namespace

int main(int argc, char** argv) {
    ob::itch::Options opt;
    const char*       in_path = nullptr;
    const char*       out_dir = nullptr;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (!std::strcmp(argv[i], "--symbol") && has_value) {
            opt.symbol = argv[++i];
        } else if (!std::strcmp(argv[i], "--in") && has_value) {
            in_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--out-dir") && has_value) {
            out_dir = argv[++i];
        } else if (!std::strcmp(argv[i], "--depth") && has_value) {
            opt.depth = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--snapshot-every") && has_value) {
            opt.snapshot_every = std::strtoull(argv[++i], nullptr, 10);
        } else {
            usage();
            return 2;
        }
    }
    if (opt.symbol.empty() || opt.symbol.size() > 8 || !out_dir || opt.depth == 0) {
        usage();
        return 2;
    }

    std::ifstream file;
    std::istream* in = &std::cin;
    if (in_path) {
        file.open(in_path, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "ob_itch: cannot open %s\n", in_path);
            return 1;
        }
        in = &file;
    } else {
        std::ios::sync_with_stdio(false);
    }

    const std::filesystem::path dir(out_dir);
    std::error_code             ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream events(dir / "events.csv"), fills(dir / "expected_fills.csv"),
        depth(dir / "expected_depth.csv"), meta(dir / "itch_meta.txt");
    if (!events || !fills || !depth || !meta) {
        std::fprintf(stderr, "ob_itch: cannot write into %s\n", out_dir);
        return 1;
    }

    ob::itch::Converter conv(opt, events, fills, &depth);
    std::string         error;
    if (!ob::itch::read_stream(*in, conv, &error)) {
        std::fprintf(stderr, "ob_itch: %s\n", error.c_str());
        return 1;
    }
    if (!conv.symbol_found()) {
        std::fprintf(stderr, "ob_itch: symbol %s not in the stock directory\n", opt.symbol.c_str());
        return 1;
    }

    const ob::itch::Stats& s = conv.stats();
    char                   line[1024];
    std::snprintf(line, sizeof line,
                  "symbol=%s locate=%u depth=%zu snapshot_every=%zu messages=%llu symbol_messages=%llu "
                  "adds=%llu executions=%llu executions_price=%llu cancels=%llu deletes=%llu "
                  "replaces=%llu hidden_trades=%llu cross_trades=%llu broken_trades=%llu "
                  "skipped_price=%llu skipped_followups=%llu unknown_refs=%llu overfills=%llu events=%llu",
                  opt.symbol.c_str(), static_cast<unsigned>(conv.locate()), opt.depth, opt.snapshot_every,
                  static_cast<unsigned long long>(s.messages), static_cast<unsigned long long>(s.symbol_messages),
                  static_cast<unsigned long long>(s.adds), static_cast<unsigned long long>(s.executions),
                  static_cast<unsigned long long>(s.executions_price), static_cast<unsigned long long>(s.cancels),
                  static_cast<unsigned long long>(s.deletes), static_cast<unsigned long long>(s.replaces),
                  static_cast<unsigned long long>(s.hidden_trades), static_cast<unsigned long long>(s.cross_trades),
                  static_cast<unsigned long long>(s.broken_trades), static_cast<unsigned long long>(s.skipped_price),
                  static_cast<unsigned long long>(s.skipped_followups),
                  static_cast<unsigned long long>(s.unknown_refs), static_cast<unsigned long long>(s.overfills),
                  static_cast<unsigned long long>(s.events));
    meta << line << '\n';
    std::printf("%s\n", line);
    events.flush();
    fills.flush();
    depth.flush();
    meta.flush();
    if (!events || !fills || !depth || !meta) {
        std::fprintf(stderr, "ob_itch: write failed\n");
        return 1;
    }
    return 0;
}
