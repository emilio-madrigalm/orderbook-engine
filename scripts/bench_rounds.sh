#!/usr/bin/env bash
# Re-measure every optimization round in one session, for the README table.
#
#   scripts/bench_rounds.sh [--events N] [--passes P] [--repeat R] [--pin CORE]
#                           [--profiles "default deep"] [--table-only]
#
# --table-only rebuilds table.md from an existing out/rounds/runs.csv.
#
# Why: on a laptop/WSL2 machine absolute speed drifts between sessions by
# 30% or more, so numbers recorded on different days are not comparable.
# This script builds the book exactly as it was at each round's commit and
# runs them interleaved (the starting round rotates each pass), then reports
# the median over passes. Because rounds in one pass run minutes apart, the
# table also compares each round with the previous one pass by pass: the
# median per-pass speed ratio, and in how many passes it was faster.
#
# Every round is built with the CURRENT bench/bench.cpp and src/generator.hpp
# (the harness and the stream are held fixed; only the book changes), using
# g++ -std=c++20 -O3 -DNDEBUG, the same flags as the CMake Release build.
# Checksums (trade count, traded qty, rejects) must match across rounds on
# each profile, or the script fails: every round has to do the same work.
#
# Output: out/rounds/runs.csv (every run) and out/rounds/table.md.
set -euo pipefail

EVENTS=1000000
PASSES=5
REPEAT=3
PIN=2
PROFILES="default deep"
TABLE_ONLY=0

usage() { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --events)   EVENTS="$2"; shift 2 ;;
    --passes)   PASSES="$2"; shift 2 ;;
    --repeat)   REPEAT="$2"; shift 2 ;;
    --pin)      PIN="$2"; shift 2 ;;
    --profiles) PROFILES="$2"; shift 2 ;;
    --table-only) TABLE_ONLY=1; shift ;;
    -h|--help)  usage ;;
    *) echo "bench_rounds.sh: unknown option $1" >&2; usage ;;
  esac
done

# name:commit. The commit is where that round's book landed on main.
ROUNDS=(
  "r0 baseline (std::map + std::list):94169f2"
  "r1 node pool allocator:474503e"
  "r2 intrusive level queues:4264a6e"
  "r3 flat price-tick arrays:b9d6b3a"
  "r4 open-addressing id index:dd24a5e"
  "r5 32-byte order node:9a00ca9"
)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
WORK="$ROOT/build-rounds"
OUT="$ROOT/out/rounds"
CXX="${CXX:-g++}"
mkdir -p "$WORK/bin" "$OUT"

CSV="$OUT/runs.csv"
n=${#ROUNDS[@]}

if [[ $TABLE_ONLY -eq 0 ]]; then
echo "== Building $n rounds with $CXX"
for i in "${!ROUNDS[@]}"; do
  sha="${ROUNDS[$i]##*:}"
  src="$WORK/src-r$i"
  rm -rf "$src" && mkdir -p "$src"
  git archive "$sha" src bench | tar -x -C "$src"
  cp bench/bench.cpp "$src/bench/bench.cpp"
  cp src/generator.hpp "$src/src/generator.hpp"
  "$CXX" -std=c++20 -O3 -DNDEBUG -Wall -Wextra -I"$src/src" \
    "$src"/src/order_book.cpp "$src"/src/event.cpp "$src"/src/generator.cpp \
    "$src"/bench/bench.cpp -o "$WORK/bin/ob_bench-r$i"
  echo "   r$i  $sha  ${ROUNDS[$i]%:*}"
done

echo "pass,profile,round,mevents_s,p50_ns,p99_ns,p999_ns,checksum" > "$CSV"
echo "== Running $PASSES passes x ${n} rounds x profiles [$PROFILES], $EVENTS events, core $PIN"
for ((p = 0; p < PASSES; p++)); do
  for profile in $PROFILES; do
    for ((k = 0; k < n; k++)); do
      i=$(( (p + k) % n ))  # rotate the starting round each pass
      res=$("$WORK/bin/ob_bench-r$i" --events "$EVENTS" --repeat "$REPEAT" \
              --pin "$PIN" --profile "$profile")
      mops=$(sed -n 's/^throughput: *\([0-9.]*\) M.*/\1/p' <<<"$res")
      read -r p50 p99 p999 < <(sed -n \
        's/^latency ns: p50 \([0-9]*\) *p99 \([0-9]*\) *p99.9 \([0-9]*\).*/\1 \2 \3/p' <<<"$res")
      sum=$(sed -n 's/^checksum: *//p' <<<"$res" | tr -d ',')
      echo "$p,$profile,r$i,$mops,$p50,$p99,$p999,$sum" >> "$CSV"
    done
  done
  echo "   pass $((p + 1))/$PASSES done"
done
fi
[[ -f "$CSV" ]] || { echo "bench_rounds.sh: no $CSV; run without --table-only first" >&2; exit 1; }

names=$(printf '%s\n' "${ROUNDS[@]}")
python3 - "$CSV" "$OUT/table.md" "$names" <<'PY'
import csv, statistics, sys
from collections import defaultdict

runs_csv, table_md, names = sys.argv[1], sys.argv[2], sys.argv[3].splitlines()
rows = list(csv.DictReader(open(runs_csv)))
cell = {(r["pass"], r["profile"], r["round"]): r for r in rows}
passes = sorted(dict.fromkeys(r["pass"] for r in rows), key=int)
med = statistics.median

lines = []
for profile in dict.fromkeys(r["profile"] for r in rows):
    sums = {r["checksum"] for r in rows if r["profile"] == profile}
    if len(sums) != 1:
        sys.exit(f"checksum mismatch on profile {profile}: {sums}")
    lines += [f"Profile `{profile}`, median of {len(passes)} passes", "",
              "| Round | M events/s | range | vs r0 | vs previous round | faster than previous "
              "| p50 ns | p99 ns | p99.9 ns |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    speed = lambda p, i: float(cell[(p, profile, f"r{i}")]["mevents_s"])
    base = med(speed(p, 0) for p in passes)
    for i, label in enumerate(names):
        m = [speed(p, i) for p in passes]
        g = [cell[(p, profile, f"r{i}")] for p in passes]
        q = {k: int(med(int(x[k]) for x in g)) for k in ("p50_ns", "p99_ns", "p999_ns")}
        if i == 0:
            prev, wins = "-", "-"
        else:  # same pass, so the same machine state
            ratios = [speed(p, i) / speed(p, i - 1) for p in passes]
            prev = f"{med(ratios):.2f}x"
            wins = f"{sum(r > 1 for r in ratios)}/{len(passes)} passes"
        name = label.rsplit(":", 1)[0]
        lines.append(f"| {name} | {med(m):.1f} | {min(m):.1f}-{max(m):.1f} | {med(m) / base:.2f}x "
                     f"| {prev} | {wins} | {q['p50_ns']} | {q['p99_ns']} | {q['p999_ns']} |")
    lines.append("")
open(table_md, "w").write("\n".join(lines))
print("\n".join(lines))
PY
echo "== Wrote $CSV and $OUT/table.md"
