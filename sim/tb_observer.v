//===========================================================================
// tb_observer.v -- Icarus Verilog testbench for bcmc_observer
//
// The second opinion on the sequential observer engine. Verilator's
// sim/bcmc_observer_hw_test.cpp is the primary check; this is a different
// simulator reading the same vector file and making the same claim, cycle by
// cycle, sharing no code with it.
//
// The claim: rtl/bcmc_observer.v reproduces, on every clock, the running /
// column / visit_valid / done / aborted that validation/observer_hw.py
// produced for the same inputs. Nothing in this file computes an expected
// value; it reads them out of sim/vectors/obshw_*.txt.
//
// What this file does NOT check, and why
// --------------------------------------
// `column_bits`. The projection's mathematics is bcmc_column.v's, already held
// to reference.py exhaustively by sim/bcmc_column_test.cpp and tb_column.v, and
// the *wiring* of the observer's instance is checked by the Verilator harness
// against the R matrix lines. Repeating it here would cost this file a flat
// vector, a parameter that has to agree with another file, and no new claim.
//
// It also cannot check `visit_q` directly: it is not a port. Where it is
// observable -- while `valid` is high, where it equals visit_valid -- the
// comparison below sees it. Where it is not observable, its consequences are
// checked instead: no visit is presented, the pass aborts, and no visit occurs
// afterwards without a new start.
//
// Usage:
//     iverilog -g2005 -Wall -Wno-timescale -s tb_observer -o tb_observer.vvp \
//              tb_observer.v ../rtl/bcmc_observer.v ../rtl/bcmc_column.v \
//              ../rtl/bcmc_cell.v
//     vvp tb_observer.vvp +vectors=vectors/obshw_edge.txt [+vcd]
//===========================================================================

`timescale 1ns / 1ps

`default_nettype none

