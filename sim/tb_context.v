//===========================================================================
// tb_context.v -- Icarus Verilog testbench for bcmc_context
//
// The second opinion on the persistent BCMC context. Verilator's
// sim/bcmc_context_test.cpp is the primary check; this is a different
// simulator reading the same vector files and making the same claim.
//
// The claim is unusual for this project, because bcmc_context contains no
// mathematics to check. What is checked instead is that the context is a
// faithful place to keep the canonical prefix representation:
//
//   A. Composition. A real bcmc_core is instantiated below and wired to the
//      context -- weights out of it, offsets back into it -- so the numbers
//      that end up in the offset window are the Core's, not the testbench's.
//      They are then compared against the O line of the vector file, which is
//      validation/reference.py's answer. Nothing here computes an offset.
//
//   B. Two views, one truth. The indexed software ports and the flat vectors
//      the Evaluator reads must agree, lane for lane, over the whole window.
//      The context is the single source of the representation; if the two
//      views could disagree, it would not be.
//
//   C. No stale offsets. Cases are run back to back WITHOUT resetting the
//      context, so a smaller C follows a larger one. OFFSET[C .. MAX_C-1]
//      must read 0 after every transform, as docs/Register_Map.md requires.
//      That is what load_start clearing the whole window is for.
//
//   D. Ownership. While the Core is busy the context reports `loading`, and
//      it releases when the Core pulses done. The traffic driven here is
//      entirely legal, which is deliberate: bcmc_context's `ifndef SYNTHESIS`
//      assertions are left armed, so any illegal access this testbench
//      accidentally produced would stop the simulation. Verilator's harness
//      does the opposite -- it elaborates a second top with -DSYNTHESIS to
//      silence the alarms and inspect the guards behind them. The alarms are
//      watched here; the locks are tested there.
//
// That division of labour is a real limit on this file, so it is worth naming.
// Deleting the `!loading_q` term from the weight write, or the `wr_ok` term
// from the offset write, leaves every check below passing: neither can be
// observed without driving an access the wrapper is forbidden to drive.
// sim/bcmc_context_test.cpp drives exactly those, against the second top.
//
// One thing that does belong here is the read of an index past the end of the
// window, which is legal to ask and must answer 0. It is checked after the
// window has contents, not only after reset -- an out-of-range index truncates
// onto a real lane, so asking when every lane holds 0 would prove nothing.
//
// The bits of the matrix -- the R lines and the L line -- are read and
// skipped. They belong to bcmc_row and bcmc_column; this module stores the
// pair the projections are computed from, and knows nothing of bits.
//
// The weights of each case are written in three orders, forwards, backwards
// and shuffled, because the weight window is addressed storage and the order
// in which a driver fills it cannot matter.
//
// Usage:
//     iverilog -g2005 -Wall -Wno-timescale -s tb_context -o tb_context.vvp \
//              tb_context.v ../rtl/bcmc_context.v ../rtl/bcmc_core.v
//     vvp tb_context.vvp +vectors=vectors/matrix_edge.txt [+vcd]
//===========================================================================

`timescale 1ns / 1ps

`default_nettype none

