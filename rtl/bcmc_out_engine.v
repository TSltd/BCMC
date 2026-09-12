//===========================================================================
// bcmc_out_engine.v -- the output engine (v2.0b)
//
//     (column_bits, visit_valid, valid)  ->  pins
//
// It turns a visited column into a physical effect. This is the whole of
// docs/Output_Engine.md sections 1 to 6, which is five lines:
//
//     on rst:              pattern <= 0
//     on falling valid:    pattern <= 0
//     on visit_valid:      pattern <= column_bits
//     otherwise:           pattern unchanged
//     pins                 = pattern, gated by valid
//
// Two signals, two meanings, and the whole design hangs on not confusing them:
// `pattern_q` is the *latched* pattern and `pins_o` is the *presented* one. They
// differ in exactly the cycles where the matrix that produced the pattern no
// longer exists, which is the same relationship `rtl/bcmc_observer.v` draws one
// layer up between `visit_q` and `visit_valid`.
//
// The line that matters most
// --------------------------
// The destroy on a falling `valid` is not redundant with the presentation gate,
// and the specification nearly shipped without it. Gating alone handles the cycle
// the matrix disappears, but not what comes next:
//
//     a pass completes        pattern = P, valid = 1, pins = P
//     software writes a weight   valid -> 0        pins idle      (the gate)
//     the Core recomputes        valid -> 1
//     ...                        pins = P again    (the gate has no objection)
//
// The gate's premise ("VALID means a matrix exists") is true again, while the
// latch's premise ("this pattern came from that matrix") is not. So the latch is
// destroyed with its matrix, and `valid_q` exists for that one purpose. This is
// the hardware form of the rule the transaction sequences already state: an
// evaluation returning the old matrix after a weight change is incorrect.
//
// What it deliberately does not have
// ----------------------------------
// No `done`, no `aborted`, no `running`, no `column`, no `N`, no weights, no
// offsets, no bus, no `trigger`, no seed, no source selector. Each absence is a
// decision recorded in section 2.2, and two of them are structural arguments
// rather than omissions:
//
//   * there is no `done`, so the block *cannot* treat a pass's last visit
//     specially (section 10.2 item 3 is unfalsifiable rather than merely tested),
//     and an application reads `done` from the sideband where it already is;
//   * there is no `trigger` and no request/acknowledge pair, so the block cannot
//     advance the traversal -- it cannot even see which traversal ran.
//
// It therefore has no back-pressure, and not by policy: there is no wire with
// which to stall the observer, so the observer's fixed trigger-to-visit latency
// cannot be made conditional on an application.
//
// Timing
// ------
// A visit presented in cycle k is latched at the edge that ends cycle k, so the
// pins carry it in cycle k+1 and keep carrying it. One clock, unconditionally.
//
// The interface is frozen in section 2.1, and section 10.2 item 11 obliges a
// check that nothing else appears. It is the model validation/output_engine.py
// made real; where the two disagree the document is right and one of them is a
// bug, and sim/bcmc_out_engine_test.cpp compares them cycle by cycle.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_out_engine #(
    // C's width. There is no VAL_W: this block has no column index, no modulus
    // and no value to store, so the width of N is not a parameter it can use.
    parameter IDX_W = 16,
    // The pattern width: how many rows this instance can drive.
    parameter MAX_C = 32
) (
    input  wire               clk,
    input  wire               rst,          // synchronous, active high

    //--- the observation sideband (section 2.1) ----------------------------
    input  wire [MAX_C-1:0]   column_bits_i,   // M(., column): the pattern
    input  wire               visit_valid_i,   // a column is being presented
    input  wire               valid_i,         // and a matrix still exists
    input  wire [IDX_W-1:0]   C_i,             // for the section 2.3 check only

    //--- the pins, out to GPIO / PWM ---------------------------------------
    output wire [MAX_C-1:0]   pins_o
);

    // One pattern register, one edge bit, and no other storage. Section 3.2
    // says the whole of this block's state is one register, and it is: nothing
    // here can be partially consumed, because there is nothing to consume from.
    reg [MAX_C-1:0] pattern_q;
    reg             valid_q;        // the previous cycle's valid_i

    wire valid_fell = valid_q & ~valid_i;

    //-----------------------------------------------------------------------
    // The presentation gate (section 6.2)
    //
    // Combinational, so the pins are idle in the very cycle the context
    // disappears -- no edge, no lag. With no polarity inversion in the reference
    // build (section 3.2) the idle level is zero on every pin.
    //-----------------------------------------------------------------------

    assign pins_o = valid_i ? pattern_q : {MAX_C{1'b0}};

    //-----------------------------------------------------------------------
    // The edge (section 6.5, in its own precedence order)
    //
    // `valid_q` tracks unconditionally: it observes an input, and is not part of
    // the precedence. The three `pattern_q` rows are, and the order is the
    // specification's -- reset beats a visit, and so does a falling `valid`. A
    // visit cannot coincide with a falling `valid` (the observer gates
    // `visit_valid` on `valid`, section 3.7), but `rst` can coincide with a
    // visit, and it must win.
    //-----------------------------------------------------------------------

    always @(posedge clk) begin
        valid_q <= valid_i;

        if (rst) begin
            pattern_q <= {MAX_C{1'b0}};
        end else if (valid_fell) begin
            pattern_q <= {MAX_C{1'b0}};
        end else if (visit_valid_i) begin
            pattern_q <= column_bits_i;
        end
    end

`ifndef SYNTHESIS

    //-----------------------------------------------------------------------
    // Two promises, checked rather than re-enforced
    //
    // Neither of these is a behaviour of this block; both are promises made
    // *upstream* of it. The temptation is to enforce them here -- to mask the
    // pattern, or to gate the latch on `valid` -- and section 2.3 explains why
    // that is the wrong move: a mask would make this design correct by
    // forgetting, and a second gate would hide a broken observer. An alarm is
    // the honest response to a promise, and it is cheap.
    //
    // An `always @(posedge clk)` checker is safe here, unlike in
    // rtl/bcmc_cell.v: this is a registered consumer, so its inputs are settled
    // by the time the edge arrives and the checker cannot fire mid-transition.
    //-----------------------------------------------------------------------

    initial begin
        if (MAX_C < 1) begin
            $display("bcmc_out_engine: ERROR MAX_C = %0d (MAX_C >= 1 required)",
                     MAX_C);
            $stop;
        end
        if (IDX_W < 1) begin
            $display("bcmc_out_engine: ERROR IDX_W = %0d (IDX_W >= 1 required)",
                     IDX_W);
            $stop;
        end
    end

    always @(posedge clk) begin
        if (!rst) begin
            // Section 2.3: a lane at or above C is never active. It is true by
            // construction in rtl/bcmc_column.v, which forces lane_weight to
            // zero for i >= C, and this block does no masking -- so the promise
            // is checked instead of trusted. C is an input for no other purpose.
            //
            // "No bit at or above C" is one logical shift: everything from lane C
            // upward must be gone once it is shifted out. C = 0 requires the
            // whole word to be zero, which is right -- C = 0 is an empty matrix
            // -- and C >= MAX_C shifts the word away entirely, which is right
            // too.
            //
            // Only while a matrix exists: outside a valid context the sideband
            // may be stale, and an alarm that fires on stale inputs is one people
            // learn to ignore.
            if (valid_i && ((column_bits_i >> C_i) != {MAX_C{1'b0}})) begin
                $display("bcmc_out_engine: ERROR a lane above C is active (%0d)",
                         C_i);
                $stop;
            end

            // Section 3.7: the observer never presents a visit with the context
            // invalid. This block does not re-enforce it -- a second gate here
            // would hide a broken observer rather than report one -- but it is
            // the reason no such gate is needed.
            if (visit_valid_i && !valid_i) begin
                $display("bcmc_out_engine: ERROR visit with invalid context");
                $stop;
            end
        end
    end

`endif

endmodule

`default_nettype wire
