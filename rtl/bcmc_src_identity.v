//===========================================================================
// bcmc_src_identity.v -- the identity traversal source (v2.0c)
//
//     ts_t  ->  ts_pi = ts_t
//
// The control case: the traversal that visits 0, 1, 2, ... in order. It is the
// wiring the v2.0a suites are re-run through, and that is a load-bearing job
// rather than a tidy one -- a build that selects the identity is, at the seam,
// exactly the v2.0a engine, so "the seam held" becomes a measurement.
//
// Why there is no clock, no reset and no `ready` here
// --------------------------------------------------
// A function of a function's argument needs no time. This module is one `assign`
// and there is nothing in it that could be late, so it declares no `clk`, no
// `rst`, no `N`, no `seed`, no `load` and no `ready` -- section 8.1's table, which
// is the same statement `rtl/bcmc_cell.v` makes by having no `clk` port. Giving
// it the affine's interface "so the window's mux is easier" would leave five
// unused inputs and force a lint waiver to hide them.
//
// That has a consequence for readiness, and it is worth stating rather than
// discovering: section 7.1 says a seed or selector write costs the identity one
// cycle of not-ready, and a source with no clock cannot count a cycle. The
// recovery is therefore the **window's**, not this module's -- which is also why
// section 7.2's rule can be uniform at all. `rtl/bcmc_obs_wb.v` owns the
// load-recovery latch and ANDs it with each source's own `ready`; the identity's
// contribution to that AND is a constant 1. See section 7.1's correction.
//
// `ts_t` is always a legal step, so no `N` is needed: the engine drives
// `ts_t = t_next`, and `t_next` is in `0 .. N-1` by construction (and 0 while
// it is IDLE, per section 8.2). `ts_pi` is therefore also in `0 .. N-1`.
//
// Where "the presented column equals the cursor" now belongs
// ---------------------------------------------------------
// `rtl/bcmc_observer.v` currently asserts `col_q == t_q` while it is in RUN,
// commented as "the one line a v2.0c source will legitimately change". That
// assertion is only true *because* the identity is what is wired to the seam: it
// is the identity-specific corollary of the general invariant
//
//     col_q == ts_pi
//
// which holds for every source. So the assertion does not move into this module
// -- this module cannot see a cursor, a clock or a column -- it moves into the
// engine as the general form, at the same moment the seam is filled (section
// 8.2). Until then the specific form is correct and stays where it is.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_src_identity #(
    // N, the step index and the column index all fit in VAL_W bits.
    parameter VAL_W = 16
) (
    input  wire [VAL_W-1:0] ts_t,      // the step the engine is asking about
    output wire [VAL_W-1:0] ts_pi      // pi(ts_t), combinational, always
);

    assign ts_pi = ts_t;

`ifndef SYNTHESIS
    initial begin
        if (VAL_W < 1) begin
            $display("bcmc_src_identity: ERROR VAL_W = %0d (VAL_W >= 1 required)",
                     VAL_W);
            $stop;
        end
    end
`endif

endmodule

`default_nettype wire
