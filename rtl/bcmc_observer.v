//===========================================================================
// bcmc_observer.v -- the sequential observer engine (v2.0a)
//
//     (start, trigger, N, valid)  ->  (column, visit_valid, done, aborted)
//
// This module holds a CURSOR, and nothing else. It contains no BCMC
// mathematics and no traversal *mathematics*: the ordering is a source behind
// a seam (docs/Hardware_Observer_Architecture.md section 3.3), and in v2.0a
// that source is the identity, which costs one wire. The matrix is not here
// either -- it is evaluated combinationally by the bcmc_column instance below,
// which the observer merely *drives*.
//
// It is the golden model validation/observer_hw.py made real, statement for
// statement, and where the two disagree the document is right and one of them
// is a bug. The harness compares them cycle by cycle, not visit by visit.
//
// What the engine decides
// -----------------------
// Three things, in the precedence order of section 3.11:
//
//     reset                      -> IDLE, clean
//     RUN and not valid          -> abort, and say so
//     IDLE and start and valid   -> begin a pass at pi(0)     (requires N >= 1)
//     RUN, last visit, oneshot   -> end the pass
//     RUN and trigger and valid  -> advance one step
//
// and nothing else. Every other input combination changes no state.
//
// The one line that matters most
// ------------------------------
//     assign visit_valid = visit_q & valid;
//
// The gate is on the OUTPUT, not inside the state machine, so a visit cannot
// be presented in the same cycle the context becomes invalid (section 3.7).
// That is the hardware form of the register map's E4. The abort is separate
// and synchronous: the FSM decides the *state*, this assign decides what the
// wires show, and neither substitutes for the other.
//
// The traversal-source seam
// -------------------------
// v2.0c replaces the identity with an affine or a shuffled source. That is a
// change to how col_q is *computed*, not to this state machine, which is why
// the column is a register of its own rather than an alias of t_q.
//
// What is deliberately NOT here
// -----------------------------
// No duplicated FSM logic for the sake of "temporal assertions". The timing
// contract -- a request in cycle k is a visit in k+1, and an invalidated
// context stops visits and aborts the pass -- is checked in
// sim/bcmc_observer_hw_test.cpp by comparing every cycle against
// observer_hw.py. Restating the FSM here to police it would create a second
// implementation of the same rule, the exact thing rtl/bcmc_wb.v refuses to do
// with the characteristic function. The simulation checks below are local
// invariants only: they restate nothing.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_observer #(
    parameter VAL_W = 16,     // N, a column index, and a stored value
    parameter IDX_W = 16,     // C and a row index
    parameter MAX_C = 16      // rows the Evaluator can hold
) (
    input  wire                   clk,
    input  wire                   rst,          // synchronous, active high

    //--- control -- the observer's own register window ----------------------
    input  wire                   start,        // one-cycle pulse: begin a pass
    input  wire                   trigger,      // one-cycle pulse: advance
    input  wire                   oneshot,      // 1: stop at done; 0: wrap

    //--- observation sideband -- from bcmc_wb, outputs only -----------------
    input  wire [VAL_W-1:0]       N,
    input  wire [IDX_W-1:0]       C,
    input  wire                   valid,        // STATUS.VALID
    input  wire [MAX_C*VAL_W-1:0] weights_flat,
    input  wire [MAX_C*VAL_W-1:0] offsets_flat,

    //--- to the output engine (v2.0b) ---------------------------------------
    output wire [VAL_W-1:0]       column,       // pi(t), presented for one cycle
    output wire                   visit_valid,  // one cycle; never while !valid
    output wire [MAX_C-1:0]       column_bits,  // M(:, column), combinational
    output wire                   done,         // one cycle, with the last visit
    output wire                   running,      // a pass is in progress
    output wire                   aborted,      // sticky: restart required

    //--- the traversal seam (v2.0c, section 8.2) ----------------------------
    output wire [VAL_W-1:0]       ts_t,         // the step being asked about
    input  wire [VAL_W-1:0]       ts_pi          // pi(ts_t), from the source
);

    localparam STATE_IDLE = 1'b0;
    localparam STATE_RUN  = 1'b1;

    //-----------------------------------------------------------------------
    // State: one FSM bit and one cursor (section 3.2)
    //-----------------------------------------------------------------------

    reg             state_q;
    reg [VAL_W-1:0] t_q;        // the cursor, 0 .. N-1
    reg [VAL_W-1:0] col_q;      // pi(t_q): the presented column
    reg             visit_q;    // "a visit is due this cycle"
    reg             aborted_q;  // sticky until the next accepted start

    // N-1, computed once. N = 0 is refused at the door (below), so inside a
    // pass N >= 1 always and this is a real last index.
    wire [VAL_W-1:0] last     = N - {{(VAL_W-1){1'b0}}, 1'b1};
    wire             last_now = (t_q == last);

    // The next cursor, wrapping at the pass boundary: one comparison and one
    // conditional select, the same shape the cell uses for its mod N.
    wire [VAL_W-1:0] t_next =
        last_now ? {VAL_W{1'b0}} : (t_q + {{(VAL_W-1){1'b0}}, 1'b1});

    //-----------------------------------------------------------------------
    // The seam (v2.0c, section 8.2)
    //
    // The traversal is no longer computed here. The engine asks a source about
    // the step it is about to present and stores the answer -- one ask in, one
    // answer out, both combinational, and no source-side timing in the path.
    //
    // `ts_t` is 0 while IDLE and `t_next` otherwise. NOT simply `t_next`: a pass
    // that starts after another has ended asks about step 0 while its cursor is
    // already at step 0, and driving `t_next` there would ask about step 1.
    //
    // Driving 0 in IDLE is also what lets ONE expression replace TWO. At a start
    // the source answers step 0 by definition, so the old `pi_at_start` path is
    // gone; and at a wrap `t_next` is 0, so the same expression covers the pass
    // boundary. Both used to be special cases here.
    //-----------------------------------------------------------------------

    assign ts_t = (state_q == STATE_IDLE) ? {VAL_W{1'b0}} : t_next;

    //-----------------------------------------------------------------------
    // Combinational outputs (sections 3.6 - 3.8)
    //-----------------------------------------------------------------------

    assign running     = (state_q == STATE_RUN);
    assign column      = col_q;
    assign visit_valid = visit_q & valid;
    assign done        = visit_q & valid & last_now;
    assign aborted     = aborted_q;

    //-----------------------------------------------------------------------
    // The rising edge (section 3.11, in order)
    //-----------------------------------------------------------------------

    always @(posedge clk) begin
        if (rst) begin
            state_q   <= STATE_IDLE;
            t_q       <= {VAL_W{1'b0}};
            col_q     <= {VAL_W{1'b0}};
            visit_q   <= 1'b0;
            aborted_q <= 1'b0;
        end else if (state_q == STATE_RUN && !valid) begin
            // The context is gone, so the pass is no longer a pass of one
            // matrix. End it and record that it did not complete. This cannot
            // fire on a cycle whose visit completed the pass: a completing
            // visit needs visit_valid, which needs valid (section 3.10).
            state_q   <= STATE_IDLE;
            visit_q   <= 1'b0;
            aborted_q <= 1'b1;
        end else if (state_q == STATE_IDLE) begin
            if (start && valid && (N != {VAL_W{1'b0}})) begin
                state_q   <= STATE_RUN;
                t_q       <= {VAL_W{1'b0}};
                col_q     <= ts_pi;
                visit_q   <= 1'b1;
                aborted_q <= 1'b0;      // a restart acknowledges an abort
            end else begin
                visit_q   <= 1'b0;      // N = 0, or !valid, or no start
            end
        end else begin
            // RUN and valid. A one-shot pass ends at its last visit; a trigger
            // on that cycle is ignored, because there is no cursor left to
            // advance (section 3.11, F12).
            if (visit_q && last_now && oneshot) begin
                state_q <= STATE_IDLE;
                visit_q <= 1'b0;
            end else if (trigger) begin
                t_q     <= t_next;
                col_q   <= ts_pi;       // the source's answer for t_next
                visit_q <= 1'b1;
            end else begin
                visit_q <= 1'b0;        // the strobe is one cycle wide
            end
        end
    end

    //-----------------------------------------------------------------------
    // The projection
    //
    // One bcmc_column answers M(i, column) for every row at once. N and C come
    // from the wrapper's registers, and that is safe because writing either
    // clears VALID: a visit is only ever presented while the offsets in the
    // context are the ones this N and this C produced.
    //
    // This is a second *instance* of the same module, not a second
    // implementation of the characteristic function -- see section 4.4. The
    // module is the one the whole project has been checking, and it stays
    // purely combinational: no clock, no reset, no state.
    //-----------------------------------------------------------------------

    bcmc_column #(
        .VAL_W (VAL_W),
        .IDX_W (IDX_W),
        .MAX_C (MAX_C)
    ) u_column (
        .N            (N),
        .C            (C),
        .column       (column),
        .weights_flat (weights_flat),
        .offsets_flat (offsets_flat),
        .column_bits  (column_bits)
    );

    //-----------------------------------------------------------------------
    // Preconditions and local invariants
    //
    // These restate nothing the FSM already decides; they are the properties a
    // reader would otherwise have to hold in their head, made checkable in
    // simulation. The timing contract is deliberately not among them: it is
    // checked by comparing every cycle against observer_hw.py (see the header).
    //-----------------------------------------------------------------------

`ifndef SYNTHESIS
    initial begin
        if (MAX_C < 1) begin
            $display("bcmc_observer: ERROR MAX_C = %0d (MAX_C >= 1 required)", MAX_C);
            $stop;
        end
        if (VAL_W < 1) begin
            $display("bcmc_observer: ERROR VAL_W = %0d (VAL_W >= 1 required)", VAL_W);
            $stop;
        end
    end

    always @(posedge clk) begin
        if (!rst) begin
            // A visit is never presented while the context is invalid (3.7).
            if (visit_valid && !valid) begin
                $display("bcmc_observer: ERROR visit_valid while !valid (E4)");
                $stop;
            end
            // done implies a visit: it marks one, it does not follow one (3.8).
            if (done && !visit_valid) begin
                $display("bcmc_observer: ERROR done without visit_valid");
                $stop;
            end
            // A visit is always a legal column (3.6).
            if (visit_valid && (col_q >= N)) begin
                $display("bcmc_observer: ERROR visit column %0d >= N = %0d",
                         col_q, N);
                $stop;
            end
            // A visit only ever comes from RUN.
            if (visit_valid && state_q == STATE_IDLE) begin
                $display("bcmc_observer: ERROR visit_valid while IDLE");
                $stop;
            end
            // v2.0c: this is `col_q == ts_pi`, the invariant that holds for EVERY
            // source. `col_q == t_q` was its identity-specific corollary, true
            // only because the identity is what v2.0a wires to the seam -- so the
            // check is generalised here, not deleted. Nothing moves into the
            // source: a module that cannot see a cursor, a clock or a column
            // cannot assert anything about one.
            if (state_q == STATE_RUN && col_q !== ts_pi) begin
                $display("bcmc_observer: ERROR column %0d != ts_pi %0d",
                         col_q, ts_pi);
                $stop;
            end
        end
    end
`endif

endmodule

`default_nettype wire
