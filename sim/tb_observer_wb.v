//===========================================================================
// tb_observer_wb.v -- the observer's register window, the second opinion
//
// The Icarus path for rtl/bcmc_obs_wb.v. Verilator's sim/bcmc_obs_wb_test.cpp
// has already held the RTL to the model's corpus; what this adds is a different
// simulator, a differently written replay, and therefore agreement that is not
// an artifact of one toolchain. It reads the SAME sim/vectors/obswb_edge.txt
// and executes nothing from validation/.
//
//     frozen corpus
//        /      \
//   Verilator    Icarus
//        \      /
//    both must reproduce the declared outputs
//
// What it is suspicious of, in order of how they have actually bitten:
// a corpus that is empty or short; headers and rows that do not parse; a
// geometry the module does not have; a request held through its response; a
// response that arrives on the wrong cycle; ack and err both asserted; and a
// refused access that leaves a trace behind.
//
// Parsing is by tokens, not by indexing a line read with $fgets -- reading a
// packed string's byte at a fixed position depends on how a simulator justifies
// $fgets, and sim/tb_observer.v was caught by exactly that. Numbers are read
// with $fscanf and converted by hand, as in sim/tb_wb.v.
//
// No debug ports
// --------------
// The module's interface is the frozen one (section 3.1). Nothing is added to
// it to make this bench easier: what cannot be seen from outside -- the cursor,
// the registered strobe -- is simply not checked here, and is checked in
// validation/ by the model that owns it.
//
// Cycle semantics, as in the corpus: drive the recorded inputs, let the
// combination settle, compare the declared outputs, and only then take the edge
// that consumes them.
//
// Usage:
//     iverilog -g2005 -Wall -Wno-timescale -s tb_observer_wb \
//         -o build-icarus/tb_observer_wb.vvp tb_observer_wb.v \
//         ../rtl/bcmc_obs_wb.v ../rtl/bcmc_observer.v \
//         ../rtl/bcmc_column.v ../rtl/bcmc_cell.v
//     vvp build-icarus/tb_observer_wb.vvp +vectors=vectors/obswb_edge.txt
//===========================================================================

`timescale 1ns / 1ps

`default_nettype none

