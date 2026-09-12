//===========================================================================
// tb_out_engine.v -- the output engine, the second opinion
//
// The Icarus path for rtl/bcmc_out_engine.v. Verilator's
// sim/bcmc_out_engine_test.cpp has already held the RTL to the model's corpus;
// what this adds is a different simulator, a differently written reader, and
// therefore agreement that is not an artifact of one toolchain. It reads the
// SAME sim/vectors/outeng_edge.txt and executes nothing from validation/.
//
//     frozen corpus
//        /      \
//   Verilator    Icarus
//        \      /
//    both must reproduce pins_o, every cycle
//
// `pins_o` is the whole observable contract -- one output, with an expected
// value recorded for every cycle -- so this bench is not comparing a convenient
// subset. It does not read `pattern_q`, because the module does not export it and
// adding a port to make the bench easier would trade a contract check for an
// implementation check.
//
// Parsing is by tokens, not by indexing a line read with $fgets: reading a packed
// string's byte at a fixed position depends on how a simulator justifies $fgets,
// and sim/tb_observer.v was caught by exactly that. Numbers are read with
// $fscanf and converted by hand, as in sim/tb_wb.v.
//
// Cycle semantics, as in the corpus: drive the recorded inputs, let the
// combination settle, compare, and only then take the edge that consumes them.
//
// Usage:
//     iverilog -g2005 -Wall -Wno-timescale -s tb_out_engine \
//         -o build-icarus/tb_out_engine.vvp tb_out_engine.v \
//         ../rtl/bcmc_out_engine.v
//     vvp build-icarus/tb_out_engine.vvp +vectors=vectors/outeng_edge.txt
//===========================================================================

`timescale 1ns / 1ps

`default_nettype none