module tb_observer;

    localparam VAL_W = 16;
    localparam IDX_W = 16;
    localparam MAX_C = 16;

    reg                   clk = 1'b0;
    reg                   rst = 1'b0;
    reg                   start = 1'b0;
    reg                   trigger = 1'b0;
    reg                   oneshot = 1'b0;
    reg                   valid = 1'b0;
    reg [VAL_W-1:0]       N = {VAL_W{1'b0}};
    reg [IDX_W-1:0]       C = {IDX_W{1'b0}};
    reg [MAX_C*VAL_W-1:0] weights_flat = {MAX_C*VAL_W{1'b0}};
    reg [MAX_C*VAL_W-1:0] offsets_flat = {MAX_C*VAL_W{1'b0}};

    wire [VAL_W-1:0]      column;
    wire                  visit_valid;
    wire                  done;
    wire                  running;
    wire                  aborted;
    wire [MAX_C-1:0]      column_bits;

    bcmc_observer #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W),
        .MAX_C (MAX_C)
    ) dut (
        .clk          (clk),
        .rst          (rst),
        .start        (start),
        .trigger      (trigger),
        .oneshot      (oneshot),
        .N            (N),
        .C            (C),
        .valid        (valid),
        .weights_flat (weights_flat),
        .offsets_flat (offsets_flat),
        .column       (column),
        .visit_valid  (visit_valid),
        .column_bits  (column_bits),
        .done         (done),
        .running      (running),
        .aborted      (aborted)
    );

    //-----------------------------------------------------------------------
    // The vector file
    //-----------------------------------------------------------------------

    reg [8*256-1:0] vecfile;
    reg [8*256-1:0] line;
    integer         fd;

    integer n_runs;
    integer n_cycles;
    integer errors;

    integer hN;
    integer honeshot;
    integer hcycles;

    integer i_rst, i_start, i_trig, i_valid;
    integer e_run, e_run2, e_col, e_sched, e_visit, e_done, e_abort;
    integer got;
    integer fields;
    integer k;
    integer n_lines;

    // One cycle of one run: drive the recorded inputs, compare every output,
    // then take the rising edge that consumes them.
    task do_cycle;
        input integer idx;
        begin
            got = $fgets(line, fd);
            if (got <= 0) begin
                $display("FAIL tb_observer: run %0d cycle %0d: file ended early",
                         idx, k);
                errors = errors + 1;
            end else begin
                fields = $sscanf(line, "C %d %d %d %d %d %d %d %d %d %d %d",
                                 i_rst, i_start, i_trig, i_valid, e_run, e_run2,
                                 e_col, e_sched, e_visit, e_done, e_abort);
                if (fields != 11) begin
                    $display("FAIL tb_observer: run %0d cycle %0d: malformed C line",
                             idx, k);
                    errors = errors + 1;
                end else begin
                    rst     = i_rst[0];
                    start   = i_start[0];
                    trigger = i_trig[0];
                    valid   = i_valid[0];
                    oneshot = honeshot[0];
                    N       = hN[VAL_W-1:0];
                    #1;

                    n_cycles = n_cycles + 1;

                    if (running !== e_run[0]) begin
                        $display("FAIL tb_observer: run %0d cycle %0d: running %0d, expected %0d",
                                 idx, k, running, e_run);
                        errors = errors + 1;
                    end
                    if (running !== e_run2[0]) begin
                        $display("FAIL tb_observer: run %0d cycle %0d: running != state field",
                                 idx, k);
                        errors = errors + 1;
                    end
                    if (column !== e_col[VAL_W-1:0]) begin
                        $display("FAIL tb_observer: run %0d cycle %0d: column %0d, expected %0d",
                                 idx, k, column, e_col);
                        errors = errors + 1;
                    end
                    if (visit_valid !== e_visit[0]) begin
                        $display("FAIL tb_observer: run %0d cycle %0d: visit_valid %0d, want %0d",
                                 idx, k, visit_valid, e_visit);
                        errors = errors + 1;
                    end
                    if (done !== e_done[0]) begin
                        $display("FAIL tb_observer: run %0d cycle %0d: done %0d, expected %0d",
                                 idx, k, done, e_done);
                        errors = errors + 1;
                    end
                    if (aborted !== e_abort[0]) begin
                        $display("FAIL tb_observer: run %0d cycle %0d: aborted %0d, expected %0d",
                                 idx, k, aborted, e_abort);
                        errors = errors + 1;
                    end

                    // Where visit_q is observable it and visit_valid are one
                    // signal, because visit_valid = visit_q & valid.
                    if (i_valid[0] === 1'b1 && visit_valid !== e_sched[0]) begin
                        $display("FAIL tb_observer: run %0d cycle %0d: visit != sched %0d",
                                 idx, k, visit_valid, e_sched);
                        errors = errors + 1;
                    end
                end
            end

            clk = 1'b1;
            #1;
            clk = 1'b0;
            #1;
        end
    endtask

    //-----------------------------------------------------------------------

    initial begin : main_init

        if (!$value$plusargs("vectors=%s", vecfile)) begin
            $display("FAIL tb_observer: no +vectors=<file> given");
            $finish;
        end

        if ($test$plusargs("vcd")) begin
            $dumpfile("waves/tb_observer.vcd");
            $dumpvars(0, tb_observer);
        end

        fd = $fopen(vecfile, "r");
        if (fd == 0) begin
            $display("FAIL tb_observer: cannot open %0s", vecfile);
            $finish;
        end

        n_runs   = 0;
        n_cycles = 0;
        errors   = 0;
        n_lines  = 0;

        // The file is read a line at a time. Comments and separators carry no
        // numbers and are skipped; an H line begins a run.
        begin : read_loop
            while (!$feof(fd)) begin
                got = $fgets(line, fd);
                if (got <= 0) begin
                    disable read_loop;
                end else begin
                    n_lines = n_lines + 1;
                    // Identify a header by parsing it, not by inspecting a byte
                    // of the packed string: how a simulator justifies the result
                    // of $fgets is not something to build a parser on. A C line,
                    // a comment and a separator all fail to match the literal H
                    // and yield no fields; a malformed H line yields some.
                    fields = $sscanf(line, "H %d %d %d", hN, honeshot, hcycles);
                    if (fields == 3) begin
                        // Reset the engine, then replay this run's cycle lines.
                        rst = 1'b1;
                        clk = 1'b1; #1; clk = 1'b0; #1;
                        clk = 1'b1; #1; clk = 1'b0; #1;
                        rst     = 1'b0;
                        start   = 1'b0;
                        trigger = 1'b0;
                        oneshot = 1'b0;
                        valid   = 1'b0;
                        for (k = 0; k < hcycles; k = k + 1)
                            do_cycle(n_runs);
                        n_runs = n_runs + 1;
                    end else if (fields > 0 && fields < 3) begin
                        $display("FAIL tb_observer: malformed H line in %0s", vecfile);
                        errors = errors + 1;
                    end
                end
            end
        end

        $fclose(fd);

        if (n_runs == 0) begin
            $display("FAIL tb_observer: no runs found in %0s (%0d lines read)",
                     vecfile, n_lines);
            errors = errors + 1;
        end

        if (errors == 0)
            $display("tb_observer: PASS  %0d runs, %0d cycles  (%0s)",
                     n_runs, n_cycles, vecfile);
        else
            $display("tb_observer: FAIL  %0d runs, %0d cycles, %0d errors  (%0s)",
                     n_runs, n_cycles, errors, vecfile);

        $finish;
    end

endmodule

`default_nettype wire
