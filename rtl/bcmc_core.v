//===========================================================================
// bcmc_core.v -- BCMC Core: the canonical prefix transform
//
//     weights[]  ->  offsets[]
//
// This module is the complete BCMC Core. It is a transform, not an
// accelerator: it has no GPIO, no timers, no observers, no matrix output, no
// traversal and no bus interface. Its only state is the prefix accumulator
// and the row index.
//
// The mathematics is
//
//     offset[0]   = 0
//     offset[i+1] = (offset[i] + weight[i]) mod N
//
// which is a recursion, so the hardware is a recursion too: one weight in,
// one offset out, per clock. There is deliberately no combinational variant.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//
// See docs/Hardware_Architecture.md for the BCMC Prefix Stream Interface,
// including the authoritative cycle-by-cycle timing diagram, and rtl/README.md
// for the design rationale.
//===========================================================================

`default_nettype none

module bcmc_core #(
    // N, every weight and every offset all fit in VAL_W bits.
    parameter VAL_W = 16,
    // C, the number of rows, fits in IDX_W bits.
    parameter IDX_W = 16
) (
    input  wire              clk,
    input  wire              rst,          // synchronous, active high

    //-----------------------------------------------------------------------
    // Framing
    //-----------------------------------------------------------------------
    input  wire              start,        // begin a transform; samples N, C
    output wire              busy,         // a transform is in progress
    output wire              done,         // 1-cycle pulse: stream complete

    //-----------------------------------------------------------------------
    // Parameters, sampled at start and held for the whole transform
    //-----------------------------------------------------------------------
    input  wire [VAL_W-1:0]  N,            // row length,  N >= 1
    input  wire [IDX_W-1:0]  C,            // number of rows

    //-----------------------------------------------------------------------
    // Weight stream in
    //-----------------------------------------------------------------------
    input  wire [VAL_W-1:0]  weight_in,    // 0 <= weight_in <= N
    input  wire              weight_valid,

    //-----------------------------------------------------------------------
    // Offset stream out
    //-----------------------------------------------------------------------
    output reg  [VAL_W-1:0]  offset_out,
    output reg               offset_valid
);

    //-----------------------------------------------------------------------
    // Control state
    //-----------------------------------------------------------------------

    localparam [1:0] ST_IDLE = 2'd0;
    localparam [1:0] ST_RUN  = 2'd1;
    localparam [1:0] ST_DONE = 2'd2;

    reg [1:0] state;

    //-----------------------------------------------------------------------
    // Datapath state
    //
    // offset_q is the entire mathematical state of the transform: it holds
    // o_i. row_q counts accepted weights and exists only so that the control
    // logic knows when to return to idle.
    //-----------------------------------------------------------------------

    reg [VAL_W-1:0] offset_q;
    reg [IDX_W-1:0] row_q;

    reg [VAL_W-1:0] n_q;      // latched N
    reg [IDX_W-1:0] c_q;      // latched C

    //-----------------------------------------------------------------------
    // Framing outputs
    //
    // Both are functions of `state` alone. `state` is a register, so these
    // are registered outputs; there is no combinational path from any input.
    //-----------------------------------------------------------------------

    assign busy = (state == ST_RUN);
    assign done = (state == ST_DONE);

    //-----------------------------------------------------------------------
    // Acceptance
    //
    // A weight is accepted only while running and only while rows remain.
    //-----------------------------------------------------------------------

    wire rows_left = (row_q != c_q);
    wire accept    = (state == ST_RUN) && weight_valid && rows_left;

    //-----------------------------------------------------------------------
    // The reduction modulo N is not a division
    //
    // The specification guarantees 0 <= weight <= N and the invariant
    // 0 <= offset_q < N, hence
    //
    //     offset_q + weight <= (N-1) + N < 2N,
    //
    // so one extra bit of headroom suffices and the reduction is a single
    // comparison and a single subtraction. This is the hypothesis w_i <= N of
    // Lemma 2 (docs/Proof.md) appearing directly as hardware.
    //-----------------------------------------------------------------------

    // One extra bit of headroom is enough to hold offset_q + weight_in.
    wire [VAL_W:0] sum       = {1'b0, offset_q} + {1'b0, weight_in};
    wire           overflows = (sum >= {1'b0, n_q});

    // When overflows holds, sum - N < N, so the difference fits in VAL_W bits
    // and the truncated subtraction below is exact.
    wire [VAL_W-1:0] wrapped = sum[VAL_W-1:0] - n_q;

    wire [VAL_W-1:0] next_offset =
        overflows ? wrapped : sum[VAL_W-1:0];

    //-----------------------------------------------------------------------
    // Sequential logic
    //-----------------------------------------------------------------------

    always @(posedge clk) begin
        if (rst) begin
            state        <= ST_IDLE;
            offset_q     <= {VAL_W{1'b0}};
            row_q        <= {IDX_W{1'b0}};
            n_q          <= {VAL_W{1'b0}};
            c_q          <= {IDX_W{1'b0}};
            offset_out   <= {VAL_W{1'b0}};
            offset_valid <= 1'b0;
        end else begin
            // Single-cycle by default; overridden below on acceptance.
            offset_valid <= 1'b0;

            case (state)

                //-----------------------------------------------------------
                ST_IDLE: begin
                    if (start) begin
                        n_q      <= N;
                        c_q      <= C;
                        offset_q <= {VAL_W{1'b0}};   // offset[0] = 0
                        row_q    <= {IDX_W{1'b0}};
                        state    <= ST_RUN;
                    end
                end

                //-----------------------------------------------------------
                ST_RUN: begin
                    if (accept) begin
                        // Emit before update. This ordering is what makes
                        // offset[0] = 0 a structural property of the
                        // datapath rather than a special case in control.
                        offset_out   <= offset_q;
                        offset_valid <= 1'b1;

                        offset_q     <= next_offset;
                        row_q        <= row_q + {{(IDX_W-1){1'b0}}, 1'b1};
                    end

                    // All C weights accepted. The final offset is on the bus
                    // during this cycle, so `done` is raised in the next one:
                    // `done` therefore means "the entire offset stream has
                    // been emitted", with no ambiguity.
                    if (!rows_left) begin
                        state <= ST_DONE;
                    end
                end

                //-----------------------------------------------------------
                ST_DONE: begin
                    // `done` is asserted combinationally from this state, so
                    // the pulse is exactly one cycle wide.
                    state <= ST_IDLE;
                end

                //-----------------------------------------------------------
                default: begin
                    state <= ST_IDLE;
                end

            endcase
        end
    end

    //-----------------------------------------------------------------------
    // Preconditions
    //
    // These are preconditions of the BCMC specification, not behaviours of
    // this module. Violating them puts the input outside the hypothesis of
    // the Balance Theorem, where the construction is undefined -- see
    // "Necessity of the hypothesis w_i <= N" in docs/Proof.md. The module
    // does not attempt to cope; it reports the violation in simulation.
    //-----------------------------------------------------------------------

`ifndef SYNTHESIS
    always @(posedge clk) begin
        if (!rst) begin
            if (start && (N == {VAL_W{1'b0}})) begin
                $display("bcmc_core: ERROR N = 0 at start (N >= 1 required)");
                $stop;
            end
            if (start && (state == ST_RUN)) begin
                $display("bcmc_core: ERROR start asserted while busy");
                $stop;
            end
            if (weight_valid && (state != ST_RUN)) begin
                $display("bcmc_core: ERROR weight_valid asserted while not busy");
                $stop;
            end
            if (weight_valid && (state == ST_RUN) && !rows_left) begin
                $display("bcmc_core: ERROR more than C weights presented");
                $stop;
            end
            if (accept && (weight_in > n_q)) begin
                $display("bcmc_core: ERROR weight %0d exceeds N = %0d",
                         weight_in, n_q);
                $stop;
            end
            // The datapath invariant that makes the single subtraction valid.
            if ((state == ST_RUN) && (offset_q >= n_q)) begin
                $display("bcmc_core: ERROR invariant offset < N violated (%0d >= %0d)",
                         offset_q, n_q);
                $stop;
            end
        end
    end
`endif

endmodule

`default_nettype wire
