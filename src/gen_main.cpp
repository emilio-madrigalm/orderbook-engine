// ob_gen: write a synthetic event stream as CSV.
//   ob_gen --events 1000000 --seed 1 --out data/sample.csv
// With no --out, writes to stdout.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "event.hpp"
#include "generator.hpp"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: ob_gen [--events N] [--seed S] [--rate EVENTS_PER_SEC]\n"
                 "              [--profile default|deep] [--out FILE]\n");
}

}  // namespace

int main(int argc, char** argv) {
    ob::GenConfig cfg;
    const char*   out_path = nullptr;
    bool          deep     = false;

    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (!std::strcmp(argv[i], "--events") && has_value) {
            cfg.num_events = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--seed") && has_value) {
            cfg.seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--rate") && has_value) {
            cfg.rate_per_sec = std::strtod(argv[++i], nullptr);
            if (!(cfg.rate_per_sec > 0.0)) {
                std::fprintf(stderr, "ob_gen: --rate must be > 0\n");
                return 2;
            }
        } else if (!std::strcmp(argv[i], "--profile") && has_value) {
            const char* name = argv[++i];
            if (!std::strcmp(name, "deep")) {
                deep = true;
            } else if (!std::strcmp(name, "default")) {
                deep = false;
            } else {
                usage();
                return 2;
            }
        } else if (!std::strcmp(argv[i], "--out") && has_value) {
            out_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }

    if (deep) {
        const double rate = cfg.rate_per_sec;
        cfg               = ob::deep_book_config(cfg.seed, cfg.num_events);
        cfg.rate_per_sec  = rate;  // --rate still applies to either profile
    }

    std::FILE* out = out_path ? std::fopen(out_path, "w") : stdout;
    if (!out) {
        std::fprintf(stderr, "ob_gen: cannot open %s\n", out_path);
        return 1;
    }

    std::fprintf(out, "%.*s\n", static_cast<int>(ob::kCsvHeader.size()), ob::kCsvHeader.data());
    ob::Generator gen(cfg);
    for (std::size_t i = 0; i < cfg.num_events; ++i) {
        const std::string line = ob::to_csv_line(gen.next());
        std::fwrite(line.data(), 1, line.size(), out);
        std::fputc('\n', out);
    }

    if (out_path) std::fclose(out);
    return 0;
}
