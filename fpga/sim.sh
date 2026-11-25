#!/usr/bin/env bash
# Simulate fpga/rtl/msg_parser.sv against the C++ reference model.
#
#   fpga/sim.sh [--events N] [--corrupt-every K] [--build-dir DIR]
#
# 1. ob_gen makes N synthetic events, ob_wire encodes them (every Kth message
#    gets one random byte replaced, to exercise the error paths), after a fixed
#    set of boundary messages (ob::edge_cases: exact price bounds, every code)
# 2. ob_wire decode writes the expected parser output
# 3. Icarus Verilog runs the testbench (with random input bubbles) and writes the
#    hardware's output
# 4. the two outputs must be identical, line for line, and the hardware's
#    counters must match counts taken from the expected output
# Needs: iverilog (and verilator for the lint step, skipped if missing).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EVENTS=100000
CORRUPT=50
BUILD="$ROOT/build"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --events) EVENTS="$2"; shift 2 ;;
        --corrupt-every) CORRUPT="$2"; shift 2 ;;
        --build-dir) BUILD="$2"; shift 2 ;;
        *) echo "usage: $0 [--events N] [--corrupt-every K] [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

command -v iverilog >/dev/null || { echo "sim.sh: iverilog not found (sudo apt install iverilog)" >&2; exit 1; }
for b in ob_gen ob_wire; do
    [[ -x "$BUILD/$b" ]] || { echo "sim.sh: $BUILD/$b missing; build the project first" >&2; exit 1; }
done

OUT="$ROOT/out/fpga"
mkdir -p "$OUT"
RTL="$ROOT/fpga/rtl/msg_parser.sv"
TB="$ROOT/fpga/tb/tb_msg_parser.sv"

if command -v verilator >/dev/null; then
    verilator --lint-only -Wall "$RTL" --top-module msg_parser
    echo "lint: verilator -Wall clean"
else
    echo "lint: verilator not found, skipped"
fi

"$BUILD/ob_gen" --events "$EVENTS" --seed 1 --out "$OUT/events.csv" >/dev/null
"$BUILD/ob_wire" encode --in "$OUT/events.csv" --out "$OUT/msgs.hex" --corrupt-every "$CORRUPT" --seed 7 --edge-cases
"$BUILD/ob_wire" decode --in "$OUT/msgs.hex" --out "$OUT/expected.csv"

iverilog -g2012 -Wall -o "$OUT/sim.vvp" "$RTL" "$TB"
vvp -n "$OUT/sim.vvp" +IN="$OUT/msgs.hex" +OUT="$OUT/got.csv" +COUNTERS="$OUT/counters.txt" +GAPS=1 \
    | tee "$OUT/sim.log"
grep -q "tb: PASS" "$OUT/sim.log" || { echo "sim.sh: testbench reported FAIL" >&2; exit 1; }

if ! cmp -s "$OUT/expected.csv" "$OUT/got.csv"; then
    echo "sim.sh: hardware output differs from the C++ model; first differences:" >&2
    diff "$OUT/expected.csv" "$OUT/got.csv" | head -10 >&2
    exit 1
fi
echo "diff: $(wc -l < "$OUT/got.csv") decoded messages identical to ob_wire decode"

# Counters vs counts from the expected output (err,ts,type,id,side,price,qty).
awk -F, '{ n++; if ($1 != 0) e++; else { if ($3 == 65) a++; if ($3 == 77) m++; if ($3 == 67) c++; if ($3 == 88) x++ } }
         END { printf "msgs %d\nerrors %d\nadd %d\nmarket %d\ncancel %d\nmodify %d\n", n, e, a, m, c, x }' \
    "$OUT/expected.csv" > "$OUT/counters_expected.txt"
if ! cmp -s "$OUT/counters_expected.txt" "$OUT/counters.txt"; then
    echo "sim.sh: counters differ" >&2
    diff "$OUT/counters_expected.txt" "$OUT/counters.txt" >&2
    exit 1
fi
echo "counters: match ($(tr '\n' ' ' < "$OUT/counters.txt"))"
