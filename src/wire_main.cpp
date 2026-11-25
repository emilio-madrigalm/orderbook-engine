// ob_wire: convert the CSV event stream to the 32-byte wire format and back.
//
//   ob_wire encode --in events.csv --out msgs.hex [--corrupt-every N] [--seed S] [--edge-cases]
//   ob_wire decode --in msgs.hex   --out expected.csv
//
// encode writes one message per line as 64 hex digits ($readmemh order). With
// --corrupt-every N, every Nth message gets one random byte replaced (seeded), so
// the parser's error paths are exercised alongside valid traffic.
// --edge-cases first writes ob::edge_cases() (exact bounds, every error code).
// decode is the reference model the FPGA testbench output is diffed against.
// Exit code: 0 ok, 1 I/O or malformed input, 2 bad usage.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "wire.hpp"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: ob_wire encode --in EVENTS.csv --out MSGS.hex [--corrupt-every N] [--seed S] [--edge-cases]\n"
                 "       ob_wire decode --in MSGS.hex --out EXPECTED.csv\n");
}

struct XorShift {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

int run_encode(std::ifstream& in, std::ofstream& out, std::uint64_t corrupt_every, std::uint64_t seed,
               bool edge_cases) {
    XorShift rng{seed ? seed : 1};
    std::string line;
    std::uint64_t n = 0, lineno = 0, corrupted = 0, edges = 0;
    if (edge_cases) {
        for (const auto& m : ob::edge_cases()) out << ob::to_hex(m) << '\n';
        edges = ob::edge_cases().size();
    }
    while (std::getline(in, line)) {
        ++lineno;
        if (lineno == 1 && line == ob::kCsvHeader) continue;
        const auto ev = ob::parse_csv_line(line);
        if (!ev) {
            std::fprintf(stderr, "ob_wire: malformed CSV at line %llu\n", static_cast<unsigned long long>(lineno));
            return 1;
        }
        auto msg = ob::encode(*ev);
        if (!msg) {
            std::fprintf(stderr, "ob_wire: line %llu does not fit the wire format\n",
                         static_cast<unsigned long long>(lineno));
            return 1;
        }
        ++n;
        if (corrupt_every && n % corrupt_every == 0) {
            const std::uint64_t r = rng.next();
            (*msg)[r % ob::kWireSize] = static_cast<std::uint8_t>(r >> 8);
            ++corrupted;
        }
        out << ob::to_hex(*msg) << '\n';
    }
    std::printf("messages=%llu corrupted=%llu edge_cases=%llu\n", static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(corrupted), static_cast<unsigned long long>(edges));
    return out ? 0 : 1;
}

int run_decode(std::ifstream& in, std::ofstream& out) {
    std::string line;
    std::uint64_t n = 0, errors = 0, lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const auto msg = ob::from_hex(line);
        if (!msg) {
            std::fprintf(stderr, "ob_wire: bad hex at line %llu\n", static_cast<unsigned long long>(lineno));
            return 1;
        }
        const ob::Decoded d = ob::decode(*msg);
        errors += d.err != ob::WireError::None;
        ++n;
        out << ob::decoded_line(d) << '\n';
    }
    std::printf("messages=%llu errors=%llu\n", static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(errors));
    return out ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string mode = argv[1];
    const char* in_path = nullptr;
    const char* out_path = nullptr;
    std::uint64_t corrupt_every = 0, seed = 1;
    bool edge_cases = false;
    for (int i = 2; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (!std::strcmp(argv[i], "--in") && has_value) {
            in_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--out") && has_value) {
            out_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--corrupt-every") && has_value && mode == "encode") {
            corrupt_every = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--seed") && has_value && mode == "encode") {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--edge-cases") && mode == "encode") {
            edge_cases = true;
        } else {
            usage();
            return 2;
        }
    }
    if (!in_path || !out_path || (mode != "encode" && mode != "decode")) {
        usage();
        return 2;
    }
    std::ifstream in(in_path);
    if (!in) {
        std::fprintf(stderr, "ob_wire: cannot open %s\n", in_path);
        return 1;
    }
    std::ofstream out(out_path);
    if (!out) {
        std::fprintf(stderr, "ob_wire: cannot write %s\n", out_path);
        return 1;
    }
    return mode == "encode" ? run_encode(in, out, corrupt_every, seed, edge_cases) : run_decode(in, out);
}
