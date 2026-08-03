//===========================================================================
// tb_wb.v -- Icarus Verilog testbench for bcmc_wb
//
// The second opinion on the Wishbone wrapper. Verilator's sim/bcmc_wb_test.cpp
// is the primary check; this is a different simulator, driving the same bus,
// reading the same recorded conversations, and making the same claim.
//
// WHAT IS UNDER TEST
//
// bcmc_wb is the top of the tree, so instantiating it drags in everything:
//
//     bcmc_wb -> bcmc_context -> (the weight and offset windows)
//             -> bcmc_core     -> (the transform itself)
//             -> bcmc_column   -> bcmc_cell
//
// There is nothing left inside the fabric to compare against, and nothing here
// that computes a matrix. Every expected value in the vector file came from
// validation/bcmc_periph.py, which is in turn built on validation/reference.py,
// which is what validated the mathematics in the first place.
//
// WHAT A VECTOR FILE IS
//
// Not a table of stimulus and response but a recorded conversation, produced
// by validation/gen_wb_vectors.py. Five ops, one per line:
//
//     Z                                  reset the peripheral
//     L <text>                           a label, free text to end of line
//     R adr sel data ACK|ERR rdata       read, expecting that outcome
//     W adr sel data ACK|ERR rdata       write, expecting that outcome
//     P adr sel mask ACK   value         poll adr until (read & mask) == value
//
// All numbers are hexadecimal. '#' to end of line is a comment. Blank lines
// are skipped. This is exactly what sim/common/vectors.cpp accepts, so both
// simulators are reading the same file the same way.
//
// The P op exists because the Python model has no clock. A recorded STATUS
// read taken just after START cannot be replayed literally against hardware,
// which needs C+4 clocks to answer the same way. What the two share is not the
// value but the wait, so the wait is what gets recorded.
//
// WHAT THIS FILE JUDGES FOR ITSELF
//
// Three things, and they are protocol invariants rather than BCMC facts:
//
//   1. ack and err are never asserted together. Checked every cycle.
//   2. Every access is answered exactly once, inside TIMEOUT clocks. A slave
//      that simply stops talking is a different bug from a slave that refuses,
//      and the message should say which; a slave that answers twice has
//      broken the handshake, and only a master that keeps the request up for
//      one clock past the response can see it.
//   3. irq_o agrees with the registers. At every label boundary, STATUS and
//      CTRL are read back over the bus and irq_o compared against
//      STATUS.IRQ & CTRL.IRQ_EN. The pin is not in the vector format, so its
//      expectation is derived from the device, never remembered.
//
// The RTL's own `ifndef SYNTHESIS assertions are left armed. All traffic here
// was recorded from a model that obeys docs/Register_Map.md, so any access
// that trips one is this testbench's fault and should stop the simulation.
//
// The whole file is replayed NPASS times without an intervening reset of the
// simulation, because each file begins with its own Z. A second pass that
// disagrees with the first has found state the reset does not clear.
//
// Usage:
//     iverilog -g2005 -Wall -Wno-timescale -s tb_wb -o tb_wb.vvp tb_wb.v \
//              ../rtl/bcmc_wb.v ../rtl/bcmc_context.v ../rtl/bcmc_core.v \
//              ../rtl/bcmc_column.v ../rtl/bcmc_cell.v
//     vvp tb_wb.vvp +vectors=vectors/wb_sequences.txt [+vcd]
//===========================================================================

`timescale 1ns / 1ps

`default_nettype none

