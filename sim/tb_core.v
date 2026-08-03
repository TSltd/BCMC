//===========================================================================
// tb_core.v -- self-checking testbench for bcmc_core, in Verilog
//
// This is the Icarus Verilog path. It reads exactly the same vector files as
// sim/bcmc_core_test.cpp (the Verilator path), so the RTL is checked against
// the Python reference model by two independent simulators. A disagreement
// between them is a bug in one of the simulators or in the RTL, never in the
// expected answers -- those come from validation/reference.py.
//
// Every case is checked two independent ways:
//
//   1. Against Python:      the offsets recorded in the vector file.
//   2. Against the defining recurrence, computed below with Verilog's own `%`
//      operator, with no reference to Python and no reuse of the RTL's
//      conditional-subtract trick:
//
//          o[0]   = 0
//          o[i+1] = (o[i] + w[i]) mod N
//
// The BCMC Prefix Stream Interface protocol is checked too: exactly C offsets,
// offset_valid only while busy, done a single-cycle pulse strictly after the
// last offset, busy and done never together, and C = 0 handled.
//
// Finally each case is replayed four times -- weights back to back, then with
// 1, 3 and randomised idle cycles between them -- and all four runs must
// produce identical offsets. The sequence of accepted weights alone determines
// the output sequence; idle cycles affect timing, never values.
//
// Usage:
//     vvp tb_core.vvp +vectors=vectors/core_random.txt [+vcd]
//
// All testbench activity happens on the falling edge of the clock: outputs are
// sampled mid-cycle, where they are stable, and inputs are driven mid-cycle so
// that they are already settled at the rising edge that ends the cycle.
//===========================================================================

`timescale 1ns / 1ps
`default_nettype none

