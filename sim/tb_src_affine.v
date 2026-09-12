//===========================================================================
// tb_src_affine.v -- the affine source, replayed from its corpus under Icarus
//
//     vectors/srcaff_edge.txt  ->  rtl/bcmc_src_affine.v  ->  compared
//
// This is the independent second opinion for rtl/bcmc_src_affine.v. It shares no
// code with validation/traversal_sources.py and does not import it: it reads the
// corpus as text and checks the module's declared outputs.
//
// What it compares
// ----------------
//   ready, a_out, b_out     checked while ready is high
//   ts_pi                   checked on every cycle of every walk
//
// `a_out`, `b_out` and `ts_pi` are compared ONLY while `ready` is high. That is
// not a convenience: a source that is not ready is not admitted to the engine
// (E4's `& SEED_READY`), so before readiness those outputs are don't-care by
// construction rather than by tolerance.
//
// Why the corpus is a script and not a table
// ------------------------------------------
// Section 4.4 promises no cycle count for the derivation, so this bench **polls
// `ready`** rather than counting cycles to it. A table of one line per cycle would
// have pinned a schedule the contract disclaims, and a correctly restructured
// implementation would have failed it.
//
// The expected values are computed here, not read here
// ---------------------------------------------------
// `ts_pi` is expected to be `affine_sequence(a, b, N)[t]` -- the CLOSED FORM,
// evaluated with a real multiply and a real modulus in this file -- while the
// module under test computes it with an accumulator and one conditional
// subtract. Two different pieces of arithmetic agreeing is the point; reading
// the expected values out of the corpus would only have restated the generator.
//
// What is deliberately NOT checked
// --------------------------------
// The derivation's internal state: the mask register, the generator state, the
// Euclid working pair, the countdown to readiness. None of them is a port, and
// none is part of the contract. They are the model's business, in validation/.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`timescale 1ns/1ps
`default_nettype none

module tb_src_affine;

    localparam VAL_W   = 16;
    // The longest derivation measured is ~671 cycles; this is generous, and a
    // failure here means the handshake never came rather than that it was slow.
    localparam MAXWAIT = 20000;

    reg               clk;
    reg               rst;
    reg               load;
    reg  [VAL_W-1:0]  N;
    reg  [31:0]       seed;
    reg  [VAL_W-1:0]  ts_t;

    wire [VAL_W-1:0]  ts_pi;
    wire [VAL_W-1:0]  a_out;
    wire [VAL_W-1:0]  b_out;
    wire              ready;

    bcmc_src_affine #(.VAL_W(VAL_W)) dut (
        .clk    (clk),
        .rst    (rst),
        .N      (N),
        .seed   (seed),
        .load   (load),
        .ts_t   (ts_t),
        .ts_pi  (ts_pi),
        .ready  (ready),
        .a_out  (a_out),
        .b_out  (b_out)
    );

    initial clk = 1'b0;
    always #5 clk = ~clk;

    integer         fd;
    integer         n;
    integer         j;
    integer         k;
    reg [8*8-1:0]   tag;
    reg [8*80-1:0]  rname;
    reg [8*160-1:0] junk;

    integer         rN;
    integer         rrst;
    integer         rA;
    integer         rB;
    integer         lflag;
    integer         cnt;
    integer         tv;
    reg  [31:0]     rseed;

    reg [VAL_W-1:0] curA;      // the (a, b, N) the current walk is under
    reg [VAL_W-1:0] curB;
    reg [VAL_W-1:0] curN;

    integer         pulse_at;  // a reset pulse owed to the next run
    integer         pulse_hold;

    integer n_runs = 0;
    integer n_walks = 0;
    integer n_cycles = 0;
    integer n_checks = 0;
    integer n_errors = 0;
    integer n_skipped = 0;
    integer waited_total = 0;

    //-----------------------------------------------------------------------
    // The reference: the closed form, the expensive way
    //-----------------------------------------------------------------------

    function [VAL_W-1:0] closed;
        input [VAL_W-1:0] t;
        input [VAL_W-1:0] aa;
        input [VAL_W-1:0] bb;
        input [VAL_W-1:0] nn;
        reg   [63:0]      prod;
        begin
            prod   = aa * t;               // 64 bits, so nothing is truncated
            closed = (prod + bb) % nn;
        end
    endfunction

    //-----------------------------------------------------------------------
    // Plumbing
    //-----------------------------------------------------------------------

    task clock1;
        begin
            @(posedge clk);
            #1;
        end
    endtask

    task fail;
        input [8*80-1:0] msg;
        begin
            $display("  FAIL tb_src_affine: %0s", msg);
            n_errors = n_errors + 1;
        end
    endtask

    task check;
        input [8*60-1:0] what;
        input [31:0]     got;
        input [31:0]     want;
        begin
            n_checks = n_checks + 1;
            if (got !== want) begin
                $display("  FAIL tb_src_affine: %0s: got %0d (%h), want %0d (%h)",
                         what, got, got, want, want);
                n_errors = n_errors + 1;
            end
        end
    endtask

    // Poll the handshake. `pat` >= 0 asserts rst for `phold` cycles `pat` cycles
    // after the wait begins -- which is one cycle into the derivation, and so
    // mid-preparation for every N, since the shortest measured is nine cycles.
    task wait_ready;
        input integer maxw;
        input integer pat;
        input integer phold;
        integer       w;
        begin
            w = 0;
            while (ready !== 1'b1 && w < maxw) begin
                if (pat >= 0 && w == pat) begin
                    rst = 1'b1;
                    for (j = 0; j < phold; j = j + 1) clock1();
                    rst = 1'b0;
                    w = w + phold;
                end else begin
                    clock1();
                    w = w + 1;
                end
            end
            waited_total = waited_total + w;
            if (ready !== 1'b1) fail("ready never asserted within the bound");
        end
    endtask

    //-----------------------------------------------------------------------
    // The scan
    //-----------------------------------------------------------------------

    initial begin : main

        if (!$value$plusargs("vectors=%s", junk)) begin
            $display("FAIL tb_src_affine: no +vectors=<file> given");
            $finish;
        end

        fd = $fopen(junk, "r");
        if (fd == 0) begin
            $display("FAIL tb_src_affine: cannot open %0s", junk);
            $finish;
        end

        rst        = 1'b1;
        load       = 1'b0;
        N          = {VAL_W{1'b0}};
        seed       = 32'd0;
        ts_t       = {VAL_W{1'b0}};
        curA       = {VAL_W{1'b0}};
        curB       = {VAL_W{1'b0}};
        curN       = {VAL_W{1'b0}};
        pulse_at   = -1;
        pulse_hold = 0;

        begin : scan
            while (!$feof(fd)) begin

                tag = 0;
                n = $fscanf(fd, "%s", tag);
                if (n != 1) disable scan;

                if (tag == "#") begin
                    // A comment. Swallow the rest of the line and read on.
                    n = $fgets(junk, fd);
                    n_skipped = n_skipped + 1;

                end else if (tag == "P") begin
                    // A reset pulse owed to the *next* run, read before it.
                    n = $fscanf(fd, "%d", pulse_at);
                    n = $fscanf(fd, "%d", pulse_hold);

                end else if (tag == "R") begin
                    n = $fscanf(fd, "%s", rname);
                    n = $fscanf(fd, "%d", rN);
                    n = $fscanf(fd, "%h", rseed);
                    n = $fscanf(fd, "%d", rrst);
                    n = $fscanf(fd, "%d", rA);
                    n = $fscanf(fd, "%d", rB);
                    n_runs = n_runs + 1;

                    N    = rN[VAL_W-1:0];
                    seed = rseed;
                    load = 1'b0;
                    ts_t = {VAL_W{1'b0}};   // held at 0: the engine is IDLE
                    curA = rA[VAL_W-1:0];
                    curB = rB[VAL_W-1:0];
                    curN = rN[VAL_W-1:0];

                    rst = 1'b1;
                    for (j = 0; j < rrst; j = j + 1) clock1();
                    rst = 1'b0;

                    wait_ready(MAXWAIT, pulse_at, pulse_hold);
                    pulse_at   = -1;        // owed to one run only
                    pulse_hold = 0;

                    check("a_out after ready", a_out, curA);
                    check("b_out after ready", b_out, curB);

                end else if (tag == "W") begin
                    n = $fscanf(fd, "%d", cnt);
                    n_walks = n_walks + 1;
                    for (k = 0; k < cnt; k = k + 1) begin
                        n = $fscanf(fd, "%d", tv);
                        ts_t = tv[VAL_W-1:0];
                        #1;
                        n_cycles = n_cycles + 1;
                        if (ready !== 1'b1) begin
                            fail("ready dropped in the middle of a walk");
                        end
                        check("ts_pi", ts_pi,
                              closed(tv[VAL_W-1:0], curA, curB, curN));
                        clock1();
                    end

                end else if (tag == "C") begin
                    n = $fscanf(fd, "%d", lflag);
                    n = $fscanf(fd, "%d", rN);
                    n = $fscanf(fd, "%h", rseed);
                    n = $fscanf(fd, "%d", rA);
                    n = $fscanf(fd, "%d", rB);

                    // The change is applied for one cycle, and `load` is a
                    // one-cycle pulse -- the wrapper's job, done here by hand.
                    N    = rN[VAL_W-1:0];
                    seed = rseed;
                    load = (lflag != 0);
                    clock1();
                    load = 1'b0;

                    // Readiness must drop BEFORE anything else can happen: an
                    // (a, b) that no longer pair with N must never be admitted.
                    n_checks = n_checks + 1;
                    if (ready !== 1'b0) fail("ready did not drop after the change");

                    curA = rA[VAL_W-1:0];
                    curB = rB[VAL_W-1:0];
                    curN = rN[VAL_W-1:0];

                    ts_t = {VAL_W{1'b0}};
                    wait_ready(MAXWAIT, -1, 0);
                    check("a_out after recovery", a_out, curA);
                    check("b_out after recovery", b_out, curB);

                end else begin
                    $display("  FAIL tb_src_affine: unknown record '%0s'", tag);
                    n_errors = n_errors + 1;
                    disable scan;
                end
            end
        end

        $fclose(fd);

        if (n_runs == 0) begin
            $display("FAIL tb_src_affine: no runs found in %0s", junk);
            n_errors = n_errors + 1;
        end

        if (n_errors == 0) begin
            $display("tb_src_affine: PASS  %0d runs, %0d walks, %0d walk cycles, %0d checks",
                     n_runs, n_walks, n_cycles, n_checks);
            $display("                (%0d derivation cycles waited in total)",
                     waited_total);
        end else begin
            $display("tb_src_affine: FAIL  %0d runs, %0d walk cycles, %0d errors",
                     n_runs, n_cycles, n_errors);
        end

        $finish;
    end

endmodule
