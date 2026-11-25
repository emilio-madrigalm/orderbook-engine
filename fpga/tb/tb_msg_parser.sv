// Testbench for msg_parser.
//
//   +IN=msgs.hex     one 256-bit message per line (ob_wire encode)
//   +OUT=got.csv     decoded lines, same format as ob_wire decode
//   +COUNTERS=c.txt  final counter values, one "name value" per line
//   +GAPS=1          drop in_valid on ~1 cycle in 4 (LFSR), to test bubbles
//
// Checks on every cycle that out_valid is in_valid delayed by exactly 2 cycles.
// The decoded output is diffed against ob_wire decode by fpga/sim.sh.

`timescale 1ns / 1ps

module tb_msg_parser;
    logic         clk = 1'b0;
    logic         rst = 1'b1;
    logic         in_valid = 1'b0;
    logic [255:0] in_data = '0;

    logic         out_valid;
    logic [2:0]   out_err;
    logic [63:0]  out_ts, out_id;
    logic [7:0]   out_type, out_side;
    logic [31:0]  out_price, out_qty;
    logic [31:0]  cnt_msgs, cnt_errors, cnt_add, cnt_market, cnt_cancel, cnt_modify;

    msg_parser dut (.*);

    always #2 clk = ~clk;  // 250 MHz nominal; simulation time only

    string  in_path, out_path, cnt_path;
    integer fin, fout, fcnt, rc;
    integer gaps = 0;
    integer n_in = 0, n_out = 0, cycles = 0, first_cycle = -1, last_cycle = 0, latency_errors = 0;
    logic [255:0] word;
    logic [15:0]  lfsr = 16'hACE1;
    logic [1:0]   valid_pipe = 2'b00;

    // Latency check: out_valid must equal in_valid from two cycles earlier.
    always @(posedge clk) begin
        if (!rst) begin
            if (out_valid !== valid_pipe[1]) latency_errors = latency_errors + 1;
            valid_pipe <= {valid_pipe[0], in_valid};
            cycles = cycles + 1;
            if (out_valid) begin
                $fdisplay(fout, "%0d,%0d,%0d,%0d,%0d,%0d,%0d",
                          out_err, out_ts, out_type, out_id, out_side, out_price, out_qty);
                n_out = n_out + 1;
                last_cycle = cycles;
            end
        end
    end

    initial begin
        if (!$value$plusargs("IN=%s", in_path)) begin
            $display("tb: missing +IN=file");
            $finish;
        end
        if (!$value$plusargs("OUT=%s", out_path)) out_path = "got.csv";
        if (!$value$plusargs("COUNTERS=%s", cnt_path)) cnt_path = "counters.txt";
        if (!$value$plusargs("GAPS=%d", gaps)) gaps = 0;

        fin  = $fopen(in_path, "r");
        fout = $fopen(out_path, "w");
        if (fin == 0 || fout == 0) begin
            $display("tb: cannot open files");
            $finish;
        end

        repeat (3) @(posedge clk);
        rst <= 1'b0;

        // No break: Icarus Verilog 12 does not support it.
        rc = $fscanf(fin, "%h\n", word);
        while (rc == 1) begin
            // Optional bubble before this message.
            lfsr = {lfsr[14:0], lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10]};
            if (gaps != 0 && lfsr[1:0] == 2'b00) begin
                @(negedge clk);
                in_valid = 1'b0;
            end
            @(negedge clk);
            in_valid = 1'b1;
            in_data  = word;
            n_in = n_in + 1;
            if (first_cycle < 0) first_cycle = cycles;
            rc = $feof(fin) ? 0 : $fscanf(fin, "%h\n", word);
        end
        @(negedge clk);
        in_valid = 1'b0;
        repeat (4) @(posedge clk);

        fcnt = $fopen(cnt_path, "w");
        $fdisplay(fcnt, "msgs %0d", cnt_msgs);
        $fdisplay(fcnt, "errors %0d", cnt_errors);
        $fdisplay(fcnt, "add %0d", cnt_add);
        $fdisplay(fcnt, "market %0d", cnt_market);
        $fdisplay(fcnt, "cancel %0d", cnt_cancel);
        $fdisplay(fcnt, "modify %0d", cnt_modify);
        $fclose(fcnt);
        $fclose(fout);
        $fclose(fin);

        $display("tb: in=%0d out=%0d latency_errors=%0d cycles_first_in_to_last_out=%0d",
                 n_in, n_out, latency_errors, last_cycle - first_cycle);
        if (n_in != n_out || latency_errors != 0) $display("tb: FAIL");
        else $display("tb: PASS");
        $finish;
    end
endmodule