module tb_observer_wb;

    // Geometry: the reference build of validation/observer_periph.py (its
    // REF_VAL_W, REF_IDX_W, REF_MAX_C) and the module's own default parameters.
    // Taken from the document, as sim/tb_wb.v takes its constants, rather than
    // read back out of the RTL, so that a module built to other geometry cannot
    // agree with the corpus by construction. The corpus's OBS_CAPS read is what
    // reports a mismatch: it is the third operation of the first file.
    localparam VAL_W = 16;
    localparam IDX_W = 16;
    localparam MAX_C = 32;

    // The row weight and offset the corpus was recorded with: the generator's
    // W2 and O2, two rows of weight 1 at offset 0. Deliberately not the zero
    // context -- a zeroed one would make every visit's projection zero, and the
    // projection is one of the things this bench is here to check.
    localparam NROWS      = 2;
    localparam ROW_WEIGHT = 1;
    localparam ROW_OFFSET = 0;

    localparam MAXTOK = 32;

    integer errors;
    integer n_runs;
    integer n_cycles;
    integer fd;

    reg clk;

    reg  [11:0] wb_adr_i;
    reg  [31:0] wb_dat_i;
    reg  [3:0]  wb_sel_i;
    reg         wb_we_i;
    reg         wb_stb_i;
    reg         wb_cyc_i;
    reg         wb_rst_i;
    wire [31:0] wb_dat_o;
    wire        wb_ack_o;
    wire        wb_err_o;

    reg  [VAL_W-1:0]       obs_n_i;
    reg  [IDX_W-1:0]       obs_c_i;
    reg                    obs_valid_i;
    reg  [MAX_C*VAL_W-1:0] W_FLAT;
    reg  [MAX_C*VAL_W-1:0] O_FLAT;

    wire [VAL_W-1:0] column_o;
    wire             visit_valid_o;
    wire [MAX_C-1:0] column_bits_o;
    wire             done_o;
    wire             running_o;
    wire             aborted_o;

    bcmc_obs_wb #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W),
        .MAX_C (MAX_C)
    ) dut (
        .wb_clk_i (clk),
        .wb_rst_i (wb_rst_i),
        .wb_adr_i (wb_adr_i),
        .wb_dat_i (wb_dat_i),
        .wb_sel_i (wb_sel_i),
        .wb_we_i  (wb_we_i),
        .wb_stb_i (wb_stb_i),
        .wb_cyc_i (wb_cyc_i),
        .wb_dat_o (wb_dat_o),
        .wb_ack_o (wb_ack_o),
        .wb_err_o (wb_err_o),
        .obs_n_i            (obs_n_i),
        .obs_c_i            (obs_c_i),
        .obs_valid_i        (obs_valid_i),
        .obs_weights_flat_i (W_FLAT),
        .obs_offsets_flat_i (O_FLAT),
        .column_o       (column_o),
        .visit_valid_o  (visit_valid_o),
        .column_bits_o  (column_bits_o),
        .done_o         (done_o),
        .running_o      (running_o),
        .aborted_o      (aborted_o)
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

    // Reset between runs. The corpus records rst = 0 on every cycle, so reset
    // is the bench's business rather than a recorded cycle: a run is a fresh
    // device, and a run that inherited state from its predecessor would not be
    // the run the corpus describes.
    task reset_dut;
        begin
            wb_rst_i    = 1'b1;
            wb_cyc_i    = 1'b0;
            wb_stb_i    = 1'b0;
            wb_we_i     = 1'b0;
            obs_valid_i = 1'b0;
            #1;
            do_cycle;
            do_cycle;
            wb_rst_i = 1'b0;
            #1;
        end
    endtask

    task set_context;
        input [VAL_W-1:0] n_len;
        input [IDX_W-1:0] n_rows;
        integer           k;
        begin
            obs_n_i = n_len;
            obs_c_i = n_rows;
            W_FLAT  = {(MAX_C*VAL_W){1'b0}};
            O_FLAT  = {(MAX_C*VAL_W){1'b0}};
            for (k = 0; k < NROWS; k = k + 1) begin
                W_FLAT[VAL_W*k +: VAL_W] = ROW_WEIGHT;
                O_FLAT[VAL_W*k +: VAL_W] = ROW_OFFSET;
            end
            #1;
        end
    endtask

    //-----------------------------------------------------------------------
    // Comparing
    //-----------------------------------------------------------------------

    reg [8*MAXTOK-1:0] run_name;
    integer            cyc_idx;

    task report_fail;
        input [8*48-1:0] what;
        input integer    want;
        input integer    got;
        begin
            $display("FAIL  %0s cycle %0d: %0s is %0d, expected %0d",
                     run_name, cyc_idx, what, got, want);
            $display("      inputs: valid=%0d rst=%0d cyc=%0d stb=%0d we=%0d",
                     obs_valid_i, wb_rst_i, wb_cyc_i, wb_stb_i, wb_we_i);
            $display("              adr=%03h sel=%h dat=%08h",
                     wb_adr_i, wb_sel_i, wb_dat_i);
            errors = errors + 1;
        end
    endtask

    task expect_eq;
        input integer    want;
        input integer    got;
        input [8*48-1:0] what;
        begin
            if (want !== got) report_fail(what, want, got);
        end
    endtask

    //-----------------------------------------------------------------------
    // Reading the corpus, a token at a time
    //
    // sim/tb_wb.v's idiom: $fscanf for the token, and the conversion done by
    // hand. It costs a few lines and buys independence from how a simulator
    // represents a line read with $fgets, which is what broke the first version
    // of sim/tb_observer.v.
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

    // Reads the next token, discarding comments to the end of their line.
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
        reg [31:0] f_v, f_r, f_c, f_s, f_w, f_a, f_se, f_d;
        reg [31:0] f_ack, f_err, f_rdata, f_run, f_vis;
        reg [31:0] f_col, f_done, f_abt, f_bits;
        begin : replay_body
            get_dec(n_len);
            get_dec(n_rows);
            get_dec(cyc_count);

            reset_dut;
            set_context(n_len[VAL_W-1:0], n_rows[IDX_W-1:0]);

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

                get_hex(f_v);     get_hex(f_r);      get_hex(f_c);
                get_hex(f_s);     get_hex(f_w);      get_hex(f_a);
                get_hex(f_se);    get_hex(f_d);      get_hex(f_ack);
                get_hex(f_err);   get_hex(f_rdata);  get_hex(f_run);
                get_hex(f_vis);   get_hex(f_col);    get_hex(f_done);
                get_hex(f_abt);   get_hex(f_bits);

                wb_rst_i    = f_r[0];
                wb_cyc_i    = f_c[0];
                wb_stb_i    = f_s[0];
                wb_we_i     = f_w[0];
                wb_adr_i    = f_a[11:0];
                wb_sel_i    = f_se[3:0];
                wb_dat_i    = f_d;
                obs_valid_i = f_v[0];
                #1;

                // A cycle is never answered twice over: ack and err are
                // mutually exclusive, whatever the corpus says.
                if (wb_ack_o && wb_err_o) begin
                    $display("FAIL  %0s cycle %0d: ack and err together",
                             run_name, k);
                    errors = errors + 1;
                end

                expect_eq(f_ack, wb_ack_o, "wb_ack_o");
                expect_eq(f_err, wb_err_o, "wb_err_o");
                expect_eq(f_run, running_o, "running_o");
                expect_eq(f_vis, visit_valid_o, "visit_valid_o");
                expect_eq(f_col, column_o, "column_o");
                expect_eq(f_done, done_o, "done_o");
                expect_eq(f_abt, aborted_o, "aborted_o");

                // wb_dat_o is loaded on an accepted access and holds
                // otherwise, so it is compared only where the corpus records
                // the access as acknowledged.
                if (f_ack[0] == 1'b1)
                    expect_eq(f_rdata, wb_dat_o, "wb_dat_o");

                // The projection is defined for N >= 1 only; with N = 0 the
                // corpus's 0 is the model declining to answer, not a
                // prediction, and the RTL's answer there is meaningless.
                if (n_len >= 1)
                    expect_eq(f_bits, column_bits_o, "column_bits_o");

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

        wb_adr_i = 12'h0;
        wb_dat_i = 32'h0;
        wb_sel_i = 4'hF;
        wb_we_i  = 1'b0;
        wb_stb_i = 1'b0;
        wb_cyc_i = 1'b0;
        wb_rst_i = 1'b0;
        obs_n_i     = {VAL_W{1'b0}};
        obs_c_i     = {IDX_W{1'b0}};
        obs_valid_i = 1'b0;
        W_FLAT      = {(MAX_C*VAL_W){1'b0}};
        O_FLAT      = {(MAX_C*VAL_W){1'b0}};
        #1;

        // Diagnostic dump, gated on +vcd=<path>. With no such plusarg the bench
        // behaves and fails exactly as before and writes no file. With one, the
        // WHOLE hierarchy is dumped so that internal state -- the shuffled
        // source's st0_q/st1_q/playing_q/underrun_q, the engine's col_q/tq -- can
        // be aligned against the model's per-edge table. Harness-only diagnostic:
        // no RTL, model, corpus or expectation changes with it.
        begin : vcd_dump
            reg [8*256-1:0] vcdfile;
            if ($value$plusargs("vcd=%s", vcdfile)) begin
                $dumpfile(vcdfile);
                $dumpvars(0, tb_observer_wb);
            end
        end

        if (!$value$plusargs("vectors=%s", vecfile)) begin
            $display("FAIL  tb_observer_wb: no +vectors=<file> given");
            $finish;
        end

        fd = $fopen(vecfile, "r");
        if (fd == 0) begin
            $display("FAIL  tb_observer_wb: cannot open %0s", vecfile);
            $finish;
        end

        // Runs are separated by `---`, and each is introduced by an H line
        // naming it. Anything else at the top level is a failure.
        begin : read_all
            while (1) begin
                get_token;
                if (tok_ok != 1) disable read_all;      // end of file
                if ((tok_len(tok) == 3) && (tok[23:0] == "---")) begin
                    // a separator: neither a run nor an error
                end else if ((tok_len(tok) == 1) && (tok[7:0] == "H")) begin
                    get_token;                          // the run's name
                    if (tok_ok != 1) begin
                        $display("FAIL  tb_observer_wb: an H line with no name");
                        errors = errors + 1;
                        disable read_all;
                    end
                    run_name = tok;
                    replay_run;
                    n_runs = n_runs + 1;
                end else begin
                    $display("FAIL  tb_observer_wb: expected an H line, found '%0s'",
                             tok);
                    errors = errors + 1;
                    disable read_all;
                end
            end
        end

        $fclose(fd);

        // A corpus that yielded no runs is a failure, not a pass: an empty
        // replay is exactly how a wiring mistake becomes a green.
        if (n_runs == 0) begin
            $display("FAIL  tb_observer_wb: no runs found in %0s", vecfile);
            errors = errors + 1;
        end

        if (errors == 0)
            $display("tb_observer_wb: PASS  %0d runs, %0d cycles  (%0s)",
                     n_runs, n_cycles, vecfile);
        else
            $display("tb_observer_wb: FAIL  %0d runs, %0d cycles, %0d errors  (%0s)",
                     n_runs, n_cycles, errors, vecfile);

        $finish;
    end

endmodule
