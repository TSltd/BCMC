//===========================================================================
// tb_observer_wb_smoke.v -- the observer window's first breath
//
// Icarus Verilog, and deliberately small. This bench asks two questions, and
// asks them separately, because they fail differently:
//
//   A. THE CHAIN. Does an accepted Wishbone write reach the engine, and does
//      the engine's answer come back with the frozen one-cycle latency -- not
//      two?
//
//          Wishbone acceptance
//                |
//                v
//          engine start/trigger input
//                |
//                v
//          one cycle
//                |
//                v
//          visit_valid / done / column_bits
//
//      A bus wrapper can introduce an extra cycle and still look perfectly
//      sensible at the register level, so the assertions below are anchored to
//      *which cycle* the response lands in, not merely that a response landed.
//
//   B. THE REFUSAL. An `err` must be `err` on both sides: ERR high and ACK low,
//      AND no register change, no pulse, no engine input, no status mutation.
//      The register map's claim is not "this returns err" but "and leaves no
//      trace", which is half the contract and needs its own check.
//
// The matrix is real, not zeroed, so the projection is exercised too: with
// N = 2, C = 2 and both weights 1 at offset 0, row i is active in column 0 and
// not in column 1 -- because (0 + c) mod 2 < 1 holds only for c = 0. Those two
// expected words come from the mathematics, not from the implementation.
//
// There is no bus-vector file here on purpose. This is the smoke test: it earns
// the right to be wired into the build, and the generated corpus that follows
// it is what actually tests the decode.
//
// Usage:
//     iverilog -g2005 -Wall -Wno-timescale -s tb_observer_wb_smoke \
//              -o build-icarus/tb_observer_wb_smoke.vvp \
//              tb_observer_wb_smoke.v ../rtl/bcmc_obs_wb.v \
//              ../rtl/bcmc_observer.v ../rtl/bcmc_column.v ../rtl/bcmc_cell.v
//     vvp build-icarus/tb_observer_wb_smoke.vvp [+vcd]
//===========================================================================

`timescale 1ns / 1ps

`default_nettype none

