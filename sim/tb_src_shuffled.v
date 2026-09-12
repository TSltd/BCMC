//===========================================================================
// tb_src_shuffled.v -- the shuffled source, replayed from its corpus under Icarus
//
//     vectors/srcshuf_edge.txt  ->  rtl/bcmc_src_shuffled.v  ->  compared
//
// The independent second opinion for the buffered source. It shares no code with
// validation/ and does not import it: it reads the corpus as text and checks the
// module's three declared outputs.
//
// What it compares, and why the mapping is derived rather than recorded
// --------------------------------------------------------------------
// The corpus records the **bank stream** -- bank 0, bank 1, ... in the order the
// fills complete them -- and this bench works out which bank each pass received
// from what it observes:
//
//     the pass matches bank b+1  ->  a new bank was handed in, so underrun is 0
//     the pass matches bank b    ->  the last bank repeated,    so underrun is 1
//
// Both the permutation and the flag are checked, and they must agree. Recording a
// permutation per pass instead would have encoded the fill's timing, because
// *which* bank a pass receives depends on whether the fill finished; section 5.4
// promises no cycle count for a fill, so there is no such thing to encode.
//
// Three run kinds
// ---------------
//     q  qualified: `underrun` must stay 0, so every pass must take a new bank
//     u  underrun: either is allowed, and the flag must say which happened
//     n  never ready: `ready` must stay 0 (an unservable N), however long we wait
//
// Readiness is POLLED, never counted: the corpus says how long to wait at most,
// not how long a fill takes.
//
// What is deliberately NOT checked
// --------------------------------
// The bank states, the fill's progress, the written-mask, the shuffle index: none
// is a port, and none is the contract. The bank-isolation invariant is an
// assertion inside the module -- it is reachable only through the interface for
// the ordinary path, and reachable in *no* path if a mutation breaks it, which is
// why it is not left to a bench to notice.
//
// A mismatch is reported with the ask and both candidate values, once per pass:
// the failure mode to distinguish is "ts_pi reads as the identity", which looks
// like every ask of every pass being wrong at once.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`timescale 1ns/1ps
`default_nettype none

module tb_src_shuffled;

    localparam VAL_W   = 16;
    localparam BANK_N  = 256;      // matches BANK_N_MAX
    localparam MAXB    = 8;        // banks a single run may name
    localparam MAXWAIT = 60000;    // a poll bound, not a schedule

    reg                clk;
    reg                rst;
    reg                load;
    reg  [VAL_W-1:0]   N;
    reg  [31:0]        seed;
    reg  [VAL_W-1:0]   ts_t;

    wire [VAL_W-1:0]   ts_pi;
    wire               ready;
    wire               underrun;

    bcmc_src_shuffled #(
        .VAL_W       (VAL_W),
        .BANK_N_MAX  (BANK_N),
        .LEAD        (2)
    ) dut (
        .clk      (clk),
        .rst      (rst),
        .N        (N),
        .seed     (seed),
        .load     (load),
        .ts_t     (ts_t),
        .ts_pi    (ts_pi),
        .ready    (ready),
        .underrun (underrun)
    );

    initial clk = 1'b0;
    always #5 clk = ~clk;

    integer         fd;
    integer         n;
    integer         j;
    integer         t;
    integer         g;
    reg [8*8-1:0]   tag;
    reg [8*80-1:0]  rname;
    reg [8*200-1:0] junk;

    integer         rN;
    integer         rrst;
    reg [31:0]      rseed;
    integer         kind;          // 0 = q, 1 = u, 2 = n
    integer         nbank;         // banks named so far in this run
    integer         cur;           // the bank the last pass took, -1 = none
    integer         idle;
    integer         gap;
    integer         cycles;
    integer         indist;        // the pass's two candidate banks are identical

    reg [VAL_W-1:0] exp_bank [0:MAXB*BANK_N-1];

    integer n_runs = 0;
    integer n_passes = 0;
    integer n_asks = 0;
    integer n_checks = 0;
    integer n_errors = 0;
    integer n_waited = 0;

    //-----------------------------------------------------------------------

    task clock1;
        begin
            @(posedge clk);
            #1;
        end
    endtask

    task fail;
        input [8*100-1:0] msg;
        begin
            $display("  FAIL tb_src_shuffled: %0s", msg);
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
                $display("  FAIL tb_src_shuffled: %0s: got %0d, want %0d",
                         what, got, want);
                n_errors = n_errors + 1;
            end
        end
    endtask

    // Poll `ready`. `want` is the value it must reach; for the unservable run it
    // must never arrive, so the whole window is a check.
    task poll_ready;
        input integer want;          // 1: must become high, 0: must stay low
        input integer maxw;
        integer w;
        begin
            w = 0;
            while (w < maxw && (ready !== 1'b1)) begin
                clock1();
                w = w + 1;
            end
            n_waited = n_waited + w;
            n_checks = n_checks + 1;
            if (want == 1) begin
                if (ready !== 1'b1) fail("ready never asserted within the bound");
            end else begin
                if (ready !== 1'b0) fail("ready asserted for an unservable N");
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // The scan
    //-----------------------------------------------------------------------

    integer k, v, saw_new, saw_rep, new_ok, rep_ok, pass, first_bad;
    reg [8*2-1:0] kindstr;

    initial begin : main

        if (!$value$plusargs("vectors=%s", junk)) begin
            $display("FAIL tb_src_shuffled: no +vectors=<file> given");
            $finish;
        end
        fd = $fopen(junk, "r");
        if (fd == 0) begin
            $display("FAIL tb_src_shuffled: cannot open %0s", junk);
            $finish;
        end

        rst   = 1'b1;
        load  = 1'b0;
        N     = {VAL_W{1'b0}};
        seed  = 32'd0;
        ts_t  = {VAL_W{1'b0}};
        kind  = 0;
        nbank = 0;
        cur   = -1;

        begin : scan
            while (!$feof(fd)) begin
                tag = 0;
                n = $fscanf(fd, "%s", tag);
                if (n != 1) disable scan;

                if (tag == "#") begin
                    n = $fgets(junk, fd);

                end else if (tag == "R") begin
                    n = $fscanf(fd, "%s", rname);
                    n = $fscanf(fd, "%s", kindstr);
                    n = $fscanf(fd, "%d", rN);
                    n = $fscanf(fd, "%h", rseed);
                    n = $fscanf(fd, "%d", rrst);
                    n_runs = n_runs + 1;
                    kind = (kindstr == "q") ? 0 : ((kindstr == "u") ? 1 : 2);

                    N     = rN[VAL_W-1:0];
                    seed  = rseed;
                    nbank = 0;
                    cur   = -1;
                    ts_t  = {VAL_W{1'b0}};

                    rst = 1'b1;
                    for (j = 0; j < rrst; j = j + 1) clock1();
                    rst = 1'b0;

                    poll_ready(kind == 2 ? 0 : 1, MAXWAIT);
                    if (kind != 2) begin
                        n_checks = n_checks + 1;
                        if (underrun !== 1'b0) fail("underrun set at start-up");
                    end

                end else if (tag == "B") begin
                    n = $fscanf(fd, "%d", k);
                    if (k >= MAXB) begin
                        fail("a run names more banks than MAXB");
                        disable scan;
                    end
                    for (t = 0; t < N; t = t + 1) begin
                        n = $fscanf(fd, "%d", v);
                        exp_bank[k*BANK_N + t] = v[VAL_W-1:0];
                    end
                    nbank = nbank + 1;

                end else if (tag == "P") begin
                    n = $fscanf(fd, "%d", pass);
                    n = $fscanf(fd, "%d", idle);
                    n = $fscanf(fd, "%d", gap);
                    n_passes = n_passes + 1;

                    // The idle: ts_t held at 0, which is what the engine drives in
                    // IDLE. The first such cycle after a pass's last ask (N-1) is
                    // the *transition* into 0, and that is the pass boundary.
                    ts_t = {VAL_W{1'b0}};
                    for (j = 0; j < idle; j = j + 1) begin
                        clock1();
                        n_checks = n_checks + 1;
                        if (ready !== 1'b1) fail("ready low in the middle of a run");
                        if (kind == 0 && underrun !== 1'b0)
                            fail("underrun in a qualified run");
                    end

                    // The asks: 0, 1, .., N-1, each held `gap` cycles.
                    saw_new = 0;
                    saw_rep = 0;
                    first_bad = -1;
                    // For N = 1 every bank is [0], so "a new bank was taken" and
                    // "the same bank came round again" are the *same* observation,
                    // and the verdict below cannot demand exactly one of them.
                    // `indist` marks a pass whose two candidate banks are
                    // identical, so the distinction the corpus normally tests is
                    // unobservable here -- a property of N = 1, not a fault in the
                    // source. The asks are still checked in full.
                    indist = ((cur >= 0) && ((cur + 1) < nbank)) ? 1 : 0;
                    for (t = 0; t < N; t = t + 1) begin
                        ts_t = t[VAL_W-1:0];
                        for (g = 0; g < gap; g = g + 1) begin
                            #1;
                            n_asks = n_asks + 1;
                            n_checks = n_checks + 1;
                            if (ready !== 1'b1) fail("ready low during a pass");
                            if (cur < 0) begin
                                if (ts_pi !== exp_bank[0*BANK_N + t]) begin
                                    if (first_bad < 0) begin
                                        first_bad = t;
                                        $display("    pass %0d ask %0d: ts_pi %0d, bank 0 has %0d",
                                                 pass, t, ts_pi,
                                                 exp_bank[0*BANK_N + t]);
                                    end
                                    fail("the first pass is not bank 0");
                                end
                            end else begin
                                new_ok = ((cur + 1) < nbank) &&
                                         (ts_pi === exp_bank[(cur+1)*BANK_N + t]);
                                rep_ok = (ts_pi === exp_bank[cur*BANK_N + t]);
                                if (((cur + 1) < nbank) &&
                                    (exp_bank[(cur+1)*BANK_N + t] !==
                                     exp_bank[cur*BANK_N + t])) indist = 0;
                                if (new_ok) saw_new = 1;
                                else if (rep_ok) saw_rep = 1;
                                else begin
                                    if (first_bad < 0) begin
                                        first_bad = t;
                                        $display("    pass %0d ask %0d: ts_pi %0d",
                                                 pass, t, ts_pi);
                                        $display("      bank %0d has %0d, bank %0d has %0d",
                                                 cur, exp_bank[cur*BANK_N + t],
                                                 cur + 1,
                                                 exp_bank[(cur+1)*BANK_N + t]);
                                    end
                                    fail("ts_pi is neither the new bank nor the repeat");
                                end
                            end
                            clock1();
                        end
                    end

                    // The verdict: exactly one of the two, and the flag must agree.
                    if (cur < 0) begin
                        n_checks = n_checks + 1;
                        if (kind == 0 && underrun !== 1'b0)
                            fail("the first pass reported an underrun");
                        cur = 0;
                    end else if (saw_new && !saw_rep) begin
                        n_checks = n_checks + 1;
                        if (underrun !== 1'b0)
                            fail("a new bank was taken but underrun says otherwise");
                        cur = cur + 1;
                    end else if (saw_rep && !saw_new) begin
                        n_checks = n_checks + 1;
                        if (kind == 0) fail("a repeat in a qualified run");
                        if (underrun !== 1'b1)
                            fail("a repeat without underrun -- the flag must say so");
                    end else if (indist) begin
                        // Both candidate banks are the same permutation, so the
                        // new/repeat distinction carries no information and the
                        // flag cannot be attributed to either. The asks were
                        // checked above; this counts the pass as consistent.
                        n_checks = n_checks + 1;
                    end else begin
                        fail("the pass matched neither bank, or both");
                    end

                end else if (tag == "D") begin
                    n = $fscanf(fd, "%d", cycles);
                    ts_t = {VAL_W{1'b0}};
                    for (j = 0; j < cycles; j = j + 1) begin
                        clock1();
                        n_checks = n_checks + 1;
                        if (kind == 2) begin
                            if (ready !== 1'b0)
                                fail("ready asserted for an unservable N");
                        end else if (ready !== 1'b1) begin
                            fail("ready low while idle");
                        end
                    end

                end else if (tag == "X") begin
                    n = $fscanf(fd, "%d", cycles);
                    // Section 8.2: the engine drives ts_t = 0 in IDLE, and a reset
                    // does not suspend that. Leaving ts_t at its last ask would
                    // make the next pass's ask for step 0 look like a wrap.
                    ts_t = {VAL_W{1'b0}};
                    rst = 1'b1;
                    for (j = 0; j < cycles; j = j + 1) clock1();
                    rst = 1'b0;
                    cur = -1;                    // a reset restarts the stream
                    poll_ready(kind == 2 ? 0 : 1, MAXWAIT);
                    n_checks = n_checks + 1;
                    if (underrun !== 1'b0) fail("a reset did not clear underrun");

                end else if (tag == "L") begin
                    n = $fscanf(fd, "%h", rseed);
                    seed = rseed;
                    ts_t = {VAL_W{1'b0}};      // as above: IDLE drives 0
                    load = 1'b1;
                    clock1();
                    load = 1'b0;
                    cur = -1;                    // a load restarts the stream
                    poll_ready(kind == 2 ? 0 : 1, MAXWAIT);

                end else if (tag == "E") begin
                    n_checks = n_checks + 1;
                    if (ready !== 1'b0) fail("ready asserted where it should be low");

                end else begin
                    $display("  FAIL tb_src_shuffled: unknown or misplaced record");
                    n_errors = n_errors + 1;
                    disable scan;
                end
            end
        end

        $fclose(fd);

        if (n_runs == 0) begin
            $display("FAIL tb_src_shuffled: no runs found in %0s", junk);
            n_errors = n_errors + 1;
        end

        if (n_errors == 0) begin
            $display("tb_src_shuffled: PASS  %0d runs, %0d passes, %0d asks, %0d checks",
                     n_runs, n_passes, n_asks, n_checks);
            $display("                   (%0d fill cycles waited in total)", n_waited);
        end else begin
            $display("tb_src_shuffled: FAIL  %0d runs, %0d passes, %0d errors",
                     n_runs, n_passes, n_errors);
        end

        $finish;
    end

endmodule

`default_nettype wire
