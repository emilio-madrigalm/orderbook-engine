# orderbook-engine

High-performance C++20 limit order book with price-time priority matching, latency benchmarks, and a Python market-making backtester.

**Status:** in progress. The scaffold is in place and the matching engine is next.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires a C++20 compiler (GCC 11+ or Clang 14+), CMake 3.20+ and Git.
