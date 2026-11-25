// timing_wrap: synthesis-only harness so place-and-route can time msg_parser.
//
// The parser has ~250 input and ~450 output bits, far more than any small FPGA
// has pins. This wrapper feeds the 256-bit bus from an 8-bit shift register and
// folds every output into a registered parity bit, so all parser paths stay
// register-to-register and nothing is optimized away. It is NOT a usable
// interface: it exists only to get a timing and resource report.

module timing_wrap (
    input  logic       clk,
    input  logic       rst,
    input  logic       in_valid,
    input  logic [7:0] in_byte,
    output logic [7:0] out_pins
);
    logic [255:0] shift;
    logic         valid_q;

    always_ff @(posedge clk) begin
        shift   <= {shift[247:0], in_byte};
        valid_q <= in_valid;
    end

    logic         out_valid;
    logic [2:0]   out_err;
    logic [63:0]  out_ts, out_id;
    logic [7:0]   out_type, out_side;
    logic [31:0]  out_price, out_qty;
    logic [31:0]  cnt_msgs, cnt_errors, cnt_add, cnt_market, cnt_cancel, cnt_modify;

    msg_parser u_parser (
        .clk, .rst,
        .in_valid (valid_q),
        .in_data  (shift),
        .out_valid, .out_err, .out_ts, .out_type, .out_id, .out_side, .out_price, .out_qty,
        .cnt_msgs, .cnt_errors, .cnt_add, .cnt_market, .cnt_cancel, .cnt_modify
    );

    always_ff @(posedge clk) begin
        out_pins <= {out_valid, ^out_err, ^out_ts, ^out_id, ^{out_type, out_side},
                     ^{out_price, out_qty}, ^{cnt_msgs, cnt_errors, cnt_add},
                     ^{cnt_market, cnt_cancel, cnt_modify}};
    end
endmodule
