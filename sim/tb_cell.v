//===========================================================================
// tb_cell.v -- self-checking testbench for bcmc_cell, in Verilog
//
// This is the Icarus Verilog path. It reads exactly the same vector files as
// sim/bcmc_cell_test.cpp (the Verilator path), so the RTL is checked against
// the Python reference model by two independent simulators. A disagreement
// between them is a bug in one of the simulators or in the RTL, never in the
// expected answers -- those come from validation/reference.py.
//
// The cell is combinational. There is no clock, no reset, no protocol and no
// sequencing: driving the four inputs *is* the whole of "running" it. What has
// to be checked is therefore much smaller than for the Core, but one thing is
// harder, not easier -- the claim that there is no state at all.
//
// Five independent checks per case:
//
//   1. Preconditions.  The testbench, not the module, owns the hypothesis
//      N >= 1, 0 <= weight <= N, 0 <= offset < N, 0 <= column < N. A vector
//      file that violated it would be a bug in the generator.
//
//   2. RTL == Python.  The bit recorded in the vector file.
//
//   3. RTL == an independent recomputation, done here with Verilog's own `%`
//      operator and no reference to the RTL's conditional-add trick:
//
//          bit = 1  iff  ((column - offset) mod N) < weight
//
//      Verilog's `%` takes the sign of its left operand, so the difference is
//      folded back into [0, N) explicitly: ((d % N) + N) % N.
//
//   4. The output is boolean.  Never x, never z.
//
//   5. Order invariance.  Every suite is evaluated three times -- forwards,
//      backwards, and in a shuffled order -- and all three must produce
//      identical bits. Between evaluations every input is driven to x, so a
//      module that had quietly latched anything would have to survive being
//      poisoned. For a combinational module this is the analogue of the Core's
//      gap invariance: it is how "no history" is tested rather than asserted.
//
// Usage:
//     vvp tb_cell.vvp +vectors=vectors/cell_exhaustive.txt [+vcd]
//
// Suites larger than MAXCASE cases need a bigger array, which is a compile
// time choice in Verilog-2005:
//
//     iverilog -DMAXCASE=400000 ...
//===========================================================================

`timescale 1ns / 1ps
`default_nettype none

`ifndef MAXCASE
  `define MAXCASE 65536
