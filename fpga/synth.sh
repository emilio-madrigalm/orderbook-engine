#!/usr/bin/env bash
# Synthesis and timing for msg_parser (no board: these are tool reports only).
#
#   fpga/synth.sh [--freq MHZ]
#
# 1. Yosys generic synthesis of msg_parser alone: cell counts
# 2. Yosys synth_ice40 + nextpnr-ice40 (iCE40 HX8K) on fpga/rtl/timing_wrap.sv:
#    LUT/FF use and the max clock frequency nextpnr reports after place and route
# Needs: yosys; nextpnr-ice40 for step 2 (skipped if missing).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FREQ=140
while [[ $# -gt 0 ]]; do
    case "$1" in
        --freq) FREQ="$2"; shift 2 ;;
        *) echo "usage: $0 [--freq MHZ]" >&2; exit 2 ;;
    esac
done
command -v yosys >/dev/null || { echo "synth.sh: yosys not found (sudo apt install yosys)" >&2; exit 1; }

OUT="$ROOT/out/fpga"
mkdir -p "$OUT"
RTL="$ROOT/fpga/rtl"

yosys -q -l "$OUT/yosys_generic.log" -p \
    "read_verilog -sv $RTL/msg_parser.sv; synth -top msg_parser; stat" >/dev/null
echo "generic synthesis (msg_parser): $OUT/yosys_generic.log"
sed -n '/Number of cells/,/^$/p' "$OUT/yosys_generic.log" | tail -n +1 | head -25

yosys -q -l "$OUT/yosys_ice40.log" -p \
    "read_verilog -sv $RTL/msg_parser.sv $RTL/timing_wrap.sv; synth_ice40 -top timing_wrap -json $OUT/timing_wrap.json" \
    >/dev/null
echo "iCE40 synthesis (timing_wrap): $OUT/yosys_ice40.log"

if command -v nextpnr-ice40 >/dev/null; then
    nextpnr-ice40 --hx8k --package ct256 --json "$OUT/timing_wrap.json" --freq "$FREQ" \
        --pcf-allow-unconstrained --seed 1 --log "$OUT/nextpnr.log" >/dev/null 2>&1 || true
    grep -E "ICESTORM_LC:" "$OUT/nextpnr.log" | tail -1
    # The last line is the post-route figure; earlier ones are placement estimates.
    grep -E "Max frequency for clock" "$OUT/nextpnr.log" | tail -1 | sed 's/^ERROR: /Info: /'
else
    echo "nextpnr-ice40 not found: no timing report (sudo apt install nextpnr-ice40)"
fi