module tb_context;

    //-----------------------------------------------------------------------
    // Geometry. MAX_N bounds the row strings this testbench is willing to
    // skip; MAX_C is the depth of the context under test.
    //-----------------------------------------------------------------------

    localparam VAL_W  = 16;
    localparam IDX_W  = 16;
    localparam MAX_N  = 32;   // widest matrix in the vector suites
    localparam MAX_C  = 32;   // lanes in the context under test
    localparam MAXTOK = 64;   // bytes in one token; a row string is <= MAX_N
    localparam NPASS  = 3;    // forwards, backwards, shuffled

    localparam FLAT_W = MAX_C * VAL_W;

    //-----------------------------------------------------------------------
    // Clock
    //-----------------------------------------------------------------------

    reg clk = 1'b0;
    always #5 clk = ~clk;

    //-----------------------------------------------------------------------
    // The device under test
    //-----------------------------------------------------------------------

    reg               rst       = 1'b1;

    reg               sw_we     = 1'b0;
    reg  [IDX_W-1:0]  sw_windex = {IDX_W{1'b0}};
    reg  [VAL_W-1:0]  sw_wdata  = {VAL_W{1'b0}};
    reg  [IDX_W-1:0]  sw_rindex = {IDX_W{1'b0}};

    wire [VAL_W-1:0]  sw_weight;
    wire [VAL_W-1:0]  sw_offset;

    reg  [IDX_W-1:0]  core_rindex = {IDX_W{1'b0}};
    reg               load_start  = 1'b0;

    wire [VAL_W-1:0]  core_weight;
    wire              loading;

    wire [FLAT_W-1:0] weights_flat;
    wire [FLAT_W-1:0] offsets_flat;

    //-----------------------------------------------------------------------
    // The Core, and the four wires that join the two modules
    //
    // This is claim A made structurally. The context's offset window can only
    // be filled from bcmc_core's output, because that is the only thing
    // connected to it -- there is no path by which this testbench could place
    // a number of its own choosing into an offset.
    //-----------------------------------------------------------------------

    reg               core_start   = 1'b0;
    reg  [VAL_W-1:0]  core_N       = {VAL_W{1'b0}};
    reg  [IDX_W-1:0]  core_C       = {IDX_W{1'b0}};
    reg               weight_valid = 1'b0;

    wire              core_busy;
    wire              core_done;
    wire [VAL_W-1:0]  core_offset_out;
    wire              core_offset_valid;

    bcmc_context #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W),
        .MAX_C (MAX_C)
    ) dut (
        .clk          (clk),
        .rst          (rst),

        .sw_we        (sw_we),
        .sw_windex    (sw_windex),
        .sw_wdata     (sw_wdata),
        .sw_rindex    (sw_rindex),
        .sw_weight    (sw_weight),
        .sw_offset    (sw_offset),

        .core_rindex  (core_rindex),
        .core_weight  (core_weight),
        .load_start   (load_start),
        .offset_valid (core_offset_valid),
        .offset_in    (core_offset_out),
        .load_done    (core_done),
        .loading      (loading),

        .weights_flat (weights_flat),
        .offsets_flat (offsets_flat)
    );

    bcmc_core #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W)
    ) core (
        .clk          (clk),
        .rst          (rst),
        .start        (core_start),
        .busy         (core_busy),
        .done         (core_done),
        .N            (core_N),
        .C            (core_C),
        .weight_in    (core_weight),      // straight out of the context
        .weight_valid (weight_valid),
        .offset_out   (core_offset_out),  // straight back into it
        .offset_valid (core_offset_valid)
    );

    //-----------------------------------------------------------------------
    // One case, as read from the vector file
    //-----------------------------------------------------------------------

    integer          case_N;
    integer          case_C;

    reg  [VAL_W-1:0] cw [0:MAX_C-1];      // W line
    reg  [VAL_W-1:0] co [0:MAX_C-1];      // O line

    integer          order [0:MAX_C-1];   // the weight-writing order
    reg  [VAL_W-1:0] snap  [0:MAX_C-1];   // weight window before a transform

    //-----------------------------------------------------------------------
    // Bookkeeping
    //-----------------------------------------------------------------------

    integer fd;
    integer errors     = 0;
    integer n_cases    = 0;
    integer n_values   = 0;   // stored values read back and checked
    integer n_loads    = 0;   // transforms driven through the pair
    integer shuf_seed;

    reg [1023:0] vecfile;
    reg [1023:0] line_buf;

    //-----------------------------------------------------------------------
    // Token reader
    //
    // Whitespace-delimited tokens, '#' to end of line is a comment. Exactly
    // the rules sim/common/vectors.cpp follows, so both simulators are reading
    // the same file the same way.
    //-----------------------------------------------------------------------

    reg [8*MAXTOK-1:0] tok;
    integer            tok_ok;    // 1 = token in tok, 0 = end of file

    function [7:0] first_char;
        input [8*MAXTOK-1:0] s;
        integer k;
        reg     found;
        begin
            first_char = 8'h00;
            found      = 1'b0;
            for (k = MAXTOK - 1; k >= 0; k = k - 1) begin
                if (!found && s[8*k +: 8] != 8'h00) begin
                    first_char = s[8*k +: 8];
                    found      = 1'b1;
                end
            end
        end
    endfunction

    function integer str_len;
        input [8*MAXTOK-1:0] s;
        integer k;
        reg     found;
        begin
            str_len = 0;
            found   = 1'b0;
            for (k = MAXTOK - 1; k >= 0; k = k - 1) begin
                if (!found && s[8*k +: 8] != 8'h00) begin
                    str_len = k + 1;
                    found   = 1'b1;
                end
            end
        end
    endfunction

    // -1 for anything that is not a run of decimal digits.
    function integer tok_to_int;
        input [8*MAXTOK-1:0] s;
        integer k, len, digit, acc;
        reg [7:0] ch;
        reg       bad;
        begin
            len = str_len(s);
            acc = 0;
            bad = (len == 0);
            for (k = 0; k < len; k = k + 1) begin
                ch    = s[8*(len-1-k) +: 8];
                digit = ch - "0";
                if (digit < 0 || digit > 9) bad = 1'b1;
                else                        acc = acc * 10 + digit;
            end
            tok_to_int = bad ? -1 : acc;
        end
    endfunction

    task get_token;
        integer r;
        begin
            tok_ok = 0;
            while (tok_ok == 0) begin
                r = $fscanf(fd, "%s", tok);
                if (r != 1) begin
                    tok_ok = 0;
                    disable get_token;          // end of file
                end else if (first_char(tok) == "#") begin
                    r = $fgets(line_buf, fd);   // discard the comment
                end else begin
                    tok_ok = 1;
                end
            end
        end
    endtask

    task expect_keyword;
        input [8*MAXTOK-1:0] want;
        begin
            get_token;
            if (tok_ok != 1) begin
                $display("FAIL %0s: unexpected end of file, expected %0s",
                         vecfile, want);
                errors = errors + 1;
            end else if (tok !== want) begin
                $display("FAIL %0s: expected %0s, got %0s", vecfile, want, tok);
                errors = errors + 1;
            end
        end
    endtask

    task get_number;
        output integer value;
        begin
            get_token;
            if (tok_ok != 1) begin
                $display("FAIL %0s: unexpected end of file, expected a number",
                         vecfile);
                errors = errors + 1;
                value  = 0;
            end else begin
                value = tok_to_int(tok);
                if (value < 0) begin
                    $display("FAIL %0s: expected a number, got %0s",
                             vecfile, tok);
                    errors = errors + 1;
                    value  = 0;
                end
            end
        end
    endtask

    // The bits of a row are not this module's business. The token is still
    // read, and its length still checked, so that a desynchronised reader is
    // caught here rather than misinterpreted as a bad context.
    task skip_row_bits;
        input integer expect_len;
        integer len;
        begin
            get_token;
            if (tok_ok != 1) begin
                $display("FAIL %0s: unexpected end of file, expected row bits",
                         vecfile);
                errors = errors + 1;
            end else begin
                len = str_len(tok);
                if (len != expect_len) begin
                    $display("FAIL %0s: row has %0d bits, expected %0d",
                             vecfile, len, expect_len);
                    errors = errors + 1;
                end
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Reading one case
    //-----------------------------------------------------------------------

    task read_case;
        output integer ok;
        integer i, v;
        begin
            ok = 0;

            get_token;
            if (tok_ok != 1) disable read_case;   // clean end of file

            if (tok !== "N") begin
                $display("FAIL %0s: expected N at the start of a case, got %0s",
                         vecfile, tok);
                errors = errors + 1;
                disable read_case;
            end

            get_number(case_N);
            expect_keyword("C");
            get_number(case_C);

            // Preconditions on the file. A case outside the specification is a
            // broken vector file, not a broken module.
            if (case_N < 1) begin
                $display("FAIL %0s: precondition N >= 1 violated (N = %0d)",
                         vecfile, case_N);
                errors = errors + 1;
            end
            if (case_N > MAX_N) begin
                $display("FAIL %0s: N = %0d exceeds MAX_N = %0d",
                         vecfile, case_N, MAX_N);
                errors = errors + 1;
            end
            if (case_C > MAX_C) begin
                $display("FAIL %0s: C = %0d exceeds MAX_C = %0d",
                         vecfile, case_C, MAX_C);
                errors = errors + 1;
            end

            expect_keyword("W");
            for (i = 0; i < case_C; i = i + 1) begin
                get_number(v);
                if (v > case_N) begin
                    $display("FAIL %0s: row %0d violates weight <= N (weight %0d, N %0d)",
                             vecfile, i, v, case_N);
                    errors = errors + 1;
                end
                cw[i] = v[VAL_W-1:0];
            end

            expect_keyword("O");
            for (i = 0; i < case_C; i = i + 1) begin
                get_number(v);
                if (v >= case_N) begin
                    $display("FAIL %0s: row %0d violates offset < N (offset %0d, N %0d)",
                             vecfile, i, v, case_N);
                    errors = errors + 1;
                end
                co[i] = v[VAL_W-1:0];
            end

            for (i = 0; i < case_C; i = i + 1) begin
                expect_keyword("R");
                skip_row_bits(case_N);
            end

            expect_keyword("L");
            for (i = 0; i < case_N; i = i + 1) get_number(v);

            ok = 1;
        end
    endtask

    //-----------------------------------------------------------------------
    // Access
    //-----------------------------------------------------------------------

    function [VAL_W-1:0] lane;
        input [FLAT_W-1:0] flat;
        input integer      idx;
        begin
            lane = flat[VAL_W*idx +: VAL_W];
        end
    endfunction

    task write_weight;
        input integer          idx;
        input [VAL_W-1:0]      data;
        begin
            sw_we     = 1'b1;
            sw_windex = idx[IDX_W-1:0];
            sw_wdata  = data;
            @(negedge clk);
            sw_we     = 1'b0;
            sw_windex = {IDX_W{1'b0}};
            sw_wdata  = {VAL_W{1'b0}};
        end
    endtask

    // Reading is combinational: point sw_rindex at a lane and settle. Both
    // views of that lane are then compared, which is claim B.
    task read_lane;
        input  integer     idx;
        output [VAL_W-1:0] w;
        output [VAL_W-1:0] o;
        begin
            sw_rindex = idx[IDX_W-1:0];
            #1;
            w = sw_weight;
            o = sw_offset;
            if (idx < MAX_C) begin
                if (w !== lane(weights_flat, idx)) begin
                    $display("FAIL %0s: lane %0d weight: indexed %0d, flat %0d",
                             vecfile, idx, w, lane(weights_flat, idx));
                    errors = errors + 1;
                end
                if (o !== lane(offsets_flat, idx)) begin
                    $display("FAIL %0s: lane %0d offset: indexed %0d, flat %0d",
                             vecfile, idx, o, lane(offsets_flat, idx));
                    errors = errors + 1;
                end
            end
            n_values = n_values + 2;
        end
    endtask

    //-----------------------------------------------------------------------
    // The order in which software fills the weight window
    //-----------------------------------------------------------------------

    task build_order;
        input integer mode;   // 0 forwards, 1 backwards, 2 shuffled
        integer i, j, t;
        begin
            for (i = 0; i < case_C; i = i + 1) begin
                if (mode == 1) order[i] = case_C - 1 - i;
                else           order[i] = i;
            end
            if (mode == 2) begin
                for (i = case_C - 1; i > 0; i = i - 1) begin
                    j = {$random(shuf_seed)} % (i + 1);
                    t        = order[i];
                    order[i] = order[j];
                    order[j] = t;
                end
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // One transform, driven through the pair
    //
    // The weight presented to the Core is never chosen here: core_rindex
    // names a lane and the context answers. The sequencer below is what the
    // bus wrapper of v0.4c will own; this is a hand-rolled stand-in for it.
    //-----------------------------------------------------------------------

    integer wi, kk, guard, waitc, gap_k;
    integer n_offsets;
    reg     done_seen;
    reg     stop_loop;
    integer send;

    task next_gap;
        output integer g;
        begin
            g     = gap_k % 3;      // 0, 1, 2: back to back, and with stalls
            gap_k = gap_k + 1;
        end
    endtask

    task run_transform;
        begin
            core_N = case_N[VAL_W-1:0];
            core_C = case_C[IDX_W-1:0];

            if (loading !== 1'b0) begin
                $display("FAIL %0s: context reports loading before load_start",
                         vecfile);
                errors = errors + 1;
            end

            // Reads are combinational and were taken between edges, so
            // realign before driving anything the modules will sample.
            @(negedge clk);

            // start and load_start together: one command, two modules.
            core_start = 1'b1;
            load_start = 1'b1;
            @(negedge clk);
            core_start = 1'b0;
            load_start = 1'b0;

            wi        = 0;
            n_offsets = 0;
            done_seen = 1'b0;
            stop_loop = 1'b0;
            next_gap(waitc);
            guard = case_C * 8 + 64;
            kk    = 0;

            while ((stop_loop === 1'b0) && (kk < guard)) begin
                kk = kk + 1;

                // Ownership, claim D: whenever the Core is working, the
                // context says so.
                if ((core_busy === 1'b1) && (loading !== 1'b1)) begin
                    $display("FAIL %0s: Core is busy but the context is not loading",
                             vecfile);
                    errors = errors + 1;
                end

                if (core_offset_valid === 1'b1) n_offsets = n_offsets + 1;
                if (core_done === 1'b1)         done_seen = 1'b1;

                send = 0;
                if ((core_busy === 1'b1) && (wi < case_C)) begin
                    if (waitc > 0) waitc = waitc - 1;
                    else           send  = 1;
                end

                // The lane, not the value. The context supplies the value.
                core_rindex  = (send != 0) ? wi[IDX_W-1:0] : {IDX_W{1'b0}};
                weight_valid = (send != 0);

                @(negedge clk);

                if (send != 0) begin
                    wi = wi + 1;
                    next_gap(waitc);
                end
                if (done_seen === 1'b1) stop_loop = 1'b1;
            end

            weight_valid = 1'b0;
            core_rindex  = {IDX_W{1'b0}};

            if (done_seen !== 1'b1) begin
                $display("FAIL %0s: the transform never completed (N %0d, C %0d)",
                         vecfile, case_N, case_C);
                errors = errors + 1;
            end
            if (n_offsets != case_C) begin
                $display("FAIL %0s: %0d offsets streamed, expected C = %0d",
                         vecfile, n_offsets, case_C);
                errors = errors + 1;
            end

            @(negedge clk);

            // load_done was the Core's done pulse, so ownership must be back
            // with software now.
            if (loading !== 1'b0) begin
                $display("FAIL %0s: context still loading after done",
                         vecfile);
                errors = errors + 1;
            end

            n_loads = n_loads + 1;
        end
    endtask

    //-----------------------------------------------------------------------
    // One pass over a case
    //-----------------------------------------------------------------------

    task check_case;
        input integer pass;
        integer i, k;
        reg [VAL_W-1:0] w, o;
        begin
            build_order(pass);

            // --- software fills the weight window -------------------------
            for (k = 0; k < case_C; k = k + 1)
                write_weight(order[k], cw[order[k]]);

            // Read back before anything else touches the context.
            for (i = 0; i < case_C; i = i + 1) begin
                read_lane(i, w, o);
                if (w !== cw[i]) begin
                    $display("FAIL %0s: weight[%0d] read back %0d, wrote %0d",
                             vecfile, i, w, cw[i]);
                    errors = errors + 1;
                end
            end

            // Snapshot the whole weight window, so that the transform can be
            // shown to have left every lane of it alone.
            for (i = 0; i < MAX_C; i = i + 1) begin
                sw_rindex = i[IDX_W-1:0];
                #1;
                snap[i] = sw_weight;
            end

            // --- the Core fills the offset window -------------------------
            run_transform;

            // --- what is in there now -------------------------------------
            for (i = 0; i < MAX_C; i = i + 1) begin
                read_lane(i, w, o);

                if (w !== snap[i]) begin
                    $display("FAIL %0s: the transform changed weight[%0d]: %0d, was %0d",
                             vecfile, i, w, snap[i]);
                    errors = errors + 1;
                end

                if (i < case_C) begin
                    // Claim A: the Core's offsets are reference.py's offsets.
                    if (o !== co[i]) begin
                        $display("FAIL %0s: offset[%0d] is %0d, reference.py says %0d",
                                 vecfile, i, o, co[i]);
                        $display("     N = %0d, C = %0d, pass %0d",
                                 case_N, case_C, pass);
                        errors = errors + 1;
                    end
                end else begin
                    // Claim C: no stale offset from an earlier, larger C.
                    if (o !== {VAL_W{1'b0}}) begin
                        $display("FAIL %0s: offset[%0d] is %0d but %0d >= C = %0d",
                                 vecfile, i, o, i, case_C);
                        errors = errors + 1;
                    end
                end
            end

            // A lane that does not exist reads 0, asked now that the lanes it
            // would truncate onto -- 0 and 7 -- are full.
            check_absent(MAX_C);
            check_absent(MAX_C + 7);
        end
    endtask

    task check_absent;
        input integer idx;
        reg [VAL_W-1:0] w, o;
        begin
            sw_rindex = idx[IDX_W-1:0];
            #1;
            w = sw_weight;
            o = sw_offset;
            if (w !== {VAL_W{1'b0}} || o !== {VAL_W{1'b0}}) begin
                $display("FAIL %0s: lane %0d >= MAX_C = %0d reads %0d / %0d, expected 0 / 0",
                         vecfile, idx, MAX_C, w, o);
                errors = errors + 1;
            end
            n_values = n_values + 2;
        end
    endtask

    //-----------------------------------------------------------------------
    // Main
    //
    // The context is reset once per pass, not once per case. Cases therefore
    // run into each other, which is exactly the situation claim C is about.
    //-----------------------------------------------------------------------

    integer pass;
    integer more;
    integer i0;
    reg [VAL_W-1:0] w0, o0;

    initial begin
        if (!$value$plusargs("vectors=%s", vecfile)) begin
            $display("FAIL tb_context: no +vectors=<file> given");
            $finish;
        end

        if ($test$plusargs("vcd")) begin
            $dumpfile("waves/tb_context.vcd");
            $dumpvars(0, tb_context);
        end

        gap_k = 0;

        for (pass = 0; pass < NPASS; pass = pass + 1) begin
            fd = $fopen(vecfile, "r");
            if (fd == 0) begin
                $display("FAIL tb_context: cannot open %0s", vecfile);
                $finish;
            end

            // --- reset ----------------------------------------------------
            rst = 1'b1;
            @(negedge clk);
            @(negedge clk);
            rst = 1'b0;
            @(negedge clk);

            if (loading !== 1'b0) begin
                $display("FAIL tb_context: loading asserted after reset");
                errors = errors + 1;
            end

            // Reset clears both windows, every lane.
            for (i0 = 0; i0 < MAX_C; i0 = i0 + 1) begin
                read_lane(i0, w0, o0);
                if (w0 !== {VAL_W{1'b0}} || o0 !== {VAL_W{1'b0}}) begin
                    $display("FAIL tb_context: lane %0d after reset: weight %0d, offset %0d",
                             i0, w0, o0);
                    errors = errors + 1;
                end
            end

            sw_rindex = {IDX_W{1'b0}};

            n_cases = 0;
            more    = 1;
            while (more == 1) begin
                read_case(more);
                if (more == 1) begin
                    n_cases   = n_cases + 1;
                    shuf_seed = 32'h43545800 ^ n_cases ^ (pass << 16);
                    check_case(pass);
                end
            end

            $fclose(fd);

            if (n_cases == 0) begin
                $display("FAIL tb_context: no cases found in %0s", vecfile);
                errors = errors + 1;
            end
        end

        if (errors == 0) begin
            $display("tb_context: PASS  %0d contexts, %0d values, %0d passes  (%0s)",
                     n_loads, n_values, NPASS, vecfile);
        end else begin
            $display("tb_context: FAIL  %0d contexts, %0d errors  (%0s)",
                     n_loads, errors, vecfile);
        end

        $finish;
    end

endmodule

`default_nettype wire