module tb_out_engine;

    // Geometry: validation/output_engine.py's REF_MAX_C and rtl/
    // bcmc_out_engine.v's own defaults. Taken from the specification rather than
    // read back out of the RTL, so that a module built to other geometry cannot
    // agree with the corpus by construction.
    localparam IDX_W = 16;
    localparam MAX_C = 32;

    localparam MAXTOK = 32;

    integer errors;
    integer n_runs;
    integer n_cycles;
    integer fd;

    reg clk;

    reg  [MAX_C-1:0] column_bits_i;
    reg              visit_valid_i;
    reg              valid_i;
    reg  [IDX_W-1:0] C_i;
    reg              rst;
    wire [MAX_C-1:0] pins_o;

    bcmc_out_engine #(
        .IDX_W (IDX_W),
        .MAX_C (MAX_C)
    ) dut (
        .clk           (clk),
        .rst           (rst),
        .column_bits_i (column_bits_i),
        .visit_valid_i (visit_valid_i),
        .valid_i       (valid_i),
        .C_i           (C_i),
        .pins_o        (pins_o)
    );

    //-----------------------------------------------------------------------
    // Driving the module
    //-----------------------------------------------------------------------

    // Named do_cycle, not edge: `edge` is a Verilog keyword.
    task do_cycle;
        begin
            clk = 1'b1; #1;
            clk = 1'b0; #1;
        end
    endtask

    // Between runs the device is reset with the context low, because the corpus
    // records rst = 0 on ordinary cycles. `valid_q` must start at zero or a
    // run's first cycle would look like a revalidation.
    task reset_dut;
        begin
            rst           = 1'b1;
            valid_i       = 1'b0;
            visit_valid_i = 1'b0;
            column_bits_i = {MAX_C{1'b0}};
            C_i           = {IDX_W{1'b0}};
            #1;
            do_cycle;
            do_cycle;
            rst = 1'b0;
            #1;
        end
    endtask

    //-----------------------------------------------------------------------
    // Comparing
    //-----------------------------------------------------------------------

    reg [8*MAXTOK-1:0] run_name;
    integer            cyc_idx;

    task report_fail;
        input [8*48-1:0]  what;
        input [MAX_C-1:0] want;
        input [MAX_C-1:0] got;
        begin
            $display("FAIL  %0s cycle %0d: %0s is %h, expected %h",
                     run_name, cyc_idx, what, got, want);
            $display("      inputs: rst=%0d valid=%0d vv=%0d bits=%h C=%0d",
                     rst, valid_i, visit_valid_i, column_bits_i, C_i);
            errors = errors + 1;
        end
    endtask

    //-----------------------------------------------------------------------
    // Reading the corpus, a token at a time
    //-----------------------------------------------------------------------

    reg [8*MAXTOK-1:0] tok;
    reg [8*256-1:0]    line_buf;
    integer            tok_ok;

    function integer first_byte;
        input [8*MAXTOK-1:0] s;
        integer              k;
        integer              found;
        begin
            first_byte = 0;
            found      = 0;
            for (k = MAXTOK - 1; k >= 0; k = k - 1) begin
                if (!found && s[8*k +: 8] != 8'h00) begin
                    first_byte = s[8*k +: 8];
                    found      = 1;
                end
            end
        end
    endfunction

    function integer tok_len;
        input [8*MAXTOK-1:0] s;
        integer              k;
        integer              found;
        begin
            tok_len = 0;
            found   = 0;
            for (k = MAXTOK - 1; k >= 0; k = k - 1) begin
                if (!found && s[8*k +: 8] != 8'h00) begin
                    tok_len = k + 1;
                    found   = 1;
                end
            end
        end
    endfunction

    task get_token;
        integer r;
        begin
            tok_ok = 0;
            while (tok_ok == 0) begin
                r = $fscanf(fd, "%s", tok);
                if (r != 1) begin
                    disable get_token;          // end of file
                end else if (first_byte(tok) == "#") begin
                    r = $fgets(line_buf, fd);   // discard the comment
                end else begin
                    tok_ok = 1;
                end
            end
        end
    endtask

    task get_dec;
        output integer value;
        integer        k;
        integer        len;
        reg [7:0]      ch;
        reg            bad;
        integer        acc;
        begin
            get_token;
            value = 0;
            acc   = 0;
            bad   = 1'b0;
            if (tok_ok != 1) begin
                $display("FAIL  unexpected end of file, expected a decimal number");
                errors = errors + 1;
            end else begin
                len = tok_len(tok);
                if ((len == 0) || (len > 9)) bad = 1'b1;
                k = 0;
                while ((k < len) && (bad == 1'b0)) begin
                    ch = tok[8*(len-1-k) +: 8];
                    if ((ch >= "0") && (ch <= "9")) acc = acc * 10 + (ch - "0");
                    else                                bad = 1'b1;
                    k = k + 1;
                end
                if (bad) begin
                    $display("FAIL  token '%0s' is not a decimal number", tok);
                    errors = errors + 1;
                end
                value = acc;
            end
        end
    endtask

    task get_hex;
        output [31:0] value;
        integer       k;
        integer       len;
        reg [3:0]     digit;
        reg [7:0]     ch;
        reg           bad;
        reg [31:0]    acc;
        begin
            get_token;
            value = 32'h0;
            acc   = 32'h0;
            bad   = 1'b0;
            if (tok_ok != 1) begin
                $display("FAIL  unexpected end of file, expected a hex number");
                errors = errors + 1;
            end else begin
                len = tok_len(tok);
                if ((len == 0) || (len > 8)) bad = 1'b1;
                k = 0;
                while ((k < len) && (bad == 1'b0)) begin
                    ch    = tok[8*(len-1-k) +: 8];
                    digit = 4'h0;
                    if      ((ch >= "0") && (ch <= "9")) digit = ch - "0";
                    else if ((ch >= "a") && (ch <= "f")) digit = (ch - "a") + 8'd10;
                    else if ((ch >= "A") && (ch <= "F")) digit = (ch - "A") + 8'd10;
                    else                                 bad   = 1'b1;
                    acc = (acc << 4) | {28'h0, digit};
                    k = k + 1;
                end
                if (bad) begin
                    $display("FAIL  token '%0s' is not a hex number in 8 digits", tok);
                    errors = errors + 1;
                end
                value = acc;
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Replaying one run
    //-----------------------------------------------------------------------

    reg [8*256-1:0] vecfile;

    task replay_run;
        integer    n_len;
        integer    n_rows;
        integer    cyc_count;
        integer    k;
        integer    f_rst;
        integer    f_valid;
        integer    f_vv;
        reg [31:0] f_bits;
        reg [31:0] f_pins;
        begin : replay_body
            get_dec(n_len);
            get_dec(n_rows);
            get_dec(cyc_count);

            reset_dut;
            C_i = n_rows[IDX_W-1:0];

            k = 0;
            while (k < cyc_count) begin
                cyc_idx = k;

                // The row's own tag, so that a corpus whose rows are not where
                // its header says they are is a failure rather than a silently
                // shifted replay.
                get_token;
                if (tok_ok != 1) begin
                    $display("FAIL  %0s: expected %0d cycles, ended at %0d",
                             run_name, cyc_count, k);
                    errors = errors + 1;
                    disable replay_body;
                end
                if (!((tok_len(tok) == 1) && (tok[7:0] == "C"))) begin
                    $display("FAIL  %0s cycle %0d: expected a C row, found '%0s'",
                             run_name, k, tok);
                    errors = errors + 1;
                    disable replay_body;
                end

                get_dec(f_rst);
                get_dec(f_valid);
                get_dec(f_vv);
                get_hex(f_bits);
                get_hex(f_pins);

                rst           = f_rst[0];
                valid_i       = f_valid[0];
                visit_valid_i = f_vv[0];
                column_bits_i = f_bits[MAX_C-1:0];
                C_i           = n_rows[IDX_W-1:0];
                #1;

                if (pins_o !== f_pins[MAX_C-1:0])
                    report_fail("pins_o", f_pins[MAX_C-1:0], pins_o);

                n_cycles = n_cycles + 1;
                do_cycle;
                k = k + 1;
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // The corpus
    //-----------------------------------------------------------------------

    initial begin : main_init
        errors   = 0;
        n_runs   = 0;
        n_cycles = 0;
        clk      = 1'b0;

        rst           = 1'b0;
        valid_i       = 1'b0;
        visit_valid_i = 1'b0;
        column_bits_i = {MAX_C{1'b0}};
        C_i           = {IDX_W{1'b0}};
        #1;

        if (!$value$plusargs("vectors=%s", vecfile)) begin
            $display("FAIL  tb_out_engine: no +vectors=<file> given");
            $finish;
        end

        fd = $fopen(vecfile, "r");
        if (fd == 0) begin
            $display("FAIL  tb_out_engine: cannot open %0s", vecfile);
            $finish;
        end

        // Runs are separated by `---`, and each is introduced by an H line naming
        // it. Anything else at the top level is a failure.
        begin : read_all
            while (1) begin
                get_token;
                if (tok_ok != 1) disable read_all;      // end of file
                if ((tok_len(tok) == 3) && (tok[23:0] == "---")) begin
                    // a separator: neither a run nor an error
                end else if ((tok_len(tok) == 1) && (tok[7:0] == "H")) begin
                    get_token;                          // the run's name
                    if (tok_ok != 1) begin
                        $display("FAIL  tb_out_engine: an H line with no name");
                        errors = errors + 1;
                        disable read_all;
                    end
                    run_name = tok;
                    replay_run;
                    n_runs = n_runs + 1;
                end else begin
                    $display("FAIL  tb_out_engine: expected an H line, found '%0s'",
                             tok);
                    errors = errors + 1;
                    disable read_all;
                end
            end
        end

        $fclose(fd);

        if (n_runs == 0) begin
            $display("FAIL  tb_out_engine: no runs found in %0s", vecfile);
            errors = errors + 1;
        end

        if (errors == 0)
            $display("tb_out_engine: PASS  %0d runs, %0d cycles  (%0s)",
                     n_runs, n_cycles, vecfile);
        else
            $display("tb_out_engine: FAIL  %0d runs, %0d cycles, %0d errors  (%0s)",
                     n_runs, n_cycles, errors, vecfile);

        $finish;
    end

endmodule
