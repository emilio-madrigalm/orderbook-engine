// msg_parser: decodes and validates the 32-byte event message (src/wire.hpp),
// one message per clock, two pipeline stages, latency 2 cycles.
//
//   stage 1: slice fields out of the bus word, evaluate each rule in parallel
//   stage 2: pick the first failed rule (type, side, price, qty, reserved),
//            register outputs, update per-type and error counters
//
// No backpressure: a market-data feed cannot be paused, so the parser must keep
// up at line rate. in_valid may drop at any cycle (bubbles pass through).
//
// Error codes match ob::WireError: 0 none, 1 type, 2 side, 3 price, 4 qty, 5 reserved.

`timescale 1ns / 1ps

module msg_parser #(
    parameter int MAX_PRICE = 131071
) (
    input  logic         clk,
    input  logic         rst,        // synchronous, active high
    input  logic         in_valid,
    input  logic [255:0] in_data,    // byte i = in_data[8*i +: 8]

    output logic         out_valid,
    output logic [2:0]   out_err,
    output logic [63:0]  out_ts,
    output logic [7:0]   out_type,
    output logic [63:0]  out_id,
    output logic [7:0]   out_side,
    output logic [31:0]  out_price,
    output logic [31:0]  out_qty,

    output logic [31:0]  cnt_msgs,
    output logic [31:0]  cnt_errors,
    output logic [31:0]  cnt_add,
    output logic [31:0]  cnt_market,
    output logic [31:0]  cnt_cancel,
    output logic [31:0]  cnt_modify
);
    localparam logic [7:0] CH_A = 8'h41, CH_M = 8'h4D, CH_C = 8'h43, CH_X = 8'h58;
    localparam logic [7:0] CH_B = 8'h42, CH_S = 8'h53, CH_DASH = 8'h2D;
    // When MAX_PRICE is 2^k - 1 (the default 131071 = 2^17 - 1), "price <= MAX_PRICE"
    // is "no bit above k is set": one wide NOR instead of a 32-bit comparator.
    // Any other MAX_PRICE falls back to the comparison.
    localparam logic [31:0] PRICE_MASK = 32'(MAX_PRICE);
    localparam bit PRICE_POW2 = ((PRICE_MASK & (PRICE_MASK + 32'd1)) == 32'd0);

    // ---- combinational field slicing (feeds stage 1) ----
    wire [7:0]  f_type  = in_data[7:0];
    wire [7:0]  f_side  = in_data[15:8];
    wire [15:0] f_rsv0  = in_data[31:16];
    wire [31:0] f_qty   = in_data[63:32];
    wire [63:0] f_ts    = in_data[127:64];
    wire [63:0] f_id    = in_data[191:128];
    wire [31:0] f_price = in_data[223:192];
    wire [31:0] f_rsv1  = in_data[255:224];

    wire is_add    = f_type == CH_A;
    wire is_market = f_type == CH_M;
    wire is_cancel = f_type == CH_C;
    wire is_modify = f_type == CH_X;
    wire sided     = is_add | is_market;

    wire type_ok  = is_add | is_market | is_cancel | is_modify;
    wire side_ok  = sided ? (f_side == CH_B || f_side == CH_S) : (f_side == CH_DASH);
    wire price_le = PRICE_POW2 ? ((f_price & ~PRICE_MASK) == 32'd0) : (f_price <= PRICE_MASK);
    wire price_ok = is_add ? (f_price != 32'd0 && price_le) : (f_price == 32'd0);
    wire qty_ok   = is_cancel ? (f_qty == 32'd0) : (f_qty != 32'd0);
    wire rsv_ok   = (f_rsv0 == 16'd0) && (f_rsv1 == 32'd0);
    wire all_ok   = type_ok & side_ok & price_ok & qty_ok & rsv_ok;

    // ---- stage 1 registers ----
    logic        s1_valid;
    logic [63:0] s1_ts, s1_id;
    logic [7:0]  s1_type, s1_side;
    logic [31:0] s1_price, s1_qty;
    logic        s1_type_ok, s1_side_ok, s1_price_ok, s1_qty_ok, s1_rsv_ok;
    logic        s1_add, s1_market, s1_cancel, s1_modify;
    logic        s1_ok;

    always_ff @(posedge clk) begin
        if (rst) begin
            s1_valid <= 1'b0;
        end else begin
            s1_valid <= in_valid;
        end
        // Data registers need no reset: they are only read when s1_valid is set.
        s1_ts       <= f_ts;
        s1_id       <= f_id;
        s1_type     <= f_type;
        s1_side     <= f_side;
        s1_price    <= f_price;
        s1_qty      <= f_qty;
        s1_type_ok  <= type_ok;
        s1_side_ok  <= side_ok;
        s1_price_ok <= price_ok;
        s1_qty_ok   <= qty_ok;
        s1_rsv_ok   <= rsv_ok;
        s1_add      <= is_add;
        s1_market   <= is_market;
        s1_cancel   <= is_cancel;
        s1_modify   <= is_modify;
        s1_ok       <= all_ok;
    end

    // ---- stage 2: first failed rule wins ----
    logic [2:0] s1_err;
    always_comb begin
        if (!s1_type_ok)       s1_err = 3'd1;
        else if (!s1_side_ok)  s1_err = 3'd2;
        else if (!s1_price_ok) s1_err = 3'd3;
        else if (!s1_qty_ok)   s1_err = 3'd4;
        else if (!s1_rsv_ok)   s1_err = 3'd5;
        else                   s1_err = 3'd0;
    end

    // Same as s1_err == 0, but from a register: keeps the priority encoder off
    // the counter-enable path (the critical path before this change).
    wire s1_good = s1_valid && s1_ok;

    always_ff @(posedge clk) begin
        if (rst) begin
            out_valid  <= 1'b0;
            cnt_msgs   <= '0;
            cnt_errors <= '0;
            cnt_add    <= '0;
            cnt_market <= '0;
            cnt_cancel <= '0;
            cnt_modify <= '0;
        end else begin
            out_valid <= s1_valid;
            if (s1_valid) cnt_msgs <= cnt_msgs + 32'd1;
            if (s1_valid && !s1_ok) cnt_errors <= cnt_errors + 32'd1;
            if (s1_good && s1_add)    cnt_add    <= cnt_add + 32'd1;
            if (s1_good && s1_market) cnt_market <= cnt_market + 32'd1;
            if (s1_good && s1_cancel) cnt_cancel <= cnt_cancel + 32'd1;
            if (s1_good && s1_modify) cnt_modify <= cnt_modify + 32'd1;
        end
        out_err   <= s1_err;
        out_ts    <= s1_ts;
        out_type  <= s1_type;
        out_id    <= s1_id;
        out_side  <= s1_side;
        out_price <= s1_price;
        out_qty   <= s1_qty;
    end
endmodule
