//===========================================================================
// tb_row.v -- Icarus Verilog testbench for bcmc_row
//
// The second opinion on the row projection. Verilator's sim/bcmc_row_test.cpp
// is the primary check; this is a different simulator reading the same vector
// files, and it makes the same two claims:
//
//   A. bcmc_row agrees with validation/reference.py, bit for bit.
//   B. bcmc_row is nothing but replicated bcmc_cell. A SEPARATE bcmc_cell is
//      instantiated below, alongside the row, and every bit of the row is
//      compared against it. The cell is the primitive; the row is replication.
//
// Because a whole matrix arrives in each case, the matrix-level statements are
// checked too, entirely on bits the RTL produced:
//
//   * Row conservation:  popcount(row i) == weights[i].
//   * Balance Theorem:   popcount(column j) == q + 1 for j < r, else q, with
//                        W = qN + r recomputed here from the weights alone.
//
// This testbench invents no expected value. The bits come from the R lines of
// the vector file, the occupancies from its L line, and the only arithmetic
// performed here is the division W = qN + r that the theorem is about.
//
// Order invariance: the rows of each case are visited forwards, backwards and
// shuffled. bcmc_row is combinational, so all three must agree; that is how
// "no state" is tested rather than asserted. Unlike tb_cell.v this testbench
// streams one case at a time -- a matrix is large, and reordering within a case
// is already enough to expose a module that remembered its last query.
//
// Usage:
//     iverilog -g2005 -Wall -Wno-timescale -s tb_row -o tb_row.vvp \
//              tb_row.v ../rtl/bcmc_row.v ../rtl/bcmc_cell.v
//     vvp tb_row.vvp +vectors=vectors/matrix_edge.txt [+vcd]
//===========================================================================

`timescale 1ns / 1ps

`default_nettype none