module tb_observer_wb_smoke;

    localparam VAL_W = 16;
    localparam IDX_W = 16;
    localparam MAX_C = 16;

    // The context: N = 2, C = 2, both weights 1, both offsets 0. Lane i occupies
    // bits [VAL_W*i +: VAL_W], row 0 in the least significant field, so the flat
    // vector is built by part-select rather than by a nested replication that
    // Icarus's -g2005 parser declines.
    localparam [VAL_W-1:0] N_VAL = 16'd2;
    localparam [IDX_W-1:0] C_VAL = 16'd2;

    reg [MAX_C*VAL_W-1:0] W_FLAT;
    reg [MAX_C*VAL_W-1:0] O_FLAT;

    integer lane;
    initial begin : build_context
        W_FLAT = {MAX_C*VAL_W{1'b0}};
        O_FLAT = {MAX_C*VAL_W{1'b0}};
        if (C_VAL > 0) W_FLAT[0*VAL_W +: VAL_W] = 16'd1;
        if (C_VAL > 1) W_FLAT[1*VAL_W +: VAL_W] = 16'd1;
    end

    localparam [11:0] OBS_ID     = 12'h000;
    localparam [11:0] OBS_CAPS   = 12'h008;
    localparam [11:0] OBS_CTRL   = 12'h00C;
    localparam [11:0] OBS_STATUS = 12'h010;
    localparam [11:0] OBS_PASS   = 12'h014;

    localparam [31:0] CTRL_START   = 32'h1;
    localparam [31:0] CTRL_STEP    = 32'h2;
    localparam [31:0] CTRL_RESET   = 32'h4;
    localparam [31:0] CTRL_ONESHOT = 32'h8;

    integer errors;
    integer checks;

    reg wb_clk_i;
    reg wb_rst_i;
    reg [11:0] wb_adr_i;
    reg [31:0] wb_dat_i;
    reg [3:0] wb_sel_i;
    reg wb_we_i;
    reg wb_stb_i;
    reg wb_cyc_i;

    reg obs_valid_i;

    wire [31:0] wb_dat_o;
    wire wb_ack_o;
    wire wb_err_o;
    wire [VAL_W-1:0] column_o;
    wire visit_valid_o;
    wire [MAX_C-1:0] column_bits_o;
    wire done_o;
    wire running_o;
    wire aborted_o;

    bcmc_obs_wb #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W),
        .MAX_C (MAX_C)
    ) dut (
        .wb_clk_i (wb_clk_i),
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
        .obs_n_i (N_VAL),
        .obs_c_i (C_VAL),
        .obs_valid_i (obs_valid_i),
        .obs_weights_flat_i (W_FLAT),
        .obs_offsets_flat_i (O_FLAT),
        .column_o (column_o),
        .visit_valid_o (visit_valid_o),
        .column_bits_o (column_bits_o),
        .done_o (done_o),
        .running_o (running_o),
        .aborted_o (aborted_o)
    );

    always #5 wb_clk_i = ~wb_clk_i;     // 100 MHz

    task fail;
        input [8*96-1:0] what;
        begin
            $display("FAIL tb_observer_wb_smoke: %0s", what);
            errors = errors + 1;
        end
    endtask

    task expect;
        input cond;
        input [8*96-1:0] what;
        begin
            checks = checks + 1;
            if (!cond) fail(what);
        end
    endtask

    //-------------------------------------------------------------------------

    reg [31:0] status_before;

    initial begin
        errors = 0;
        checks = 0;

        wb_clk_i = 1'b0;
        wb_rst_i = 1'b1;
        wb_adr_i = 12'd0;
        wb_dat_i = 32'd0;
        wb_sel_i = 4'd0;
        wb_we_i  = 1'b0;
        wb_stb_i = 1'b0;
        wb_cyc_i = 1'b0;
        obs_valid_i = 1'b0;

        if ($test$plusargs("vcd")) begin
            $dumpfile("waves/tb_observer_wb_smoke.vcd");
            $dumpvars(0, tb_observer_wb_smoke);
        end

        repeat (4) @(negedge wb_clk_i);
        wb_rst_i = 1'b0;
        repeat (2) @(negedge wb_clk_i);

        // ---- the identity and the geometry --------------------------------
        wb_read(OBS_ID);
        expect(rd_val == 32'h4F425356, "OBS_ID is not OBSV");
        wb_read(OBS_CAPS);
        expect(rd_val == ((16 << 24) | (16 << 16) | 16), "OBS_CAPS geometry");

        // ---- 1. the sideband becomes valid: the matrix now exists ----------
        obs_valid_i = 1'b1;
        @(negedge wb_clk_i);

        // ---- 2. START, and the whole chain, in the accepting cycle ---------
        // The response cycle IS the visit cycle: acceptance reaches the engine
        // and the engine answers one cycle later. An extra cycle anywhere in
        // the wrapper fails these, which is the point.
        wb_req(1'b1, OBS_CTRL, CTRL_ONESHOT | CTRL_START);
        expect(wb_ack_o,   "START: no ack");
        expect(!wb_err_o,  "START: unexpected err");
        expect(running_o,  "START: RUNNING not high in the ack cycle");
        expect(visit_valid_o, "START: no visit in the ack cycle");
        expect(column_o == 16'd0, "START: the first visit is not pi(0)");
        expect(column_bits_o == {{(MAX_C-2){1'b0}}, 2'b11},
               "START: column_bits are not M(:,0) = rows 0,1");
        expect(!done_o, "START: a two-visit pass cannot be done at pi(0)");
        wb_release();

        // ---- 3. STEP, same chain ------------------------------------------
        wb_req(1'b1, OBS_CTRL, CTRL_ONESHOT | CTRL_STEP);
        expect(wb_ack_o,  "STEP: no ack");
        expect(!wb_err_o, "STEP: unexpected err");
        expect(visit_valid_o, "STEP: no visit in the ack cycle");
        expect(column_o == 16'd1, "STEP: the second visit is not pi(1)");
        expect(column_bits_o == {{(MAX_C-2){1'b0}}, 2'b00},
               "STEP: column_bits are not M(:,1) = no rows");
        expect(done_o, "STEP: the closing visit did not assert done");
        wb_release();

        // ---- 4. the pass is over, and DONE is latched ---------------------
        @(negedge wb_clk_i);
        expect(!running_o, "a one-shot pass is still running");
        wb_read(OBS_STATUS);
        expect(rd_val == 32'h2, "STATUS is not DONE-only after the pass");
        wb_read(OBS_PASS);
        expect(rd_val == 32'd1, "PASS did not count the completed pass");

        // ---- 5. RESET stops it and clears the observer --------------------
        wb_req(1'b1, OBS_CTRL, CTRL_RESET);
        expect(wb_ack_o,  "RESET: no ack");
        expect(!wb_err_o, "RESET: unexpected err");
        expect(!running_o, "RESET: still running in the ack cycle");
        wb_release();
        wb_read(OBS_STATUS);
        expect(rd_val == 32'd0, "RESET left STATUS dirty");
        wb_read(OBS_PASS);
        expect(rd_val == 32'd0, "RESET left the pass counter");
        expect(obs_valid_i, "RESET disturbed the sideband; it is not its to touch");

        // ---- 6. refusals, each checked on BOTH sides ----------------------
        wb_write_err(12'h048, 32'd0, "unmapped write");

        wb_read(OBS_STATUS);
        status_before = rd_val;

        // (a) refused START: there is no matrix
        obs_valid_i = 1'b0;
        @(negedge wb_clk_i);
        wb_write_err(OBS_CTRL, CTRL_ONESHOT | CTRL_START, "START while !VALID");
        expect(!running_o, "a refused START started a pass");
        expect(!visit_valid_o, "a refused START produced a visit");
        wb_read(OBS_STATUS);
        expect(rd_val == status_before, "a refused START mutated STATUS");
        wb_read(OBS_CTRL);
        expect(rd_val == 32'd0, "a refused START half-applied ONESHOT");
        obs_valid_i = 1'b1;
        @(negedge wb_clk_i);

        // (b) refused STEP: there is nothing to advance
        wb_write_err(OBS_CTRL, CTRL_ONESHOT | CTRL_STEP, "STEP while IDLE");
        expect(!visit_valid_o, "a refused STEP produced a visit");
        expect(!running_o,     "a refused STEP started a pass");

        // ---- verdict ------------------------------------------------------
        if (errors == 0)
            $display("tb_observer_wb_smoke: PASS  %0d checks", checks);
        else
            $display("tb_observer_wb_smoke: FAIL  %0d checks, %0d errors",
                     checks, errors);

        $finish;
    end

    //-------------------------------------------------------------------------
    // The Wishbone master. `wb_req` drives the request and returns *in the
    // response cycle*, with the bus still held, so the caller can look at the
    // engine's wires in exactly the cycle the response is being reported. That
    // is what makes the one-cycle-latency assertion possible at all.
    //-------------------------------------------------------------------------

    reg [31:0] rd_val;

    task wb_req;
        input        we;
        input [11:0] a;
        input [31:0] d;
        integer      t;
        begin
            @(negedge wb_clk_i);
            wb_adr_i = a;
            wb_dat_i = d;
            wb_we_i  = we;
            wb_sel_i = 4'hF;
            wb_stb_i = 1'b1;
            wb_cyc_i = 1'b1;
            @(negedge wb_clk_i);                 // the response is valid now
            t = 0;
            while (!(wb_ack_o || wb_err_o) && (t < 8)) begin
                t = t + 1;
                @(negedge wb_clk_i);
            end
        end
    endtask

    task wb_release;
        begin
            wb_stb_i = 1'b0;
            wb_cyc_i = 1'b0;
            wb_we_i  = 1'b0;
            @(negedge wb_clk_i);
        end
    endtask

    task wb_read;
        input [11:0] a;
        begin
            wb_req(1'b0, a, 32'd0);
            expect(wb_ack_o, "read: no ack");
            expect(!wb_err_o, "read: unexpected err");
            rd_val = wb_dat_o;
            wb_release();
        end
    endtask

    task wb_write_ok;
        input [11:0] a;
        input [31:0] d;
        begin
            wb_req(1'b1, a, d);
            expect(wb_ack_o, "write: no ack");
            expect(!wb_err_o, "write: unexpected err");
            wb_release();
        end
    endtask

    task wb_write_err;
        input [11:0] a;
        input [31:0] d;
        input [8*64-1:0] what;
        begin
            wb_req(1'b1, a, d);
            expect(wb_err_o,  what);
            expect(!wb_ack_o, what);
            wb_release();
            @(negedge wb_clk_i);      // let any (wrong) side effect land, then
        end
    endtask

endmodule

`default_nettype wire

