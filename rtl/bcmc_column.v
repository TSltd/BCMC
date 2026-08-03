//===========================================================================
// bcmc_column.v -- BCMC Column: one column of the matrix, all rows at once
//
//     (weights[], offsets[], column)  ->  bits[0 .. C-1]
//
// This module introduces NO NEW MATHEMATICS. It is MAX_C instances of
// bcmc_cell with a different row's (weight, offset) pair in each. That is the
// entire design; there is no arithmetic here that is not inside a cell.
//
//     column_bits[i] = bcmc_cell(N, weights[i], offsets[i], column)
//
// which is exactly the "column projection" of the characteristic function: the
// column index is bound and the row runs free. It is the mirror image of
// bcmc_row, and neither is privileged -- see "Projections of the
// Characteristic Function" in docs/Hardware_Architecture.md.
//
// The replication claim is not merely asserted. sim/bcmc_column_test.cpp
// compares every bit of this module against a separately instantiated
// bcmc_cell, for every row of every case, so `bcmc_cell` is demonstrably the
// primitive and this module is demonstrably nothing but copies of it.
//
// This module exists primarily for simulation and mathematical verification.
// Large hardware implementations should instantiate `bcmc_cell` directly
// according to the application's traversal strategy. It is, however, the
// projection an allocator usually wants: a column is one scheduling slot, and
// column_bits is the set of consumers served in that slot. The Balance Theorem
// is a statement about the popcount of exactly this output.
//
// Bit ordering
// ------------
//     column_bits[0] is row 0. column_bits[i] is row i.
//
// Array ports
// -----------
// Verilog-2005 has no array ports, so weights[] and offsets[] arrive as flat
// vectors, row i occupying bits [VAL_W*i +: VAL_W] -- row 0 in the least
// significant field, matching the bit ordering above.
//
// Lanes above C
// -------------
// MAX_C is a synthesis-time bound; C is a runtime value and may be smaller.
// Rows i >= C do not exist in the matrix, so the corresponding lanes are not
// switched off -- they are asked a question whose answer is zero, exactly as in
// bcmc_row: a cell with weight = 0 outputs 0 for every column, and weight = 0
// is legal for every N. Every cell in this module therefore receives a query
// that satisfies the cell's preconditions, and no output masking is needed.
//
// Preconditions (inherited from bcmc_cell, checked by the testbenches)
// -------------------------------------------------------------------
//     N >= 1
//     0 <= C <= MAX_C
//     0 <= column < N
//     0 <= weights[i] <= N     for every i < C
//     0 <= offsets[i] <  N     for every i < C
//
// Generic Verilog-2005. Nothing here is specific to any FPGA family.
//===========================================================================

`default_nettype none

module bcmc_column #(
    // N, the weights, the offsets and the column index all fit in VAL_W bits.
    parameter VAL_W = 16,
    // C fits in IDX_W bits.
    parameter IDX_W = 16,
    // The most rows this instance can hold.
    parameter MAX_C = 16
) (
    input  wire [VAL_W-1:0]       N,             // row length, N >= 1
    input  wire [IDX_W-1:0]       C,             // rows in use, C <= MAX_C
    input  wire [VAL_W-1:0]       column,        // 0 <= column < N

    // Row i occupies bits [VAL_W*i +: VAL_W] of each vector.
    input  wire [MAX_C*VAL_W-1:0] weights_flat,
    input  wire [MAX_C*VAL_W-1:0] offsets_flat,

    output wire [MAX_C-1:0]       column_bits    // column_bits[i] = M(i, column)
);

    genvar i;

    generate
        for (i = 0; i < MAX_C; i = i + 1) begin : g_row
            // The row index this lane answers for, as an IDX_W-bit constant.
            localparam [IDX_W-1:0] ROW = i;

            // Row i exists only if i < C. The comparison is against the runtime
            // C, so it is real logic -- one more reason this module is a
            // convenience for verification rather than a recommended shape for
            // large hardware.
            wire active = (ROW < C);

            wire [VAL_W-1:0] row_weight = weights_flat[VAL_W*i +: VAL_W];
            wire [VAL_W-1:0] row_offset = offsets_flat[VAL_W*i +: VAL_W];

            wire [VAL_W-1:0] lane_weight = active ? row_weight : {VAL_W{1'b0}};
            wire [VAL_W-1:0] lane_offset = active ? row_offset : {VAL_W{1'b0}};

            bcmc_cell #(
                .VAL_W (VAL_W)
            ) u_cell (
                .N       (N),
                .weight  (lane_weight),
                .offset  (lane_offset),
                .column  (column),
                .bit_out (column_bits[i])
            );
        end
    endgenerate

endmodule

`default_nettype wire
