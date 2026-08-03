//===========================================================================
// bcmc_row.v -- BCMC Row: one row of the matrix, all columns at once
//
//     (weight, offset)  ->  bits[0 .. N-1]
//
// This module introduces NO NEW MATHEMATICS. It is MAX_N instances of
// bcmc_cell with the column index held at a different constant in each. That
// is the entire design; there is no arithmetic here that is not inside a cell.
//
//     row_bits[j] = bcmc_cell(N, weight, offset, j)
//
// which is exactly the "row projection" of the characteristic function: the
// row's (weight, offset) pair is bound and the column index runs free. See
// "Projections of the Characteristic Function" in docs/Hardware_Architecture.md.
//
// The replication claim is not merely asserted. sim/bcmc_row_test.cpp compares
// every bit of this module against a separately instantiated bcmc_cell, for
// every column of every case, so `bcmc_cell` is demonstrably the primitive and
// this module is demonstrably nothing but copies of it.
//
// This module exists primarily for simulation and mathematical verification.
// Large hardware implementations should instantiate `bcmc_cell` directly
// according to the application's traversal strategy: a scheduler that visits
// one column per tick needs one cell, not N of them, and MAX_N cells is the
// most expensive way to answer a question about a single column.
//
// Bit ordering
// ------------
//     row_bits[0] is column 0. row_bits[j] is column j.
//
// Lanes above N
// -------------
// MAX_N is a synthesis-time bound; N is a runtime value and may be smaller.
// Columns j >= N do not exist in the matrix, so the corresponding lanes are not
// switched off -- they are asked a question whose answer is zero. A cell with
// weight = 0 outputs 0 for every column, and weight = 0 is legal for every N,
// so the inactive lanes are fed (weight = 0, column = 0). Every cell in this
// module therefore receives a query that satisfies the cell's preconditions,
// and no output masking logic is needed.
//
// Preconditions (inherited from bcmc_cell, checked by the testbenches)
// -------------------------------------------------------------------
//     1 <= N <= MAX_N
//     0 <= weight <= N
//     0 <= offset <  N
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_row #(
    // N, the weight and the offset all fit in VAL_W bits.
    parameter VAL_W = 16,
    // The widest row this instance can hold: MAX_N columns.
    parameter MAX_N = 16
) (
    input  wire [VAL_W-1:0] N,         // row length, 1 <= N <= MAX_N
    input  wire [VAL_W-1:0] weight,    // row weight,  0 <= weight <= N
    input  wire [VAL_W-1:0] offset,    // row offset,  0 <= offset <  N

    output wire [MAX_N-1:0] row_bits   // row_bits[j] = M(row, j)
);

    genvar j;

    generate
        for (j = 0; j < MAX_N; j = j + 1) begin : g_col
            // The column index this lane asks about, as a VAL_W-bit constant.
            localparam [VAL_W-1:0] COL = j;

            // Column j exists only if j < N. The comparison is against the
            // runtime N, so it is real logic -- one more reason this module is
            // a convenience for verification rather than a recommended shape
            // for large hardware.
            wire active = (COL < N);

            wire [VAL_W-1:0] lane_weight = active ? weight : {VAL_W{1'b0}};
            wire [VAL_W-1:0] lane_column = active ? COL : {VAL_W{1'b0}};

            bcmc_cell #(
                .VAL_W (VAL_W)
            ) u_cell (
                .N       (N),
                .weight  (lane_weight),
                .offset  (offset),
                .column  (lane_column),
                .bit_out (row_bits[j])
            );
        end
    endgenerate

endmodule

`default_nettype wire