module tb_wb;

    //-----------------------------------------------------------------------
    // Geometry
    //
    // The vector files were recorded against MAX_C = 64, VAL_W = 16,
    // IDX_W = 16, and say so in their header comment. A comment is not a
    // check, so the S1 probe at the head of every suite reads CAPS: if this
    // instance were built to a different shape, that read would disagree.
    //-----------------------------------------------------------------------

    localparam VAL_W  = 16;
    localparam IDX_W  = 16;
    localparam MAX_C  = 64;

    localparam MAXTOK  = 32;    // bytes in one token; the longest is "ACK"
    localparam MAXLINE = 512;   // bytes in one label
    localparam TIMEOUT = 64;    // clocks to wait for ack or err
    localparam POLLMAX = 4096;  // reads before a poll is called stuck
    localparam NPASS   = 2;     // replays of the whole file
    localparam MAXSHOW = 20;    // failures printed before falling silent

    // The two registers audit_irq consults. Duplicated from
    // docs/Register_Map.md rather than from bcmc_wb.v, so that a wrapper that
    // moved them would be caught rather than followed.
    localparam [11:0] ADR_CTRL   = 12'h00C;
    localparam [11:0] ADR_STATUS = 12'h010;

    localparam CTRL_IRQ_EN_BIT = 1;
    localparam STATUS_IRQ_BIT  = 2;

    //-----------------------------------------------------------------------
    // Clock
    //-----------------------------------------------------------------------

    reg clk = 1'b0;
    always #5 clk = ~clk;

    //-----------------------------------------------------------------------
    // The bus, and the device on the end of it
    //-----------------------------------------------------------------------

    reg         wb_rst_i = 1'b1;
    reg  [11:0] wb_adr_i = 12'h000;
    reg  [31:0] wb_dat_i = 32'h0000_0000;
    reg  [3:0]  wb_sel_i = 4'h0;
    reg         wb_we_i  = 1'b0;
    reg         wb_stb_i = 1'b0;
    reg         wb_cyc_i = 1'b0;

    wire [31:0] wb_dat_o;
    wire        wb_ack_o;
    wire        wb_err_o;
    wire        irq_o;

    bcmc_wb #(
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
        .irq_o    (irq_o)
    );

    //-----------------------------------------------------------------------
    // Bookkeeping
    //-----------------------------------------------------------------------

    integer fd;
    integer errors    = 0;
    integer n_ops     = 0;
    integer n_checks  = 0;
    integer n_access  = 0;
    integer n_clocks  = 0;
    integer pass;

    reg [8*MAXLINE-1:0] vecfile;
    reg [8*MAXLINE-1:0] line_buf;
    reg [8*MAXLINE-1:0] label_str;

    always @(posedge clk) n_clocks = n_clocks + 1;

    // Invariant 1, watched continuously rather than at the point of use. The
    // outputs are registered, so the settled value is the one at the negedge.
    always @(negedge clk) begin
        if (!wb_rst_i && (wb_ack_o === 1'b1) && (wb_err_o === 1'b1)) begin
            report("ack and err asserted together");
        end
    end

    task report;
        input [8*MAXLINE-1:0] msg;
        begin
            errors = errors + 1;
            if (errors <= MAXSHOW) begin
                $display("FAIL tb_wb: %0s [%0s pass %0d] %0s",
                         vecfile, label_str, pass, msg);
            end
            if (errors == MAXSHOW) begin
                $display("     ... further failures suppressed");
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Token reader
    //
    // Whitespace-delimited tokens, '#' to end of line is a comment. The same
    // rules sim/common/vectors.cpp follows.
    //-----------------------------------------------------------------------

    reg [8*MAXTOK-1:0] tok;
    integer            tok_ok;   // 1 = token in tok, 0 = end of file

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

    // Every operand in a bus vector is hexadecimal, at most eight digits.
    task get_hex;
        output [31:0] value;
        integer   k;
        integer   len;
        reg [3:0] digit;
        reg [7:0] ch;
        reg       bad;
        reg [31:0] acc;
        begin
            get_token;
            acc = 32'h0000_0000;
            bad = 1'b0;
            if (tok_ok != 1) begin
                report("unexpected end of file, expected a hex number");
                bad = 1'b1;
            end else begin
                len = str_len(tok);
                if ((len == 0) || (len > 8)) bad = 1'b1;
                for (k = 0; k < len; k = k + 1) begin
                    ch    = tok[8*(len-1-k) +: 8];
                    digit = 4'h0;
                    if      ((ch >= "0") && (ch <= "9")) digit = ch - "0";
                    else if ((ch >= "a") && (ch <= "f")) digit = (ch - "a") + 8'd10;
                    else if ((ch >= "A") && (ch <= "F")) digit = (ch - "A") + 8'd10;
                    else                                 bad   = 1'b1;
                    acc = (acc << 4) | {28'h000_0000, digit};
                end
                if (bad) report("expected a hex number");
            end
            value = acc;
        end
    endtask

    // ACK or ERR, the recorded outcome of an access.
    task get_outcome;
        output want_err;
        begin
            get_token;
            want_err = 1'b0;
            if (tok_ok != 1) begin
                report("unexpected end of file, expected ACK or ERR");
            end else if (tok === "ACK") begin
                want_err = 1'b0;
            end else if (tok === "ERR") begin
                want_err = 1'b1;
            end else begin
                report("expected ACK or ERR");
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // The bus master
    //
    // Classic Wishbone B4: no bursts, no pipelining, a single-cycle response
    // in the cycle after cyc & stb. Inputs change on the falling edge; the
    // response is sampled after the rising edge, because wb_ack_o, wb_err_o
    // and wb_dat_o are registered.
    //
    // Every access is followed by one idle cycle. That is not politeness: the
    // wrapper qualifies a new access with !wb_ack_o && !wb_err_o, so a master
    // that held stb through the response would be ignored for a cycle anyway.
    //
    // Which is half of invariant 2, and this master checks it rather than
    // trusting it: the request is held for one clock beyond the response and
    // that clock is inspected. A slave that had forgotten to qualify a new
    // access would answer the same request twice, and a master that dropped
    // stb the moment it saw ack could never notice. The cost is one clock per
    // access.
    //-----------------------------------------------------------------------

    task drive_idle;
        begin
            wb_adr_i <= 12'h000;
            wb_dat_i <= 32'h0000_0000;
            wb_sel_i <= 4'h0;
            wb_we_i  <= 1'b0;
            wb_stb_i <= 1'b0;
            wb_cyc_i <= 1'b0;
        end
    endtask

    task wb_access;
        input  [11:0] adr;
        input         we;
        input  [31:0] wdata;
        input  [3:0]  sel;
        output        got_err;
        output        got_timeout;
        output        got_dup;
        output [31:0] rdata;
        integer i;
        reg     answered;
        begin
            @(negedge clk);
            wb_adr_i <= adr;
            wb_dat_i <= we ? wdata : 32'h0000_0000;
            wb_sel_i <= sel;
            wb_we_i  <= we;
            wb_stb_i <= 1'b1;
            wb_cyc_i <= 1'b1;

            answered    = 1'b0;
            got_err     = 1'b0;
            got_timeout = 1'b1;
            got_dup     = 1'b0;
            rdata       = 32'h0000_0000;

            for (i = 0; (i < TIMEOUT) && !answered; i = i + 1) begin
                @(posedge clk);
                #1;
                if ((wb_ack_o === 1'b1) || (wb_err_o === 1'b1)) begin
                    answered    = 1'b1;
                    got_timeout = 1'b0;
                    got_err     = wb_err_o;
                    rdata       = wb_dat_o;
                end
            end

            // One more clock with the request still driven -- invariant 2.
            if (answered) begin
                @(posedge clk);
                #1;
                got_dup = (wb_ack_o === 1'b1) || (wb_err_o === 1'b1);
            end

            @(negedge clk);
            drive_idle;
            @(posedge clk);
            n_access = n_access + 1;
        end
    endtask

    task wb_reset;
        begin
            @(negedge clk);
            wb_rst_i <= 1'b1;
            drive_idle;
            repeat (4) @(posedge clk);
            @(negedge clk);
            wb_rst_i <= 1'b0;
            @(posedge clk);
        end
    endtask

    //-----------------------------------------------------------------------
    // Invariant 3: the interrupt pin against the registers that define it
    //
    // Both registers are readable in every state, BUSY included, so this is
    // safe to run at any label boundary. Reading STATUS does not clear it --
    // the IRQ flag is write-one-to-clear -- so the audit is not destructive.
    //-----------------------------------------------------------------------

    task audit_irq;
        reg [31:0] status_val;
        reg [31:0] ctrl_val;
        reg        a_err;
        reg        a_to;
        reg        a_dup;
        reg        expect_irq;
        begin
            wb_access(ADR_STATUS, 1'b0, 32'h0, 4'hF, a_err, a_to, a_dup, status_val);
            if (a_to)  report("no response to the STATUS read of the IRQ audit");
            if (a_err) report("STATUS erred during the IRQ audit");
            if (a_dup) report("the STATUS read of the IRQ audit was answered twice");

            wb_access(ADR_CTRL, 1'b0, 32'h0, 4'hF, a_err, a_to, a_dup, ctrl_val);
            if (a_to)  report("no response to the CTRL read of the IRQ audit");
            if (a_err) report("CTRL erred during the IRQ audit");
            if (a_dup) report("the CTRL read of the IRQ audit was answered twice");

            expect_irq = status_val[STATUS_IRQ_BIT] & ctrl_val[CTRL_IRQ_EN_BIT];
            n_checks   = n_checks + 1;

            if (irq_o !== expect_irq) begin
                report("irq_o disagrees with STATUS.IRQ & CTRL.IRQ_EN");
                if (errors <= MAXSHOW) begin
                    $display("     irq_o = %0b, STATUS = %08h, CTRL = %08h",
                             irq_o, status_val, ctrl_val);
                end
            end
        end
    endtask

    //-----------------------------------------------------------------------
    // Replay
    //-----------------------------------------------------------------------

    reg [8*MAXTOK-1:0] op;
    reg [31:0]         v_adr;
    reg [31:0]         v_sel;
    reg [31:0]         v_data;
    reg [31:0]         v_rdata;
    reg                v_err;

    reg [31:0]         r_data;
    reg                r_err;
    reg                r_to;
    reg                r_dup;

    reg                eof;
    reg                matched;
    integer            poll_i;
    integer            r;

    task do_access_op;
        begin
            wb_access(v_adr[11:0], op === "W", v_data, v_sel[3:0],
                      r_err, r_to, r_dup, r_data);
            n_checks = n_checks + 1;

            if (r_dup) begin
                report("answered twice: the response must be one cycle wide");
                if (errors <= MAXSHOW) begin
                    $display("     %0s %03h sel %h", op, v_adr[11:0], v_sel[3:0]);
                end
            end

            if (r_to) begin
                report("no ack or err within the timeout");
                if (errors <= MAXSHOW) begin
                    $display("     %0s %03h sel %h", op, v_adr[11:0], v_sel[3:0]);
                end
            end else if (r_err !== v_err) begin
                report("wrong outcome");
                if (errors <= MAXSHOW) begin
                    $display("     %0s %03h sel %h: expected %0s, got %0s",
                             op, v_adr[11:0], v_sel[3:0],
                             v_err ? "ERR" : "ACK", r_err ? "ERR" : "ACK");
                end
            end else if ((op === "R") && !v_err && (r_data !== v_rdata)) begin
                report("wrong data");
                if (errors <= MAXSHOW) begin
                    $display("     R %03h: read %08h, bcmc_periph.py says %08h",
                             v_adr[11:0], r_data, v_rdata);
                end
            end
        end
    endtask

    // The poll op. The mask is in the data field and the value it must reach
    // is in the rdata field, so the wait is replayed rather than the moment.
    task do_poll_op;
        begin
            matched = 1'b0;
            for (poll_i = 0; (poll_i < POLLMAX) && !matched; poll_i = poll_i + 1) begin
                wb_access(v_adr[11:0], 1'b0, 32'h0, v_sel[3:0],
                          r_err, r_to, r_dup, r_data);
                n_checks = n_checks + 1;
                if (r_dup) begin
                    report("answered twice while polling");
                    matched = 1'b1;
                end else if (r_to) begin
                    report("no ack or err while polling");
                    matched = 1'b1;
                end else if (r_err) begin
                    report("polling a register that erred");
                    matched = 1'b1;
                end else if ((r_data & v_data) === v_rdata) begin
                    matched = 1'b1;
                end
            end
            if (!matched) begin
                report("a polled register never reached its value");
                if (errors <= MAXSHOW) begin
                    $display("     P %03h: (read & %08h) never became %08h",
                             v_adr[11:0], v_data, v_rdata);
                end
            end
        end
    endtask

    initial begin
        if (!$value$plusargs("vectors=%s", vecfile)) begin
            $display("FAIL tb_wb: no +vectors=<file> given");
            $finish;
        end

        if ($test$plusargs("vcd")) begin
            $dumpfile("waves/tb_wb.vcd");
            $dumpvars(0, tb_wb);
        end

        label_str = "before the first label";

        for (pass = 0; pass < NPASS; pass = pass + 1) begin
            fd = $fopen(vecfile, "r");
            if (fd == 0) begin
                $display("FAIL tb_wb: cannot open %0s", vecfile);
                $finish;
            end

            // A known state to start from, whatever the previous pass left.
            wb_reset;

            eof   = 1'b0;
            n_ops = 0;

            while (eof == 1'b0) begin
                get_token;
                if (tok_ok != 1) begin
                    eof = 1'b1;
                end else if (tok === "Z") begin
                    wb_reset;
                    n_ops = n_ops + 1;
                end else if (tok === "L") begin
                    // The pin is audited under the OLD label, because that is
                    // the section whose effect is being left behind.
                    audit_irq;
                    r = $fgets(line_buf, fd);
                    if (line_buf[7:0] == 8'h0A) line_buf = line_buf >> 8;
                    label_str = line_buf;
                    n_ops     = n_ops + 1;
                end else if ((tok === "R") || (tok === "W") || (tok === "P")) begin
                    op = tok;
                    get_hex(v_adr);
                    get_hex(v_sel);
                    get_hex(v_data);
                    get_outcome(v_err);
                    get_hex(v_rdata);
                    n_ops = n_ops + 1;
                    if (op === "P") do_poll_op;
                    else            do_access_op;
                end else begin
                    report("unknown op");
                    eof = 1'b1;
                end
            end

            audit_irq;
            $fclose(fd);

            if (n_ops == 0) begin
                $display("FAIL tb_wb: no ops found in %0s", vecfile);
                errors = errors + 1;
            end
        end

        if (errors == 0) begin
            $display({"tb_wb: PASS  %0d ops, %0d checks, %0d accesses, ",
                      "%0d clocks, %0d passes  (%0s)"},
                     n_ops, n_checks, n_access, n_clocks, NPASS, vecfile);
        end else begin
            $display("tb_wb: FAIL  %0d ops, %0d errors  (%0s)",
                     n_ops, errors, vecfile);
        end

        $finish;
    end

endmodule

`default_nettype wire