module tb_row;

    //-----------------------------------------------------------------------
    // Geometry. MAX_N and MAX_C must be at least as large as the widest case
    // in the vector files; gen_vectors.py keeps the matrix suites within 32.
    //-----------------------------------------------------------------------

    localparam VAL_W  = 16;
    localparam MAX_N  = 32;
    localparam MAX_C  = 32;
    localparam MAXTOK = 64;   // bytes in one token; a row string is <= MAX_N
    localparam NPASS  = 3;    // forwards, backwards, shuffled

    //-----------------------------------------------------------------------
    // The device under test
    //
    // Inputs start as x. Every application poisons them again first, so a
    // module that latched a previous value would show it as x, not as a stale
    // but plausible number.
    //-----------------------------------------------------------------------

    reg  [VAL_W-1:0] N      = {VAL_W{1'bx}};
    reg  [VAL_W-1:0] weight = {VAL_W{1'bx}};
    reg  [VAL_W-1:0] offset = {VAL_W{1'bx}};

    wire [MAX_N-1:0] row_bits;

    bcmc_row #(
        .VAL_W (VAL_W),
        .MAX_N (MAX_N)
    ) dut (
        .N        (N),
        .weight   (weight),
        .offset   (offset),
        .row_bits (row_bits)
    );

    //-----------------------------------------------------------------------
    // The separate cell: claim B's right-hand side
    //-----------------------------------------------------------------------

    reg  [VAL_W-1:0] c_N      = {VAL_W{1'bx}};
    reg  [VAL_W-1:0] c_weight = {VAL_W{1'bx}};
    reg  [VAL_W-1:0] c_offset = {VAL_W{1'bx}};
    reg  [VAL_W-1:0] c_column = {VAL_W{1'bx}};

    wire             c_bit;

    bcmc_cell #(
        .VAL_W (VAL_W)
    ) ref_cell (
        .N       (c_N),
        .weight  (c_weight),
        .offset  (c_offset),
        .column  (c_column),
        .bit_out (c_bit)
    );

    //-----------------------------------------------------------------------
    // One case, as read from the vector file
    //-----------------------------------------------------------------------

    integer         case_N;
    integer         case_C;

    reg  [VAL_W-1:0] cw  [0:MAX_C-1];          // W line
    reg  [VAL_W-1:0] co  [0:MAX_C-1];          // O line
    reg  [MAX_N-1:0] py  [0:MAX_C-1];          // R lines, bit j = column j
    integer          occ [0:MAX_N-1];          // L line

    reg  [MAX_N-1:0] got [0:MAX_C-1];          // what the RTL produced
    integer          order [0:MAX_C-1];        // the visiting order

    //-----------------------------------------------------------------------
    // Bookkeeping
    //-----------------------------------------------------------------------

    integer fd;
    integer errors      = 0;
    integer n_cases     = 0;
    integer n_evals     = 0;
    integer n_bits      = 0;   // bits compared against the separate cell
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

    // One R line: a string of '0' and '1', character 0 being column 0.
    task get_row_bits;
        input  integer          expect_len;
        output [MAX_N-1:0]      bits;
        integer k, len;
        reg [7:0] ch;
        begin
            bits = {MAX_N{1'b0}};
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
                for (k = 0; k < len && k < MAX_N; k = k + 1) begin
                    ch = tok[8*(len-1-k) +: 8];
                    if (ch == "1")      bits[k] = 1'b1;
                    else if (ch == "0") bits[k] = 1'b0;
                    else begin
                        $display("FAIL %0s: row bits must be 0 or 1, got %0s",
                                 vecfile, tok);
                        errors = errors + 1;
                    end
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
        reg [MAX_N-1:0] bits;
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

            // Preconditions on the file, checked before anything is driven:
            // a case outside the specification is a broken vector file, not a
            // broken module. bcmc_row asserts nothing itself, by design.
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
                get_row_bits(case_N, bits);
                py[i] = bits;
            end

            expect_keyword("L");
            for (i = 0; i < case_N; i = i + 1) begin
                get_number(v);
                occ[i] = v;
            end

            ok = 1;
        end
    endtask

    //-----------------------------------------------------------------------
    // Driving
    //-----------------------------------------------------------------------

    task apply_row;
        input [VAL_W-1:0] an;
        input [VAL_W-1:0] aw;
        input [VAL_W-1:0] ao;
        begin
            N      = {VAL_W{1'bx}};
            weight = {VAL_W{1'bx}};
            offset = {VAL_W{1'bx}};
            #1;
            N      = an;
            weight = aw;
            offset = ao;
            #1;
            n_evals = n_evals + 1;
        end
    endtask

    task apply_cell;
        input [VAL_W-1:0] an;
        input [VAL_W-1:0] aw;
        input [VAL_W-1:0] ao;
        input [VAL_W-1:0] ac;
        begin
            c_N      = {VAL_W{1'bx}};
            c_weight = {VAL_W{1'bx}};
            c_offset = {VAL_W{1'bx}};
            c_column = {VAL_W{1'bx}};
            #1;
            c_N      = an;
            c_weight = aw;
            c_offset = ao;
            c_column = ac;
            #1;
        end
    endtask

    //-----------------------------------------------------------------------
    // The visiting order for the rows of a case
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
    // One pass over a case
    //-----------------------------------------------------------------------

    task check_case;
        input integer pass;
        integer k, i, j, pop, load, W, q, r;
        reg [MAX_N-1:0] bits;
        begin
            build_order(pass);

            for (k = 0; k < case_C; k = k + 1) begin
                i = order[k];

                apply_row(case_N[VAL_W-1:0], cw[i], co[i]);
                bits   = row_bits;
                got[i] = bits;

                for (j = 0; j < case_N; j = j + 1) begin
                    // Claim A: RTL == reference.py.
                    if (bits[j] !== py[i][j]) begin
                        $display("FAIL %0s: row %0d column %0d disagrees with reference.py",
                                 vecfile, i, j);
                        $display("     pass %0d  N=%0d w=%0d o=%0d  got %b, expected %b",
                                 pass, case_N, cw[i], co[i], bits[j], py[i][j]);
                        errors = errors + 1;
                    end

                    // Claim B: RTL == a separately instantiated bcmc_cell.
                    apply_cell(case_N[VAL_W-1:0], cw[i], co[i], j[VAL_W-1:0]);
                    n_bits = n_bits + 1;
                    if (bits[j] !== c_bit) begin
                        $display("FAIL %0s: row %0d column %0d is not what bcmc_cell says",
                                 vecfile, i, j);
                        $display("     N=%0d w=%0d o=%0d  row %b, cell %b",
                                 case_N, cw[i], co[i], bits[j], c_bit);
                        errors = errors + 1;
                    end
                end

                // Lanes above N do not exist in the matrix and must read 0.
                for (j = case_N; j < MAX_N; j = j + 1) begin
                    if (bits[j] !== 1'b0) begin
                        $display("FAIL %0s: row %0d lane %0d is above N = %0d but reads %b",
                                 vecfile, i, j, case_N, bits[j]);
                        errors = errors + 1;
                    end
                end

                // Row conservation (Lemma 1), on RTL bits.
                pop = 0;
                for (j = 0; j < case_N; j = j + 1)
                    if (bits[j] === 1'b1) pop = pop + 1;
                if (pop != cw[i]) begin
                    $display("FAIL %0s: row conservation: row %0d has popcount %0d, weight %0d",
                             vecfile, i, pop, cw[i]);
                    errors = errors + 1;
                end
            end

            // The Balance Theorem, on the matrix the RTL just assembled.
            W = 0;
            for (i = 0; i < case_C; i = i + 1) W = W + cw[i];
            q = W / case_N;
            r = W % case_N;

            for (j = 0; j < case_N; j = j + 1) begin
                load = 0;
                for (i = 0; i < case_C; i = i + 1)
                    if (got[i][j] === 1'b1) load = load + 1;

                if (load != ((j < r) ? q + 1 : q)) begin
                    $display("FAIL %0s: Balance Theorem: column %0d carries %0d",
                             vecfile, j, load);
                    $display("     W = %0d = %0d*%0d + %0d predicts %0d",
                             W, q, case_N, r, (j < r) ? q + 1 : q);
                    errors = errors + 1;
                end
                if (load != occ[j]) begin
                    $display("FAIL %0s: column %0d occupancy %0d disagrees with reference.py's %0d",
                             vecfile, j, load, occ[j]);
                    errors = errors + 1;
                end
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Main
    //-----------------------------------------------------------------------

    integer pass;
    integer more;

    initial begin
        if (!$value$plusargs("vectors=%s", vecfile)) begin
            $display("FAIL tb_row: no +vectors=<file> given");
            $finish;
        end

        if ($test$plusargs("vcd")) begin
            $dumpfile("waves/tb_row.vcd");
            $dumpvars(0, tb_row);
        end

        fd = $fopen(vecfile, "r");
        if (fd == 0) begin
            $display("FAIL tb_row: cannot open %0s", vecfile);
            $finish;
        end

        more = 1;
        while (more == 1) begin
            read_case(more);
            if (more == 1) begin
                n_cases   = n_cases + 1;
                shuf_seed = 32'h42434D43 ^ n_cases;
                for (pass = 0; pass < NPASS; pass = pass + 1)
                    check_case(pass);
            end
        end

        $fclose(fd);

        if (n_cases == 0) begin
            $display("FAIL tb_row: no cases found in %0s", vecfile);
            errors = errors + 1;
        end

        if (errors == 0) begin
            $display("tb_row: PASS  %0d matrices, %0d evals, %0d cell bits, %0d passes  (%0s)",
                     n_cases, n_evals, n_bits, NPASS, vecfile);
        end else begin
            $display("tb_row: FAIL  %0d matrices, %0d errors  (%0s)",
                     n_cases, errors, vecfile);
        end

        $finish;
    end

endmodule

`default_nettype wire