module tb_core;

    //-----------------------------------------------------------------------
    // Sizes
    //-----------------------------------------------------------------------

    localparam integer VAL_W  = 16;
    localparam integer IDX_W  = 16;
    localparam integer MAXC   = 4096;   // largest C any vector file may hold
    localparam integer MAXTOK = 32;     // longest token in a vector file

    //-----------------------------------------------------------------------
    // Device under test
    //-----------------------------------------------------------------------

    reg                clk          = 1'b0;
    reg                rst          = 1'b1;
    reg                start        = 1'b0;
    reg  [VAL_W-1:0]   N            = {VAL_W{1'b0}};
    reg  [IDX_W-1:0]   C            = {IDX_W{1'b0}};
    reg  [VAL_W-1:0]   weight_in    = {VAL_W{1'b0}};
    reg                weight_valid = 1'b0;

    wire               busy;
    wire               done;
    wire [VAL_W-1:0]   offset_out;
    wire               offset_valid;

    bcmc_core #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W)
    ) dut (
        .clk          (clk),
        .rst          (rst),
        .start        (start),
        .busy         (busy),
        .done         (done),
        .N            (N),
        .C            (C),
        .weight_in    (weight_in),
        .weight_valid (weight_valid),
        .offset_out   (offset_out),
        .offset_valid (offset_valid)
    );

    always #5 clk = ~clk;

    //-----------------------------------------------------------------------
    // Case storage
    //-----------------------------------------------------------------------

    reg [VAL_W-1:0] wgt  [0:MAXC-1];   // weights, from the vector file
    reg [VAL_W-1:0] pyo  [0:MAXC-1];   // offsets, from the vector file (Python)
    reg [VAL_W-1:0] rec  [0:MAXC-1];   // offsets, from the recurrence (here)
    reg [VAL_W-1:0] got  [0:MAXC-1];   // offsets, from the RTL
    reg [VAL_W-1:0] ref0 [0:MAXC-1];   // offsets from the first run, for gaps

    integer case_n;        // N for the current case
    integer case_c;        // C for the current case
    integer case_index;    // 1-based, for messages
    integer gap_mode;      // 0, 1, 3, or -1 for random

    integer total_cases;
    integer total_runs;
    integer total_rows;

    //-----------------------------------------------------------------------
    // Vector file reading
    //
    // $fscanf with %s skips all whitespace, so the only thing the reader has
    // to handle specially is a '#' comment, which runs to end of line.
    //-----------------------------------------------------------------------

    integer            fd;
    reg [8*256-1:0]    vec_path;
    reg [8*MAXTOK-1:0] tok;
    reg [8*512-1:0]    line_buf;
    reg                tok_eof;
    integer            tok_got;

    // The first character of a right-justified, zero-padded string.
    function [7:0] first_char;
        input [8*MAXTOK-1:0] s;
        integer k;
        begin
            first_char = 8'h00;
            for (k = MAXTOK-1; k >= 0; k = k - 1) begin
                if (first_char == 8'h00) begin
                    first_char = s[8*k +: 8];
                end
            end
        end
    endfunction

    task get_token;
        begin
            tok_eof = 1'b0;
            tok_got = 0;
            while ((tok_got == 0) && (tok_eof === 1'b0)) begin
                tok = {(8*MAXTOK){1'b0}};
                if ($fscanf(fd, "%s", tok) != 1) begin
                    tok_eof = 1'b1;
                end else if (first_char(tok) == "#") begin
                    if ($fgets(line_buf, fd) == 0) tok_eof = 1'b1;
                end else begin
                    tok_got = 1;
                end
            end
        end
    endtask

    task expect_keyword;
        input [8*MAXTOK-1:0] want;
        begin
            get_token;
            if (tok_eof || (tok !== want)) begin
                $display("tb_core: PARSE ERROR in %0s: expected '%0s', got '%0s'",
                         vec_path, want, tok);
                $fatal(1);
            end
        end
    endtask

    task get_number;
        output integer value;
        begin
            get_token;
            if (tok_eof) begin
                $display("tb_core: PARSE ERROR in %0s: unexpected end of file",
                         vec_path);
                $fatal(1);
            end
            if ($sscanf(tok, "%d", value) != 1) begin
                $display("tb_core: PARSE ERROR in %0s: expected a number, got '%0s'",
                         vec_path, tok);
                $fatal(1);
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Failure reporting
    //-----------------------------------------------------------------------

    task fail;
        input [8*64-1:0] msg;
        begin
            $display("tb_core: FAIL  %0s", msg);
            $display("               case %0d of %0s: N = %0d, C = %0d, gap mode %0d",
                     case_index, vec_path, case_n, case_c, gap_mode);
            $fatal(1);
        end
    endtask

    //-----------------------------------------------------------------------
    // The defining recurrence, computed here with a real modulo
    //-----------------------------------------------------------------------

    integer acc;
    integer ri;

    task compute_recurrence;
        begin
            acc = 0;
            for (ri = 0; ri < case_c; ri = ri + 1) begin
                rec[ri] = acc[VAL_W-1:0];
                acc     = (acc + wgt[ri]) % case_n;
            end
            if ((case_c > 0) && (rec[0] !== {VAL_W{1'b0}})) begin
                fail("recurrence gives offset[0] != 0");
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Drive one case through the DUT
    //-----------------------------------------------------------------------

    integer nrec;        // offsets collected
    integer wi;          // weights sent
    integer waitc;       // idle cycles still to insert before the next weight
    integer guard;       // cycle budget
    integer kk;
    integer send;
    reg     done_seen;
    reg     stop_loop;

    task next_gap;
        output integer g;
        begin
            if (gap_mode < 0) g = {$random} % 5;
            else              g = gap_mode;
        end
    endtask

    task run_case;
        begin
            // --- reset ----------------------------------------------------
            rst          = 1'b1;
            start        = 1'b0;
            weight_valid = 1'b0;
            weight_in    = {VAL_W{1'b0}};
            N            = case_n[VAL_W-1:0];
            C            = case_c[IDX_W-1:0];
            @(negedge clk);
            @(negedge clk);
            rst = 1'b0;
            @(negedge clk);

            if ((busy !== 1'b0) || (done !== 1'b0) || (offset_valid !== 1'b0)) begin
                fail("not quiescent after reset");
            end

            // --- start ----------------------------------------------------
            start = 1'b1;
            @(negedge clk);
            start = 1'b0;

            nrec      = 0;
            wi        = 0;
            done_seen = 1'b0;
            stop_loop = 1'b0;
            next_gap(waitc);
            guard = case_c * 8 + 64;
            kk    = 0;

            while ((stop_loop === 1'b0) && (kk < guard)) begin
                kk = kk + 1;

                // --- sample the current cycle -------------------------
                if ((busy === 1'b1) && (done === 1'b1)) begin
                    fail("busy and done asserted in the same cycle");
                end

                if (offset_valid === 1'b1) begin
                    if (busy !== 1'b1) fail("offset_valid asserted while not busy");
                    if (nrec >= case_c) fail("more than C offsets emitted");
                    got[nrec] = offset_out;
                    nrec      = nrec + 1;
                end

                if (done === 1'b1) begin
                    if (nrec != case_c) fail("done asserted before the last offset");
                    done_seen = 1'b1;
                end

                // --- drive the current cycle --------------------------
                send = 0;
                if ((busy === 1'b1) && (wi < case_c)) begin
                    if (waitc > 0) waitc = waitc - 1;
                    else           send  = 1;
                end
                weight_valid = (send != 0);
                weight_in    = (send != 0) ? wgt[wi] : {VAL_W{1'b0}};

                @(negedge clk);

                if (send != 0) begin
                    wi = wi + 1;
                    next_gap(waitc);
                end
                if (done_seen === 1'b1) stop_loop = 1'b1;
            end

            weight_valid = 1'b0;
            weight_in    = {VAL_W{1'b0}};

            if (done_seen !== 1'b1) fail("done never asserted");
            if (wi != case_c)       fail("not every weight was accepted");
            if (nrec != case_c)     fail("wrong number of offsets emitted");

            // --- quiet once the transform is over -------------------------
            for (kk = 0; kk < 8; kk = kk + 1) begin
                if (offset_valid === 1'b1) fail("offset_valid asserted after done");
                if (done === 1'b1)         fail("done asserted for more than one cycle");
                if (busy === 1'b1)         fail("busy still asserted after done");
                @(negedge clk);
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Check one run
    //-----------------------------------------------------------------------

    integer ci;

    task check_run;
        input integer is_first;
        begin
            for (ci = 0; ci < case_c; ci = ci + 1) begin
                if (got[ci] !== pyo[ci]) begin
                    $display("tb_core: offset[%0d]: rtl %0d, python %0d",
                             ci, got[ci], pyo[ci]);
                    fail("RTL disagrees with Python");
                end
                if (got[ci] !== rec[ci]) begin
                    $display("tb_core: offset[%0d]: rtl %0d, recurrence %0d",
                             ci, got[ci], rec[ci]);
                    fail("RTL disagrees with the recurrence");
                end
                if (got[ci] >= case_n[VAL_W-1:0]) begin
                    fail("offset is not less than N");
                end
                if (is_first != 0) ref0[ci] = got[ci];
                else if (got[ci] !== ref0[ci]) begin
                    fail("idle cycles between weights changed the output");
                end
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Main
    //-----------------------------------------------------------------------

    integer mode_i;
    integer tmp;
    integer i;
    reg     eof_reached;

    initial begin
        if (!$value$plusargs("vectors=%s", vec_path)) begin
            $display("tb_core: ERROR give +vectors=<file>");
            $fatal(1);
        end

        if ($test$plusargs("vcd")) begin
            $dumpfile("waves/tb_core.vcd");
            $dumpvars(0, tb_core);
        end

        fd = $fopen(vec_path, "r");
        if (fd == 0) begin
            $display("tb_core: ERROR cannot open %0s", vec_path);
            $fatal(1);
        end

        total_cases = 0;
        total_runs  = 0;
        total_rows  = 0;
        case_index  = 0;
        gap_mode    = 0;
        eof_reached = 1'b0;

        while (eof_reached === 1'b0) begin
            get_token;
            if (tok_eof) begin
                eof_reached = 1'b1;
            end else begin
                if (tok !== "N") begin
                    $display("tb_core: PARSE ERROR in %0s: expected 'N', got '%0s'",
                             vec_path, tok);
                    $fatal(1);
                end
                get_number(case_n);
                expect_keyword("C");
                get_number(case_c);

                case_index  = case_index + 1;
                total_cases = total_cases + 1;
                total_rows  = total_rows + case_c;

                if (case_c > MAXC) begin
                    $display("tb_core: ERROR C = %0d exceeds MAXC = %0d",
                             case_c, MAXC);
                    $fatal(1);
                end

                for (i = 0; i < case_c; i = i + 1) begin
                    get_number(tmp);
                    wgt[i] = tmp[VAL_W-1:0];
                end

                expect_keyword("---");

                for (i = 0; i < case_c; i = i + 1) begin
                    get_number(tmp);
                    pyo[i] = tmp[VAL_W-1:0];
                end

                // The vector file must itself satisfy the recurrence.
                compute_recurrence;
                for (i = 0; i < case_c; i = i + 1) begin
                    if (pyo[i] !== rec[i]) begin
                        $display("tb_core: offset[%0d]: python %0d, recurrence %0d",
                                 i, pyo[i], rec[i]);
                        fail("vector file disagrees with the recurrence");
                    end
                end

                // Four runs: back to back, then 1, 3 and random idle cycles.
                for (mode_i = 0; mode_i < 4; mode_i = mode_i + 1) begin
                    case (mode_i)
                        0:       gap_mode = 0;
                        1:       gap_mode = 1;
                        2:       gap_mode = 3;
                        default: gap_mode = -1;
                    endcase
                    run_case;
                    check_run((mode_i == 0) ? 1 : 0);
                    total_runs = total_runs + 1;
                end
            end
        end

        $fclose(fd);

        if (total_cases == 0) begin
            $display("tb_core: ERROR no cases found in %0s", vec_path);
            $fatal(1);
        end

        $display("tb_core: PASS  %0d cases, %0d runs, %0d rows  (%0s)",
                 total_cases, total_runs, total_rows, vec_path);
        $finish;
    end

endmodule

`default_nettype wire
