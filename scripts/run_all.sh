#!/usr/bin/env bash
# One command: build (Release), run all tests, benchmark, backtest, plot.
#
#   scripts/run_all.sh [--events N] [--bench-events N] [--seed S] [--pin CORE]
#                      [--skip-bench]
#
# Outputs go to out/ (git-ignored): events.csv, trades.csv, snapshots.csv,
# fills.csv, pnl.csv, latency_hist.csv, bench.txt and the PNGs from plots.py.
# Plots need matplotlib (apt install python3-matplotlib); without it the run
# still finishes and says the plot step was skipped.
set -euo pipefail

EVENTS=200000        # events for the backtest stream
BENCH_EVENTS=1000000 # events for ob_bench
SEED=1
PIN=""
SKIP_BENCH=0

usage() { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --events)       EVENTS="$2"; shift 2 ;;
    --bench-events) BENCH_EVENTS="$2"; shift 2 ;;
    --seed)         SEED="$2"; shift 2 ;;
    --pin)          PIN="$2"; shift 2 ;;
    --skip-bench)   SKIP_BENCH=1; shift ;;
    -h|--help)      usage ;;
    *) echo "run_all.sh: unknown option $1" >&2; usage ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD=build
OUT=out
mkdir -p "$OUT"

step() { printf '\n== %s\n' "$*"; }

step "Configure + build (Release)"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j

step "C++ tests"
ctest --test-dir "$BUILD" --output-on-failure

step "Python tests"
OB_BUILD_DIR="$BUILD" python3 -m unittest discover -s python/tests

if [[ $SKIP_BENCH -eq 0 ]]; then
  step "Benchmark ($BENCH_EVENTS events, seed $SEED)"
  pin_args=()
  [[ -n "$PIN" ]] && pin_args=(--pin "$PIN")
  "$BUILD/ob_bench" --events "$BENCH_EVENTS" --seed "$SEED" "${pin_args[@]}" \
    --latency-hist "$OUT/latency_hist.csv" | tee "$OUT/bench.txt"
fi

step "Generate $EVENTS events + backtest"
"$BUILD/ob_gen" --events "$EVENTS" --seed "$SEED" --out "$OUT/events.csv"
python3 python/backtest.py --events "$OUT/events.csv" --out-dir "$OUT" --build-dir "$BUILD"

step "Plots"
if python3 -c "import matplotlib" 2>/dev/null; then
  python3 python/plots.py --out-dir "$OUT"
else
  echo "matplotlib not installed; skipped (apt install python3-matplotlib)"
fi

step "Done. Results in $OUT/"