`endif

module tb_cell;

    //-----------------------------------------------------------------------
    // Sizes
    //-----------------------------------------------------------------------

    localparam integer VAL_W   = 16;
    localparam integer MAXCASE = `MAXCASE;   // largest suite this build accepts
    localparam integer MAXTOK  = 32;         // longest token in a vector file
    localparam integer NPASS   = 3;          // forwards, backwards, shuffled

    //-----------------------------------------------------------------------
    // Device under test
    //
    // No clk. No rst. Nothing else. That is the point of the module.
    //-----------------------------------------------------------------------

    reg  [VAL_W-1:0] N      = {VAL_W{1'bx}};
    reg  [VAL_W-1:0] weight = {VAL_W{1'bx}};
    reg  [VAL_W-1:0] offset = {VAL_W{1'bx}};
    reg  [VAL_W-1:0] column = {VAL_W{1'bx}};

    wire             bit_out;

    bcmc_cell #(
        .VAL_W (VAL_W)
    ) dut (
        .N       (N),
        .weight  (weight),
        .offset  (offset),
        .column  (column),
        .bit_out (bit_out)
    );

    //-----------------------------------------------------------------------
    // Case storage
    //-----------------------------------------------------------------------

    reg [VAL_W-1:0] c_n   [0:MAXCASE-1];
    reg [VAL_W-1:0] c_w   [0:MAXCASE-1];
    reg [VAL_W-1:0] c_o   [0:MAXCASE-1];
    reg [VAL_W-1:0] c_c   [0:MAXCASE-1];
    reg             c_py  [0:MAXCASE-1];   // expected bit, from Python
    reg             c_got [0:MAXCASE-1];   // bit from the RTL, first pass

    integer order [0:MAXCASE-1];           // the traversal for this pass

    integer n_cases;
    integer total_evals;
    integer pass_i;
    integer case_index;                    // 1-based, for messages

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

    task get_number;
        output integer value;
        begin
            get_token;
            if (tok_eof) begin
                $display("tb_cell: PARSE ERROR in %0s: unexpected end of file",
                         vec_path);
                $fatal(1);
            end
            if ($sscanf(tok, "%d", value) != 1) begin
                $display("tb_cell: PARSE ERROR in %0s: expected a number, got '%0s'",
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
            $display("tb_cell: FAIL  %0s", msg);
            $display("               case %0d of %0s, pass %0d",
                     case_index, vec_path, pass_i);
            $display("               N = %0d, weight = %0d, offset = %0d, column = %0d",
                     N, weight, offset, column);
            $fatal(1);
        end
    endtask

    //-----------------------------------------------------------------------
    // The characteristic function, computed here with a real modulo
    //
    // Deliberately not the RTL's algorithm. The RTL adds N back conditionally;
    // this divides. If both give the same answer for every legal tuple, the
    // conditional add is justified.
    //-----------------------------------------------------------------------

    function integer ref_bit;
        input integer rn;
        input integer rw;
        input integer ro;
        input integer rc;
        integer d;
        begin
            d = (rc - ro) % rn;      // Verilog: sign follows the dividend
            d = (d + rn) % rn;       // fold into [0, rn)
            ref_bit = (d < rw) ? 1 : 0;
        end
    endfunction

    //-----------------------------------------------------------------------
    // One evaluation
    //
    // Poison first: every input to x, settle, and only then apply the real
    // stimulus. A module holding state would have to recover from that.
    //-----------------------------------------------------------------------

    task evaluate;
        input [VAL_W-1:0] en;
        input [VAL_W-1:0] ew;
        input [VAL_W-1:0] eo;
        input [VAL_W-1:0] ec;
        begin
            N      = {VAL_W{1'bx}};
            weight = {VAL_W{1'bx}};
            offset = {VAL_W{1'bx}};
            column = {VAL_W{1'bx}};
            #1;

            N      = en;
            weight = ew;
            offset = eo;
            column = ec;
            #1;


            total_evals = total_evals + 1;
        end
    endtask

    //-----------------------------------------------------------------------
    // Traversal orders
    //-----------------------------------------------------------------------

    integer shuf_seed;

    task build_order;
        input integer mode;   // 0 forwards, 1 backwards, 2 shuffled
        integer k;
        integer j;
        integer r;
        integer swap;
        begin
            for (k = 0; k < n_cases; k = k + 1) begin
                order[k] = (mode == 1) ? (n_cases - 1 - k) : k;
            end
            if (mode == 2) begin
                // Fisher-Yates, fixed seed: shuffled but reproducible.
                shuf_seed = 32'h42434D43 ^ n_cases;
                for (k = n_cases - 1; k > 0; k = k - 1) begin
                    r = $random(shuf_seed);
                    if (r < 0) r = -r;
                    j = r % (k + 1);
                    swap     = order[k];
                    order[k] = order[j];
                    order[j] = swap;
                end
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Main
    //-----------------------------------------------------------------------

    integer i;
    integer k;
    integer tmp;
    integer got;
    integer expect_py;
    integer expect_ref;
    reg     eof_reached;

    initial begin
        if (!$value$plusargs("vectors=%s", vec_path)) begin
            $display("tb_cell: ERROR no +vectors=<file> given");
            $fatal(1);
        end

        if ($test$plusargs("vcd")) begin
            $dumpfile("waves/tb_cell.vcd");
            $dumpvars(0, tb_cell);
        end

        fd = $fopen(vec_path, "r");
        if (fd == 0) begin
            $display("tb_cell: ERROR cannot open %0s", vec_path);
            $fatal(1);
        end

        //-------------------------------------------------------------------
        // Read the whole suite. Order invariance needs it in memory.
        //-------------------------------------------------------------------

        n_cases     = 0;
        total_evals = 0;
        pass_i      = 0;
        case_index  = 0;
        eof_reached = 1'b0;

        while (eof_reached === 1'b0) begin
            get_token;
            if (tok_eof) begin
                eof_reached = 1'b1;
            end else begin
                if (n_cases >= MAXCASE) begin
                    $display("tb_cell: ERROR %0s holds more than MAXCASE = %0d cases",
                             vec_path, MAXCASE);
                    $display("               rebuild with iverilog -DMAXCASE=<bigger>");
                    $fatal(1);
                end

                if ($sscanf(tok, "%d", tmp) != 1) begin
                    $display("tb_cell: PARSE ERROR in %0s: expected a number, got '%0s'",
                             vec_path, tok);
                    $fatal(1);
                end
                c_n[n_cases] = tmp[VAL_W-1:0];
                case_index   = n_cases + 1;

                get_number(tmp);  c_w[n_cases] = tmp[VAL_W-1:0];
                get_number(tmp);  c_o[n_cases] = tmp[VAL_W-1:0];
                get_number(tmp);  c_c[n_cases] = tmp[VAL_W-1:0];
                get_number(tmp);
                if ((tmp !== 0) && (tmp !== 1)) begin
                    $display("tb_cell: PARSE ERROR in %0s: case %0d has bit = %0d",
                             vec_path, case_index, tmp);
                    $fatal(1);
                end
                c_py[n_cases] = tmp[0];

                //-----------------------------------------------------------
                // Check 1: the hypothesis of the theorem. The testbench owns
                // it because only the testbench knows the inputs are settled.
                //-----------------------------------------------------------
                if (c_n[n_cases] < 1) begin
                    $display("tb_cell: PRECONDITION case %0d of %0s: N = %0d < 1",
                             case_index, vec_path, c_n[n_cases]);
                    $fatal(1);
                end
                if (c_w[n_cases] > c_n[n_cases]) begin
                    $display("tb_cell: PRECONDITION case %0d of %0s: weight = %0d > N = %0d",
                             case_index, vec_path, c_w[n_cases], c_n[n_cases]);
                    $fatal(1);
                end
                if (c_o[n_cases] >= c_n[n_cases]) begin
                    $display("tb_cell: PRECONDITION case %0d of %0s: offset = %0d >= N = %0d",
                             case_index, vec_path, c_o[n_cases], c_n[n_cases]);
                    $fatal(1);
                end
                if (c_c[n_cases] >= c_n[n_cases]) begin
                    $display("tb_cell: PRECONDITION case %0d of %0s: column = %0d >= N = %0d",
                             case_index, vec_path, c_c[n_cases], c_n[n_cases]);
                    $fatal(1);
                end

                //-----------------------------------------------------------
                // The vector file must itself satisfy the definition.
                //-----------------------------------------------------------
                expect_ref = ref_bit(c_n[n_cases], c_w[n_cases],
                                     c_o[n_cases], c_c[n_cases]);
                if (c_py[n_cases] !== expect_ref[0]) begin
                    $display("tb_cell: case %0d of %0s: python %0d, definition %0d",
                             case_index, vec_path, c_py[n_cases], expect_ref);
                    $display("tb_cell: FAIL  vector file disagrees with the definition");
                    $fatal(1);
                end

                n_cases = n_cases + 1;
            end
        end

        $fclose(fd);

        if (n_cases == 0) begin
            $display("tb_cell: ERROR no cases found in %0s", vec_path);
            $fatal(1);
        end

        //-------------------------------------------------------------------
        // Three passes over the same cases, in three different orders.
        //-------------------------------------------------------------------

        for (pass_i = 0; pass_i < NPASS; pass_i = pass_i + 1) begin
            build_order(pass_i);

            for (k = 0; k < n_cases; k = k + 1) begin
                i          = order[k];
                case_index = i + 1;

                evaluate(c_n[i], c_w[i], c_o[i], c_c[i]);

                // Check 4: boolean, never x or z.
                if ((bit_out !== 1'b0) && (bit_out !== 1'b1)) begin
                    fail("bit_out is not 0 or 1");
                end
                got = bit_out ? 1 : 0;

                // Check 2: RTL == Python.
                expect_py = c_py[i] ? 1 : 0;
                if (got !== expect_py) begin
                    $display("               rtl %0d, python %0d", got, expect_py);
                    fail("RTL disagrees with the reference model");
                end

                // Check 3: RTL == an independent modulo.
                expect_ref = ref_bit(c_n[i], c_w[i], c_o[i], c_c[i]);
                if (got !== expect_ref) begin
                    $display("               rtl %0d, definition %0d", got, expect_ref);
                    fail("RTL disagrees with the defining modulo");
                end

                // Check 5: order invariance.
                if (pass_i == 0) begin
                    c_got[i] = bit_out;
                end else if (bit_out !== c_got[i]) begin
                    $display("               this pass %0d, first pass %0b",
                             got, c_got[i]);
                    fail("result depends on evaluation order");
                end
            end
        end

        // Leave the inputs poisoned: nothing downstream may rely on them.
        N      = {VAL_W{1'bx}};
        weight = {VAL_W{1'bx}};
        offset = {VAL_W{1'bx}};
        column = {VAL_W{1'bx}};

        $display("tb_cell: PASS  %0d cases, %0d evaluations, %0d passes  (%0s)",
                 n_cases, total_evals, NPASS, vec_path);
        $finish;
    end

endmodule

`default_nettype wire
