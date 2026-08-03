//===========================================================================
// bcmc_cell.v -- BCMC Cell: the characteristic function, one matrix element
//
//     (weight, offset, column)  ->  bit
//
// This module is the whole of the BCMC Evaluator's mathematics. Everything
// else in the Evaluator -- rows, columns, the matrix itself -- is replication
// of this module and introduces no new mathematics whatsoever.
//
// The definition, straight from docs/Proof.md:
//
//     M(i,j) = 1   iff   ((j - offset[i]) mod N) < weight[i]
//
// Note what is absent, and deliberately so:
//
//   * no clk, no rst          the characteristic function is pointwise, so
//                             the hardware is combinational
//   * no state, no FSM        M(i,j) depends on nothing but its arguments
//   * no row index i          the caller supplies row i's (weight, offset)
//                             pair; the cell never indexes an array
//   * no assertions           see "Preconditions" below
//
// The Core is sequential because a prefix sum is recursive. This module is
// combinational for the mirror-image reason. See "Stateful Core, Stateless
// Evaluator" in docs/Hardware_Architecture.md.
//
// Preconditions
// -------------
//     N >= 1
//     0 <= weight <= N
//     0 <= offset <  N
//     0 <= column <  N
//
// These are preconditions, not behaviours: exactly as the theorem assumes
// 0 <= w_i <= N without enforcing it, this module computes `bit` for whatever
// inputs it is given. Outside the preconditions the result is meaningless but
// still defined.
//
// They are checked in sim/bcmc_cell_test.cpp and sim/tb_cell.v, and NOT here.
// A combinational module has no clock edge on which to check safely: an
// `always @*` checker re-evaluates on each individual input change, so during
// a legitimate transition it can momentarily observe an inconsistent
// combination such as `column >= N` and abort a correct simulation. The
// testbench owns the preconditions because only the testbench knows when the
// inputs are settled.
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_cell #(
    // N, the weight, the offset and the column index all fit in VAL_W bits.
    parameter VAL_W = 16
) (
    input  wire [VAL_W-1:0] N,        // row length, N >= 1
    input  wire [VAL_W-1:0] weight,   // row weight,    0 <= weight <= N
    input  wire [VAL_W-1:0] offset,   // row offset,    0 <= offset <  N
    input  wire [VAL_W-1:0] column,   // column index,  0 <= column <  N

    output wire             bit_out   // M(row, column)
);

    //-----------------------------------------------------------------------
    // The Wrap is not a Division
    //
    // Because 0 <= column < N and 0 <= offset < N,
    //
    //     -N < column - offset < N
    //
    // so reducing modulo N needs one comparison and one addition. No divider,
    // and no modulo operator, is ever required.
    //
    // This is the exact mirror of the Core's reduction. The Core wraps
    // forwards, computing offset + weight, which may overshoot N, and so
    // conditionally SUBTRACTS N. The Evaluator wraps backwards, computing
    // column - offset, which may undershoot 0, and so conditionally ADDS N.
    // Both facts come from the same hypothesis of Lemma 2.
    //-----------------------------------------------------------------------

    wire wraps = (column < offset);

    // Both branches are computed on VAL_W bits without overflow:
    //
    //   ordinary case  column >= offset  =>  0 <= column - offset < N
    //   wrapped case   column <  offset  =>  N - offset <= N, and
    //                                        column + (N - offset) < N
    //
    // so `delta` is always a true residue in [0, N-1]. Grouping the wrapped
    // case as column + (N - offset) rather than (column - offset) + N keeps
    // every intermediate value below N, which is why VAL_W bits suffice.
    wire [VAL_W-1:0] delta = wraps ? (column + (N - offset))
                                   : (column - offset);

    //-----------------------------------------------------------------------
    // The characteristic function itself.
    //
    // delta is in [0, N-1] and weight is in [0, N], so:
    //
    //   weight = 0  =>  bit is always 0   (an empty row)
    //   weight = N  =>  bit is always 1   (a full row)
    //
    // Both fall out of the comparison. Neither is a special case.
    //-----------------------------------------------------------------------

    assign bit_out = (delta < weight);

endmodule

`default_nettype wire
